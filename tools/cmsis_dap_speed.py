#!/usr/bin/env python3
"""cmsis_dap_speed.py —— 测 CMSIS-DAP 探针的**批量**读写速度（默认 16 KB SRAM）

为什么不用 pyOCD 自带的 commander：
  · commander 的 `read32 addr n` 是「发一条命令等一条」，量不出批量路径的真实速度；
  · 而且它**不校验读回来的数据** —— 速度再快，数据是错的也没意义。

所以本脚本的顺序是：
  ① 写入已知图案 → ② 块读回来**逐字节比对**（先证明数据是对的）→ ③ 才计时。

需要 pyOCD（本机装在独立 venv：%USERPROFILE%\\.venvs\\pyocd）。
**用哪个 python 跑都行**：脚本发现当前解释器没有 pyOCD 会自动换成 venv 的解释器重跑。

用法：
    python tools\\cmsis_dap_speed.py                             # 默认：0x20000000 读/写 16 KB
    python tools\\cmsis_dap_speed.py --size 4096 --rounds 10
    python tools\\cmsis_dap_speed.py --freq 4000000 --target cortex_m
    python tools\\cmsis_dap_speed.py --addr 0x08000000            # 换到 Flash（只读）
"""
import argparse
import os
import statistics
import subprocess
import sys
import time

# Windows 控制台默认 GBK：输出里只要有编不出的字符就 UnicodeEncodeError 崩
# （本工作区为这个症状白查过好几次，见 AGENTS §4.9）——强制 UTF-8 + replace，并且不用 emoji。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

VENV_PY = os.path.expandvars(r"%USERPROFILE%\.venvs\pyocd\Scripts\python.exe")


def ensure_pyocd() -> None:
    """pyOCD 在本机独立 venv 里（见 AGENTS §4.11）。用别的解释器跑本脚本时自动换过去重跑，
    这样 `python tools\\cmsis_dap_speed.py` 不管用哪个 python 都能跑，不用记路径。"""
    try:
        import pyocd  # noqa: F401
        return
    except ImportError:
        pass
    if os.environ.get("_DAP_REEXEC") == "1" or not os.path.exists(VENV_PY):
        sys.exit("找不到 pyOCD。用 %s 跑本脚本，或先 pip install pyocd（见 AGENTS.md §4.11）。" % VENV_PY)
    os.environ["_DAP_REEXEC"] = "1"
    print("[i] 当前解释器没有 pyOCD，自动换成 %s 重跑" % VENV_PY, flush=True)
    raise SystemExit(subprocess.call([VENV_PY, os.path.abspath(__file__)] + sys.argv[1:]))


ensure_pyocd()

try:
    from pyocd.core.helpers import ConnectHelper
except ImportError:
    sys.exit("找不到 pyOCD（本机路径：%s）" % VENV_PY)


def make_pattern(size: int) -> bytearray:
    """非平凡图案：相邻字节不同，能抓出串位/丢字节类错误。"""
    return bytearray(((i * 37) ^ (i >> 3)) & 0xFF for i in range(size))


def est_swclk(nbytes: int, seconds: float) -> float:
    """由块传输耗时反推 SWCLK 的粗估（Hz）。

    SWD 块传输的时钟账（32 位字）：首个字 ≈ 8(头) + 3(ACK) + 1(转向) + 32(数据) + 1(校验) = 45，
    之后每个字 ≈ 32 + 1 = 33（地址自增，头/ACK 摊掉了）。只用来给个数量级。
    """
    words = nbytes / 4.0
    if seconds <= 0 or words < 1:
        return 0.0
    clocks = 45 + (words - 1) * 33
    return clocks / seconds


def bench(fn, rounds: int):
    times = []
    for _ in range(rounds):
        t0 = time.perf_counter()
        fn()
        times.append(time.perf_counter() - t0)
    return min(times), statistics.median(times)


def main() -> int:
    ap = argparse.ArgumentParser(description="CMSIS-DAP 探针批量读写速度测试")
    ap.add_argument("--target", default="stm32f103c8", help="目标型号（默认 stm32f103c8；不想装 pack 可用 cortex_m）")
    ap.add_argument("--probe", default=None, help="探针 unique id（多探针时用）")
    ap.add_argument("--freq", type=int, default=1_000_000, help="SWD 时钟 Hz（默认 1 MHz）")
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=0x20000000, help="起始地址（默认 SRAM 0x20000000）")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=16384, help="字节数（默认 16384 = 16 KB）")
    ap.add_argument("--rounds", type=int, default=5, help="每种操作重复次数，取最小值（默认 5）")
    ap.add_argument("--read-only", action="store_true", help="只测读（例如目标是 Flash）")
    args = ap.parse_args()

    print(f"目标 {args.target} / 探针 {args.probe or '自动'} / SWD {args.freq/1e6:.2f} MHz")
    session = ConnectHelper.session_with_chosen_probe(
        unique_id=args.probe, target_override=args.target, options={"frequency": args.freq})
    if session is None:
        return print("连不上探针（pyocd list 看看有没有）") or 1

    with session:
        target = session.target
        target.halt()                      # 停止取指，排除目标自己改内存
        addr, size = args.addr, args.size
        print(f"地址 0x{addr:08X} / {size} 字节 / 每种操作 {args.rounds} 轮取最快\n")

        # ---------- ① + ② 先证明数据是对的 ----------
        pattern = make_pattern(size)
        target.write_memory_block8(addr, pattern)
        got = bytearray(target.read_memory_block8(addr, size))
        bad = [i for i in range(size) if got[i] != pattern[i]]
        if bad:
            print(f"[FAIL] 数据校验失败：{len(bad)}/{size} 字节不符，第一个在偏移 {bad[0]}"
                  f"（读到 0x{got[bad[0]]:02X}，期望 0x{pattern[bad[0]]:02X}）")
            print("   速度数字没有意义，先修传输正确性。")
            return 2
        print(f"[OK] 数据校验通过：写入 {size} 字节 → 块读回来逐字节一致\n")

        # ---------- ③ 计时 ----------
        results = []
        tw_min, tw_med = bench(lambda: target.write_memory_block8(addr, pattern), args.rounds)
        results.append(("写 SRAM", tw_min, tw_med))
        tr_min, tr_med = bench(lambda: target.read_memory_block8(addr, size), args.rounds)
        results.append(("读 SRAM", tr_min, tr_med))

        print(f"{'操作':<10}{'最快':>12}{'中位':>12}{'吞吐(最快)':>14}{'≈SWCLK':>12}")
        print("-" * 62)
        for name, tmin, tmed in results:
            thr = size / tmin / 1e6                     # MB/s
            clk = est_swclk(size, tmin)
            print(f"{name:<10}{tmin*1e3:>10.2f}ms{tmed*1e3:>10.2f}ms{thr:>12.2f}MB/s{clk/1e6:>10.2f}MHz")

        print("\n注：≈SWCLK 是按 SWD 块传输的时钟账（首字 45、后续每字 33 个时钟）反推的粗估，")
        print("    只用来判断数量级；真实波形要用逻辑分析仪量 GPIO47(SWCLK)。")

        target.resume()
    return 0


if __name__ == "__main__":
    sys.exit(main())

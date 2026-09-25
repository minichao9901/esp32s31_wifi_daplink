# 测试报告 —— 无线探针实测（2026-09-25，家中网络）

> 这份是**分享前重新走通一遍流程**的记录：链路自检 → 16KB 读写 + 逐字节校验 →
> 探针侧统计 → 裸客户端往返延迟。所有数字都是真机跑出来的，命令和原始输出都贴在下面。

## 0. 测试环境

| | |
|---|---|
| 探针 | ESP32-S31-Function-CoreBoard-1，本工程固件；WiFi **STA** 接 `minisz`，**rssi −67 ~ −77**，IP `192.168.1.48:4441` |
| 主机 | Windows PC，`MediaTek Wi-Fi 6 MT7921`，源地址 `192.168.1.9` —— **走 WLAN**，所以是**两端都无线**（经 AP 中转两次） |
| 目标 | STM32F103，SWCLK=**J2-13**(GPIO47)、SWDIO=**J2-14**(GPIO48)、GND=**J2-8**（空片，没烧过程序） |
| 客户端 | ESP-IDF 自带 **OpenOCD 0.12**，`cmsis-dap backend tcp`（`min_timeout` 除扫描那节外都用 10 ms） |

> 📌 **和 §1 历史数字（2026-09-21）的差别**：那组是办公室网络、主机有线；
> 这次在家里、**两端都无线**且信号一般（rssi −67~−77）。所以下面的数字**明显慢一些**，
> 这正是无线探针的固有特性 —— 看数字前先看链路。

---

## 1. 链路自检（`scripts/dap-wifi.ps1`）

```powershell
pwsh -File scripts\dap-wifi.ps1 -ProbeIp 192.168.1.48
```

```
== 无线探针自检：192.168.1.48:4441 ==
TCP 192.168.1.48:4441 通 ✓

---- OpenOCD 关键输出 ----
  Info : CMSIS-DAP: SWD supported
  Info : CMSIS-DAP: FW Version = 2.1.2
  Info : CMSIS-DAP: Serial# = 1C2904D07E1E
  Info : CMSIS-DAP: Interface Initialised (SWD)
  Info : CMSIS-DAP: Interface ready
  Info : SWD DPIDR 0x1ba01477
  Info : [stm32f1x.cpu] Cortex-M3 r1p1 processor detected
  0xe0042000: 20036410
  pc (/32): 0xfffffffe

[PASS] 无线链路通：识别到目标、读到了 DBGMCU IDCODE
       测速：pwsh -File scripts\dap-speed.ps1 -Tool openocd -Backend tcp -HostIp 192.168.1.48 -Port 4441
```

判读：`DPIDR 0x1ba01477` + `Cortex-M3 r1p1` = SWD 物理层通了；
`0xE0042000 = 0x20036410` = STM32F103 的 DBGMCU_IDCODE（0x410 = 中容量 F103）；
`pc = 0xfffffffe` 是**空片**的正常值（没烧过程序）。**PASS**。

---

## 2. 16KB SRAM 读写 + 逐字节校验（`scripts/dap-speed.ps1 -Backend tcp`）

```powershell
pwsh -File scripts\dap-speed.ps1 -Tool openocd -Backend tcp -HostIp 192.168.1.48 -Size 16384 -Rounds 3
```

**同一个固件、同一个目标，连跑三次差 2 倍** —— 这就是无线链路：

| 轮次 | 读最快 | 读中位 | 写最快 | 写中位 | 逐字节校验 |
|---|---|---|---|---|---|
| 第 1 次 | 282.23 ms（58 kB/s） | 360.10 ms | 207.65 ms（79 kB/s） | 248.95 ms | ✅ **PASS** |
| 第 2 次 | **161.17 ms（102 kB/s）** | 191.70 ms | **111.27 ms（147 kB/s）** | 157.58 ms | ✅ **PASS** |
| 第 3 次（用**本仓库自带脚本**跑，分享前最终验证） | **133.88 ms（122 kB/s）** | 206.91 ms | 124.36 ms（132 kB/s） | 141.38 ms | ✅ **PASS** |

```
=========== 数据校验（写图案 → 读回 → 逐字节比对）===========
[PASS] 写入 16384 字节 → 读回逐字节一致
```

> 结论：**功能是对的**（16KB 写进去再读回来逐字节一致，三次全 PASS），
> 速度看"最快"那列、并且**至少跑 2~3 轮**（刚跑过别的客户端时第一轮常明显偏慢）。

### 2.1 `min_timeout` 扫描（16KB，2 轮取最快）

`min_timeout` 是这份 OpenOCD fork 的 TCP 后端"每收一条命令先睡多久"的参数：

| `min_timeout` | 读最快 | 写最快 | `init+halt+shutdown` 基线 |
|---|---|---|---|
| 0 ms | 231.36 ms | 204.55 ms | 1.739 s |
| 2 ms | 248.21 ms | 323.37 ms | 1.658 s |
| 5 ms | 281.62 ms | 188.91 ms | 1.718 s |
| 10 ms | 240.10 ms | 213.73 ms | 1.721 s |
| 20 ms | 258.66 ms | 209.80 ms | **2.719 s** |

⇒ **批量传输基本不吃 `min_timeout`**（231~282 ms 之间随机波动，被链路抖动盖住）；
但**零碎命令主导的会话**（init/halt/shutdown 一百多条命令）会**线性挨罚**：
20 ms 时基线从 1.7 s 涨到 2.7 s。
局域网用 **10 ms**，想更快用 0~5 ms（网络差再往上调）。

---

## 3. 探针侧统计（最有说服力的一组）

固件在**每次会话结束时**自己打一行统计（`usj_watch.py` 挂着串口抓的，不干扰测试）：

```
OpenOCD 已连接：192.168.1.9:62972
会话结束：84 条命令 / 收 1722 B / 发 16808 B / SWD 侧均值 175 us / 最大 882 us / 共 1.5 s   ← 16KB 读
OpenOCD 已连接：192.168.1.9:62975
会话结束：81 条命令 / 收 17870 B / 发 400 B  / SWD 侧均值 174 us / 最大 808 us / 共 1.7 s   ← 16KB 写
```

★ **一次 16KB 读**：探针侧 SWD 总共只花 `84 × 175 µs ≈ **14.7 ms**`，
而主机侧看到的读数时间是 **161~282 ms**、整个 OpenOCD 会话 **1.5~1.8 s**。

⇒ **瓶颈在网络往返，不在探针** —— 探针侧只占 ~1%。想更快只有两条路：
降低往返（PC 走有线 / 换更好的 AP / 5GHz）或每条命令多搬点（已用满 1024 B 包）。

---

## 4. 裸客户端往返延迟（`tools/dap_tcp_probe.py`，脱离 OpenOCD）

```powershell
python tools\dap_tcp_probe.py 192.168.1.48
```

```
读 DP IDCODE   OK    0.1 ms   05 01 01 77 14 a0 1b
读 DP IDCODE   OK    3.9 ms   05 01 01 77 14 a0 1b
读 DP IDCODE   OK 1467.6 ms   05 01 01 77 14 a0 1b
读 DP IDCODE   OK 1500.9 ms   05 01 01 77 14 a0 1b
...
20 次往返共 7426 ms → 每次 371.30 ms
```

**链路是双峰的**：

| 分布 | 耗时 | 说明 |
|---|---|---|
| 大多数命令 | **0.0 ~ 0.2 ms** | 探针响应极快（SWD 一次事务才几十 µs） |
| 周期性 | **~1450 ~ 1500 ms** | ← 这种卡顿就是批量传输波动的来源 |
| 偶发 | 19 / 57 ms | 一般的重传 |

⇒ 每次往返的"中位"是亚毫秒级，但**平均值被周期性 1.5 s 卡顿拖到 371 ms**。
两端都走无线（PC 也是 WLAN）+ rssi −67~−77 是主因；PC 改有线应能显著改善。

> 顺带排除：固件**已经关掉了 WiFi 省电**（`esp_wifi_set_ps(WIFI_PS_NONE)`），
> 所以这不是 modem-sleep 造成的 —— 是链路本身。

---

## 5. 怎么复现这套测试

```powershell
# ① 配好 WiFi（sdkconfig 里 CONFIG_CHERRYDAP_WIFI_SSID/PASSWORD；留空则自开热点）
idf.py --preview -p COM43 flash monitor        # 日志里会打：拿到 IP：192.168.x.x

# ② 一把过自检
pwsh -File scripts\dap-wifi.ps1 -ProbeIp 192.168.x.x

# ③ 16KB 测速 + 逐字节校验
pwsh -File scripts\dap-speed.ps1 -Tool openocd -Backend tcp -HostIp 192.168.x.x -Size 16384 -Rounds 3

# ④ 裸客户端看往返延迟分布（可选，最能看出链路质量）
python tools\dap_tcp_probe.py 192.168.x.x
```

参数速查：`-MinTimeout`（只 `openocd_speed.ps1` 有，默认 10）、`-Size`（默认 16384）、`-Rounds`（默认 3）。

---

## 6. 结论

| 项 | 结果 |
|---|---|
| SWD 物理层 / 识别目标 | ✅ PASS（DPIDR 0x1ba01477、Cortex-M3、DBGMCU 0x20036410） |
| 16KB 写→读→逐字节比对 | ✅ PASS（每次跑都是 0 字节错） |
| 16KB 读 / 写耗时（家，两端无线 rssi −67~−77） | 读 **134~282 ms** / 写 **111~208 ms**（122~58 / 147~79 kB/s） |
| 探针侧 SWD 占比 | **~1%**（84 条命令 × 175 µs ≈ 14.7 ms） |
| 瓶颈 | 无线往返（裸客户端平均 371 ms/次，周期性 1.5 s 卡顿） |
| 结论 | **功能全通、数据可信；速度由链路决定，不是探针的问题** |

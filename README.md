# cherrydap_s31_wifi —— ESP32-S31 **无线** CMSIS-DAP 探针（WiFi + TCP）

把 S31 变成一个**不用插 USB 的 SWD 调试探针**：

```
   电脑                                          目标板（实测用 STM32F103）
   OpenOCD ──WiFi/TCP(:4441)──> ESP32-S31 ──SWD(17~22MHz)──> SWCLK/SWDIO/GND
      `cmsis-dap backend tcp`     本固件                  J2-13 / J2-14 / J2-8
```

SWD 位时序层**整套复用 `cherrydap_s31_fast`**（dedicated GPIO + 无分支展开汇编，
一个字没改）；本工程只把**传输层**从 USB bulk 换成 WiFi 上的 TCP。
所以"探针的手感"和有线版完全一样（DPIDR/IDCODE/halt/内存读写全对），只是线变长了。

> 参考实现：[`bkuschak/cmsis_dap_tcp_esp32`](https://github.com/bkuschak/cmsis_dap_tcp_esp32)
> （ESP32 跑 CMSIS-DAP over TCP）。协议本身已经进 **OpenOCD 上游**：
> `src/jtag/drivers/cmsis_dap_tcp.c` + `interface/cmsis-dap-tcp.cfg`。

---

## 1. 实测数字（2026-09-21，目标 STM32F103，ESP-IDF 自带 OpenOCD 0.12）

| 项目 | 无线（本工程） | 有线（`cherrydap_s31_fast`，USB-HS） | 参考实现（ESP32-S3 @240MHz） |
|---|---|---|---|
| `init;halt` 到识别目标 | **1.3~1.4 s** | ~2 s（多数时间在 OpenOCD 自己的重试上） | — |
| 16 KB **读** | **85~90 ms → 183~192 kB/s** | 13.59 ms → 1.21 MB/s | ~80 kB/s |
| 16 KB **写** | **76~81 ms → 201~216 kB/s** | 13.27 ms → 1.23 MB/s | ~200 kB/s |
| 数据校验 | ✅ **写进去再读回来逐字节一致** | ✅ | — |
| 裸客户端单条命令往返 | **3.3~4.2 ms** | ~0.1 ms | — |

**无线慢在哪：往返延迟，不是探针。** 16 KB 读要 ~17 条命令（每条 1024 B 包 = 255 字），
每条命令探针侧只花 ~770 µs 做 SWD，剩下 4~6 ms 全在 WiFi 上飞
（`make dap-wifi-speed` 的日志里能看到"SWD 侧均值 xx µs"）。
→ 想更快只有两条路：**降低往返**（换 5GHz/更好的 AP）或**每条命令多搬点**（已用满 1024 B 包）。

> 📌 **2026-09-25 复测（分享前重走一遍流程）**：家里网络、**两端都无线**（PC 也走 WLAN）、
> rssi −67~−77 的条件下，16 KB 读 **161~282 ms**、写 **111~208 ms**，
> **逐字节校验每次 PASS**；探针侧 SWD 只占 **~1%**（84 条命令 × 175 µs ≈ 14.7 ms）。
> 完整数据（含 `min_timeout` 扫描、往返延迟的双峰分布、探针侧统计原始日志）
> → **[TEST-REPORT.md](TEST-REPORT.md)**。
> 对比上面那行 85~90 ms 能看出：**无线探针的速度强依赖链路**（那组是办公室网络 + 主机有线）。

---

## 2. 怎么用

> 本仓库**自带测试脚本**（`scripts/` + `tools/`），clone 下来就能跑整套流程；
> 实测数据与原始输出见 **[TEST-REPORT.md](TEST-REPORT.md)**。

```powershell
# ① 先配 WiFi（改本工程的 sdkconfig —— 见下面「sdkconfig 与凭据」）
#    想接自家路由器（推荐：电脑不用切网、互联网不受影响）：
#        CONFIG_CHERRYDAP_WIFI_SSID="你的SSID"
#        CONFIG_CHERRYDAP_WIFI_PASSWORD="你的密码"
#    留空则探针自开热点 CherryDAP-S31（192.168.4.1，密码在 CONFIG_CHERRYDAP_AP_PASSWORD）

# ② 编译 + 烧录（走 USB-DBG 口，和 WiFi 互不干扰）
idf.py --preview -p COM43 build flash monitor
#   ⚠️ esp32s31 在 IDF 6.1 里是 preview target，所有 idf.py 调用都要带 --preview
#   串口日志会打出两行关键信息：
#     I cherrydap-wifi: 拿到 IP：192.168.1.48（探针地址 = 192.168.1.48:4441）
#     I cherrydap-wifi: OpenOCD 这样连：-c "adapter driver cmsis-dap" -c "cmsis-dap backend tcp" ...

# ③ 一把过自检（TCP 通不通 + 认不认目标 + IDCODE/halt/reg pc）
pwsh -File scripts\dap-wifi.ps1 -ProbeIp 192.168.1.48

# ④ 测速 + 逐字节校验（16 KB SRAM）
pwsh -File scripts\dap-speed.ps1 -Tool openocd -Backend tcp -HostIp 192.168.1.48 -Size 16384 -Rounds 3

# ⑤（可选）裸客户端看往返延迟分布 —— 最能看出链路质量
python tools\dap_tcp_probe.py 192.168.1.48
```

> 在作者的工作区里这几个脚本还有 make 封装（`make run PROJ=cherrydap_s31_wifi` /
> `make dap-wifi WIFI_HOST=...` / `make dap-wifi-speed WIFI_HOST=...`）；
> 本仓库直接用上面的 `pwsh -File` 即可。

手动跑 OpenOCD（等价于 ③ 展开）：

```powershell
$ocd = "$env:USERPROFILE\.espressif\tools\openocd-esp32\v0.12.0-esp32-*\openocd-esp32\bin\openocd.exe"
$s   = "...\openocd-esp32\share\openocd\scripts"
& $ocd -s $s -c "adapter driver cmsis-dap" -c "transport select swd" `
       -c "cmsis-dap backend tcp" -c "cmsis-dap tcp host 192.168.1.48" `
       -c "cmsis-dap tcp port 4441" -c "cmsis-dap tcp min_timeout 10" `
       -f "$s\target\stm32f1x.cfg" -c "init" -c "halt" -c "mdw 0xE0042000" -c "reg pc"
```

**接线**（和有线版共用同一对脚，别改）：SWCLK=**GPIO47/J2-13**、SWDIO=**GPIO48/J2-14**、
GND=**J2-8**、nRESET=GPIO46/J2-16（开漏，可不接）。三个脚都不是 strapping 脚。

### sdkconfig 与凭据（⚠️ 本工程的 `sdkconfig` 里**有 WiFi 密码**）

**2026-09-22 起，工作区所有工程的 `sdkconfig` 都进 git 了**（`.gitignore` 不再排除
`projects/*/sdkconfig`；在这之前只有本工程是例外）—— 换台机器 `git pull` 就能直接 `make run`，
不必照 `sdkconfig.defaults` 重配，也不必重跑 menuconfig。
本工程是**唯一还留了例外**的一个：它有 WiFi 凭据，提交的那份必须是占位符。
（换机器后把占位符改成自己的 SSID/密码再编译；不改也能跑 —— 连不上会自动退回自开热点。）

**提交进 git 的那份凭据是占位符**：

```
CONFIG_CHERRYDAP_WIFI_SSID="YOUR_WIFI_SSID"
CONFIG_CHERRYDAP_WIFI_PASSWORD="YOUR_WIFI_PASSWORD"
```

**本机工作区那份是真凭据**，靠 `skip-worktree` 让 git 不去管它：

```powershell
git ls-files -v projects/cherrydap_s31_wifi/sdkconfig   # 首列是 S = skip-worktree

# 想正常改它（比如换 WiFi），先摘掉标记：
git update-index --no-skip-worktree projects/cherrydap_s31_wifi/sdkconfig
#   …改完记得把凭据改回占位符再提交，然后重新戴上：
git update-index --skip-worktree    projects/cherrydap_s31_wifi/sdkconfig
```

- 🚨 **这个标记会悄悄消失 —— 别信它，要验它**：2026-09-22 整理 sdkconfig 时发现它已经没了
  （`git ls-files -v` 首列变成 `H`，`git status` 里这个文件显示为"已修改"）——
  那一刻真密码处于"**一次 `git add -A` 就会进库并推上去**"的状态，
  偏偏上面这句"已实测不会带进来"会让人以为它是安全的。
  已知会清掉它的操作：`git reset --hard`、`git checkout <commit> -- <path>`、`git stash`。
  → **动 git 之前先跑一次上面那条 `git ls-files -v`**；首列不是 `S` 就立刻补
  `git update-index --skip-worktree <文件>`（本机文件内容不受影响）。
  ✅ 已核对历史：真凭据**从未**进过任何提交（`git log --all -S '<密码>'` 为空）。
- ✅ 好处：`git status` 干净、`git add -A` 不会把真密码带进来（**前提是标记还在**）。
- ⚠️ 代价：`git reset --hard` / `git checkout -- <这个文件>` 会用提交里的**占位符**覆盖本机那份
  —— 真凭据自己留个备份。
- 📌 想确认提交里是什么：`git show HEAD:projects/cherrydap_s31_wifi/sdkconfig | findstr CHERRYDAP`。
- 📌 **根治办法（还没做）**：把 SSID/密码从 Kconfig 挪进一个**被 gitignore 的头文件**
  （`#include "wifi_creds.h"`，文件不存在就编成空串 = 自动退回热点模式）——
  这样 `sdkconfig` 就能原样进库，再没有"标记消失"这类隐患。

---

## 3. 协议（8 字节头 + 标准 CMSIS-DAP 载荷）

主机先连 TCP，然后一问一答（全部小端）：

```
  44 41 50 00 | len_lo len_hi | type | 00 | payload...
  └─ "DAP" ─┘   └─ 载荷长度 ─┘  0x01=请求
                                0x02=响应
```

`payload` 就是**和 USB 版逐字节相同**的 CMSIS-DAP 命令/响应
（`00 F0` = DAP_Info(Capabilities)、`05 ..` = DAP_Transfer、`06 ..` = DAP_TransferBlock…），
所以固件这边只干三件事：收命令 → `DAP_ExecuteCommand()` → 回响应。

第一次连上时 OpenOCD 发的第一包就是它，可以用来对表：

```
  OpenOCD → 44 41 50 00 02 00 01 00 | 00 F0      （DAP_Info 0xF0 = Capabilities）
  探针    → 44 41 50 00 02 00 02 00 | 00 02 11 01 （cmd=0x00 len=2 caps=[0x11,0x01]）
```

想脱离 OpenOCD 直接验探针：`python tools\dap_tcp_probe.py <探针IP>`（裸客户端，
逐条打 opcode / 长度 / 往返耗时）。

---

## 4. 🚨 四个坑（都踩过，照抄结论）

### ① `DAP_PACKET_COUNT` 必须报 **1**（最坑的一个）

`DAP_Info(0xF0)` 里我们报的 **packet count** 决定 OpenOCD 能不能**流水线**。
报 ≥2 时，OpenOCD 会同时挂多条命令、用非阻塞读收响应 —— 而 **ESP-IDF 那份 OpenOCD 0.12
fork 的 TCP 后端在流水线路径上会漏读一条响应**，于是下一条同步命令（如 SWJ_Sequence）
读到的是遗留的旧响应：

```
Error: CMSIS-DAP command mismatch. Sent 0x12 received 0x5
Error: Failed to read memory at 0x200003f4
```

之后整条流错位、`dump_image` 全废（实测：dump 4 KB/16 KB **必现**；小的 256 B 偶尔侥幸通过）。
**报 1 就严格一问一答**，dump/load 16 KB 全通。代价是没有流水线 —— 对 RTT 主导的 WiFi 链路
本来也没多少可重叠的。

> 排查过程值得记：探针侧逐命令 trace 显示"每条请求都回了恰好一条响应"，
> 客户端侧只有 1 次 mismatch、**0 次超时、0 次短读** → 说明不是丢包/拆包，
> 而是客户端自己少消费了一条响应。最后在 `cmsis_dap.c` 里看到
> `packet_count = MIN(MAX_PENDING_REQUESTS, DAP_Info(0xF0))` 才锁定。
> （`DAP.c` 里原本有 `#if (DAP_PACKET_COUNT < 2U) #error`，本工程改成允许 1。）

### ② `cmsis-dap tcp min_timeout` 是"每条命令睡多久"，越小越快

这份 fork 的 TCP 后端**每收一条命令都先睡一个 `min_timeout` 再读响应**：

| `min_timeout` | `init;halt` 总耗时（128 条命令） |
|---|---|
| 0 / 5 / 10 ms | **1.4~1.8 s** |
| 20 ms | 2.4 s |
| 50 ms | 4.4 s |
| 150 ms | 10.4 s |
| 300 ms | **20.6 s** |

→ 局域网用 **10 ms** 就够（往返实测 3.3~4.2 ms）；网络差再往上调。
`make dap-wifi-speed` / `dap-wifi.ps1` 已经默认 10。

### ③ dedicated GPIO 的 bundle 是**按核绑定**的 → DAP 任务必须钉 core 0

`DAP_Connect` 会走到 `PORT_SWD_SETUP()` → `dedic_gpio_del_bundle()` + `new_bundle()`
重装焊盘路由（见 §4.14 的 bug ②），而 IDF 要求**建/删 bundle 必须在同一个核上**。
把任务钉到 core 1 就会看到：

```
E dedic_gpio: dedic_gpio_del_bundle(319): del bundle on wrong CPU
W gpio: conflict found for GPIO[46]
```

连接还能凑合用，但路由处于"半装"状态 —— 这是拿运气当设计。
本工程把 `dap-tcp` 任务钉 **core 0**（= `app_main`/`DAP_Setup()` 所在核），日志里会打
`DAP 任务跑在 core 0`。优先级 18 < WiFi 任务的 23：WiFi 该抢占就抢占。

### ④ 起服务的顺序：`esp_netif_init()` → 建 socket → 才起 WiFi

反了会怎样：TCP 任务在 `esp_netif_init()` 之前 `socket()` 直接失败、任务退出，
**监听压根没起来**（PC 怎么连都连不上，而日志看上去一切正常）。
现在的顺序是 `net_stack_init()` → 建 `dap-tcp` 任务 → `wifi_start()`，
而且**关联/DHCP 慢也不再拖住服务**（早先版本在这里死等 30 s，DHCP 一慢就"看着像没起来"）。

### 附：一些"看着吓人其实正常"的现象

- **`Error: CMSIS-DAP: error reading header` + `peek_socket WSAGetLastError==10035` 刷屏**：
  这份 fork 的 TCP 后端每收一条命令都会先做一次**非阻塞 peek**，响应当然还没到 →
  打一行 Error 再等。128 行 = 128 条命令，属正常噪声（上游后来的版本已经改安静了）。
- **`Warn: target was in unknown state when halt was requested`**：空片（没烧过程序的 F103）
  上正常，halt 之后 `pc = 0xfffffffe`。
- **`SoftAP 模式` 下电脑会"断网"**：电脑连到探针热点后，互联网当然就没了 ——
  这正是**推荐用 STA 模式（接你家路由器）**的原因。STA 连不上时固件会自动退回 SoftAP 兜底。

---

## 5. 文件与分工

| 文件 | 作用 |
|---|---|
| `main/wifi_dap_main.c` | 本工程新增：WiFi(STA/SoftAP) + TCP 服务器 + 8 字节头组帧 + 统计 |
| `main/daplink/DAP.c` | ARM CMSIS-DAP 协议层（原样复用；只放宽了 `DAP_PACKET_COUNT ≥ 1`） |
| `main/daplink/SW_DP_fast.c` / `dedic_swd.S` | 17~22 MHz 的 SWD 位时序（与有线版**同一份**） |
| `main/daplink/DAP_config.h` | 引脚 + **`DAP_PACKET_SIZE=1024` / `DAP_PACKET_COUNT=1`**（见 §4 ①） |
| `main/Kconfig.projbuild` | SSID/密码/热点名/端口 |
| `scripts/dap-wifi.ps1` | **一把过自检**：TCP → 认目标 → IDCODE/halt/reg pc，最后给 PASS/FAIL |
| `scripts/dap-speed.ps1` | **测速**：16 KB SRAM 读写 + 逐字节校验（`-Backend tcp` 走无线）|
| `tools/openocd_speed.ps1` | 上面那个真正干活的：起 OpenOCD、计时、比对（`-Backend usb/tcp` 都支持）|
| `tools/ocd_path.ps1` | 自动找 OpenOCD（换机器盘符/版本号都不用改脚本）|
| `tools/dap_tcp_probe.py` | 裸客户端（协议级自测 / 往返延迟分布）|
| `tools/cmsis_dap_speed.py` | pyOCD 侧的测速（无线版用不到，留着和有线版对照）|
| `TEST-REPORT.md` | **实测报告**：环境、原始输出、`min_timeout` 扫描、探针侧统计 |

> 📌 本仓库是独立仓库：这几个脚本原版放在作者工作区的 `scripts\` / `tools\` 下，
> 这里原样收进来（只把 `openocd_speed.ps1` 的临时目录从工作区路径改成仓库根的 `_ocd\`）。

## 6. 已知限制

- **一次只服务一个客户端**：OpenOCD 断开后才会接下一个（想并行调试请起第二个探针）。
- **没有流水线**（见 §4 ①）：吞吐受 RTT 限制，见 §1 的数字。
- pyOCD **不支持** `cmsis-dap backend tcp`（那是 OpenOCD 的后端），无线版只能用 OpenOCD。
- 这份 OpenOCD 0.12 fork 的 TCP 后端噪声大（见 §4 附）；换更新的 OpenOCD
  （上游 ≥ 那个 "More robust socket handling" 补丁）应该会安静很多，协议是同一个。

---

## 7. 许可与第三方

| 内容 | 来源 / 许可 |
|---|---|
| 本工程代码（`main/`、`scripts/`、`tools/`） | Apache-2.0，见 [LICENSE](LICENSE) |
| `main/daplink/DAP.c` / `DAP.h` | ARM **CMSIS-DAP** 参考实现（Apache-2.0），原样复用；只把 `DAP_PACKET_COUNT` 的下限从 2 放宽到 1（见 §4 ①） |
| `main/daplink/SW_DP_*.c` / `dedic_swd.S` / `dedic_selftest.c` | 本工作区 `cherrydap_s31_fast` 的 SWD 位时序层，同一份 |
| 协议后端 | OpenOCD 上游 `src/jtag/drivers/cmsis_dap_tcp.c`（GPL-2.0，仅在**你的电脑上**使用，不包含在本仓库里） |
| 参考实现 | [bkuschak/cmsis_dap_tcp_esp32](https://github.com/bkuschak/cmsis_dap_tcp_esp32)（思路） |

> ⚠️ 编译需要 **ESP-IDF**（本工程是标准 IDF 工程，`esp32s31` 是 preview target，
> 所有 `idf.py` 调用要带 `--preview`）。测试还需要 ESP-IDF 自带的 **OpenOCD 0.12** 与 PowerShell。

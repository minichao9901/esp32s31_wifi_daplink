# dap-wifi.ps1 —— 无线探针（cherrydap_s31_wifi）一把过自检
#
# 干三件事：连上探针 → 认目标 → 读身份/停核/看寄存器，最后把"能不能用"讲清楚。
# 只读不烧录，目标板不会被改。
#
# 用法：pwsh -File scripts\dap-wifi.ps1 [-ProbeIp 192.168.1.48] [-Port 4441] [-MinTimeout 10]
#   （IP 从串口日志里抄：`拿到 IP：192.168.x.x（探针地址 = 192.168.x.x:4441）`；
#     自开热点模式则固定是 192.168.4.1）
param(
    [string]$ProbeIp = "",
    [int]$Port = 4441,
    [int]$MinTimeout = 10
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools\ocd_path.ps1')

$ocd = Get-OpenOcdPath
if (-not $ocd) { Write-Host "找不到 openocd.exe" -ForegroundColor Red; exit 1 }
$scripts = Get-OpenOcdScriptsDir -OpenOcd $ocd
if (-not $scripts) { Write-Host "$ocd 旁边没有 share\openocd\scripts" -ForegroundColor Red; exit 1 }

if (-not $ProbeIp) {
    # local.env.ps1 里可以写 ESP32_S31_WIFI_PROBE_IP
    $envFile = Join-Path $root 'local.env.ps1'
    if (Test-Path $envFile) {
        $m = Select-String -Path $envFile -Pattern 'ESP32_S31_WIFI_PROBE_IP\s*=\s*["'']([^"'']+)' | Select-Object -First 1
        if ($m) { $ProbeIp = $m.Matches[0].Groups[1].Value }
    }
}
if (-not $ProbeIp) { $ProbeIp = '192.168.4.1'; Write-Host "没给 -ProbeIp，按自开热点默认值 $ProbeIp 试（接自家 WiFi 的话要显式给 IP）" -ForegroundColor Yellow }

Write-Host "== 无线探针自检：$ProbeIp`:$Port ==" -ForegroundColor Cyan

# 先探端口：连不上就别浪费时间让 OpenOCD 重试
try {
    $c = New-Object Net.Sockets.TcpClient
    if (-not ($c.ConnectAsync($ProbeIp, $Port).Wait(1500) -and $c.Connected)) { throw "连不上" }
    $c.Close()
    Write-Host "TCP $ProbeIp`:$Port 通 ✓"
} catch {
    Write-Host "TCP $ProbeIp`:$Port 连不上 —— 依次查：" -ForegroundColor Red
    Write-Host "  1) 串口日志里那行「拿到 IP：...」是多少（STA 模式 IP 会变；自开热点固定 192.168.4.1）"
    Write-Host "  2) 电脑和探针在同一个网络吗（STA 模式要连同一个路由器）"
    Write-Host "  3) 探针起来了吗（make run PROJ=cherrydap_s31_wifi 看日志）"
    exit 1
}

$ocdArgs = @('-s', $scripts,
          '-c', 'adapter driver cmsis-dap',
          '-c', 'transport select swd',
          '-c', 'cmsis-dap backend tcp',
          '-c', "cmsis-dap tcp host $ProbeIp",
          '-c', "cmsis-dap tcp port $Port",
          '-c', "cmsis-dap tcp min_timeout $MinTimeout",
          '-f', "$scripts\target\stm32f1x.cfg",
          '-c', 'init', '-c', 'halt',
          '-c', 'mdw 0xE0042000', '-c', 'reg pc', '-c', 'shutdown')

$out = & $ocd @ocdArgs 2>&1
# 这份 fork 的 TCP 后端每收一条命令都会先打一行 peek 超时的 Error —— 是噪声，滤掉
$noise = 'peek_socket|error reading header|^\s*\.?\s*$'
$clean = $out | Where-Object { $_ -notmatch $noise }

Write-Host "`n---- OpenOCD 关键输出 ----"
$clean | Select-String -Pattern 'CMSIS-DAP: (SWD|FW|Interface|Serial)|DPIDR|Cortex|0xE0042000|pc \(' |
    ForEach-Object { "  " + $_.Line.Trim() }

$ok = ($clean | Select-String 'Examination succeed').Count -gt 0
$id = ($clean | Select-String '0xE0042000:').Count -gt 0
Write-Host ""
if ($ok -and $id) {
    Write-Host "[PASS] 无线链路通：识别到目标、读到了 DBGMCU IDCODE" -ForegroundColor Green
    Write-Host "       测速：pwsh -File scripts\dap-speed.ps1 -Tool openocd -Backend tcp -HostIp $ProbeIp -Port $Port" -ForegroundColor Green
    exit 0
}
Write-Host "[FAIL] 没识别到目标 —— 看上面原始输出里的 Error；" -ForegroundColor Red
Write-Host "       常见：目标没供电 / SWCLK(J2-13)、SWDIO(J2-14)、GND(J2-8) 没接牢 / 目标被别的调试器占着"
$clean | Select-String -Pattern 'Error' | Select-Object -First 6 | ForEach-Object { "  " + $_.Line.Trim() }
exit 1

# openocd_speed.ps1 —— 用 OpenOCD 测探针的 16KB 批量读写速度 + 逐字节校验
#
# 两种探针都支持（同一套口径，数字可直接对比）：
#   -Backend usb （默认）：`cmsis-dap backend usb_bulk` —— 插 USB-HS 的 CMSIS-DAP v2 探针
#                          （cherrydap_s31 / cherrydap_s31_fast）
#   -Backend tcp          ：`cmsis-dap backend tcp` —— 无线探针（cherrydap_s31_wifi，走 WiFi）
#
# 🚨 USB 后端的坑：ESP-IDF 那份 OpenOCD 0.12 **默认走 TCP 后端**（不指定就报
#    "hostname ... must be specified"），所以 USB 必须显式 usb_bulk；
#    TCP 后端则必须给 `cmsis-dap tcp host/port`。`D:\sdk_env` 那份 0.11 两种都不行。
#
# 用法：
#   pwsh -File tools\openocd_speed.ps1 [-Rounds 5] [-Size 16384]
#   pwsh -File tools\openocd_speed.ps1 -Backend tcp -HostIp 192.168.1.48 -Port 4441
param(
    [int]$Rounds = 5,
    [int]$Size = 16384,
    [string]$Addr = "0x20000000",
    [string]$OpenOcd = "",             # 留空 = 自动找 .espressif 里最新那份 0.12
    [ValidateSet('usb', 'tcp')] [string]$Backend = 'usb',
    [string]$HostIp = "",              # -Backend tcp 时必填（探针 IP，串口日志里会打）
    [int]$Port = 4441,
    [int]$MinTimeout = 10             # 无线链路的往返余量；慢网络可加大
)
$ErrorActionPreference = "Continue"

# 自动发现 OpenOCD（换机器盘符/版本号都会变，别写死）
. (Join-Path $PSScriptRoot 'ocd_path.ps1')
if (-not $OpenOcd) { $OpenOcd = Get-OpenOcdPath }
if (-not $OpenOcd) { Write-Host "找不到 openocd.exe（.espressif\tools\openocd-esp32\*\...\bin\ 下没有）" -ForegroundColor Red; exit 1 }
$scripts = Join-Path (Split-Path (Split-Path $OpenOcd)) "share\openocd\scripts"
$root = Split-Path $PSScriptRoot -Parent
# 临时文件（写/读用的 bin）放仓库根的 _ocd\ 下 —— 本仓库是独立仓库，
# 不依赖作者工作区里别的工程目录（原版指向 projects\cherrydap_s31_fast\_ocd）。
$work = Join-Path $root "_ocd"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$wbin = ($work -replace '\\', '/') + "/w.bin"
$rbin = ($work -replace '\\', '/') + "/r.bin"

# 造测试数据（可复现的图案）
$big = New-Object byte[] $Size
for ($i = 0; $i -lt $Size; $i++) { $big[$i] = [byte](($i * 7 + 3) -band 0xFF) }
[System.IO.File]::WriteAllBytes("$work\w.bin", $big)

if ($Backend -eq 'tcp') {
    if (-not $HostIp) { Write-Host "用 -Backend tcp 必须给 -HostIp <探针IP>（串口日志里会打）" -ForegroundColor Red; exit 1 }
    Write-Host "== 无线探针（WiFi/TCP）：$HostIp`:$Port ==" -ForegroundColor Cyan
    $base = @("-s", $scripts,
              "-c", "adapter driver cmsis-dap",
              "-c", "transport select swd",
              "-c", "cmsis-dap backend tcp",
              "-c", "cmsis-dap tcp host $HostIp",
              "-c", "cmsis-dap tcp port $Port",
              "-c", "cmsis-dap tcp min_timeout $MinTimeout",
              "-f", "$scripts\target\stm32f1x.cfg",
              "-c", "init", "-c", "halt")
} else {
    $base = @("-s", $scripts,
              "-f", "$scripts\interface\cmsis-dap.cfg",
              "-c", "cmsis-dap backend usb_bulk",
              "-f", "$scripts\target\stm32f1x.cfg",
              "-c", "init", "-c", "halt")
}

function Invoke-Ocd([string[]]$extra) {
    $o = & $OpenOcd @base @extra -c "shutdown" 2>&1
    return ($o | ForEach-Object { $_.ToString() })
}

# 基线（不含传输）
$t0 = Get-Date
$null = Invoke-Ocd @()
$baseSec = ((Get-Date) - $t0).TotalSeconds
Write-Host ("基线（init+halt+shutdown）= {0:N3} s" -f $baseSec)

$res = @{}
foreach ($mode in @("read", "write")) {
    $times = @()
    for ($i = 0; $i -lt $Rounds; $i++) {
        if ($mode -eq "read") {
            $o = Invoke-Ocd @("-c", "dump_image $rbin $Addr $Size")
            $m = [regex]::Match(($o -join "`n"), 'dumped \d+ bytes in ([\d.]+)s')
        } else {
            $o = Invoke-Ocd @("-c", "load_image $wbin $Addr bin")
            $m = [regex]::Match(($o -join "`n"), 'downloaded \d+ bytes in ([\d.]+)s')
        }
        if ($m.Success) { $times += [double]$m.Groups[1].Value }
        else { Write-Host "  [$mode] 第 $($i+1) 轮没解析到计时：$($o | Select-String 'rror' | Select-Object -First 1)" }
    }
    $res[$mode] = $times
}

Write-Host "`n=========== OpenOCD 16 KB SRAM（$Rounds 轮，OpenOCD 自报计时）==========="
foreach ($mode in @("read", "write")) {
    $t = $res[$mode]
    if (-not $t) { continue }
    $best = ($t | Measure-Object -Minimum).Minimum
    $med = ($t | Sort-Object)[[int]($t.Count / 2)]
    $kbs = $Size / $best / 1000
    Write-Host ("{0,-5} 最快 {1,7:N2} ms / 中位 {2,7:N2} ms  ->  {3,7:N0} kB/s ({4:N2} MB/s)" -f `
        $mode, ($best * 1000), ($med * 1000), $kbs, ($kbs / 1000))
}

# ---- 逐字节校验：写进去的图案，读回来必须一模一样（无线链路最怕"看着成功、数据错了"）----
Write-Host "`n=========== 数据校验（写图案 → 读回 → 逐字节比对）==========="
$null = Invoke-Ocd @("-c", "load_image $wbin $Addr bin")
$null = Invoke-Ocd @("-c", "dump_image $rbin $Addr $Size")
if (Test-Path "$work\r.bin") {
    $a = [System.IO.File]::ReadAllBytes("$work\w.bin")
    $b = [System.IO.File]::ReadAllBytes("$work\r.bin")
    if ($a.Length -eq $b.Length) {
        $diff = 0
        for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { $diff++ } }
        if ($diff -eq 0) {
            Write-Host ("[PASS] 写入 $($a.Length) 字节 → 读回逐字节一致") -ForegroundColor Green
        } else {
            Write-Host ("[FAIL] $diff / $($a.Length) 字节不一致") -ForegroundColor Red
        }
    } else {
        Write-Host "[FAIL] 长度不一致：读回 $($b.Length) != 写入 $($a.Length)" -ForegroundColor Red
    }
} else {
    Write-Host "[FAIL] 没生成 r.bin（dump_image 失败？）" -ForegroundColor Red
}

Write-Host "（对比：tools\cmsis_dap_speed.py 的 pyOCD 数字；线上极限按 46 拍/字 × ~3.05µs ≈ 12.5 ms）"
if ($Backend -eq 'tcp') {
    Write-Host "ℹ️ 无线版慢在**往返延迟**（每次 SWD 事务一个来回），不是探针：探针侧 SWD 时间只有几十 µs。"
    Write-Host " ℹ️ 刚跑过别的客户端时第一轮往往明显偏慢 → 看"最快"那列或多跑几轮。"
} else {
    Write-Host "⚠️ 刚跑过别的客户端时**第一轮往往明显偏慢**（实测单轮 16/26 ms，5 轮最快 14.1/12.8 ms）→ 至少 3 轮、看"最快""
}

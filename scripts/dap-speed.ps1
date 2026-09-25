#Requires -Version 7
# make dap-speed-pyocd / dap-speed-openocd  —— DAP 探针 16KB SRAM 批量测速
#
# 两个客户端测的是**同一个探针**，数字可以直接对比：
#   pyOCD    读受它的 v2 后端影响（按 512B 包循环读，读只有线上的 ~58%）
#   OpenOCD  两个方向都到线上极限的 95~98%（读比 pyOCD 快 ~1.6 倍）
# 详见 AGENTS.md §4.11 / §4.14 与 projects\cherrydap_s31_fast\README.md
param(
    [ValidateSet('pyocd', 'openocd')] [string]$Tool = 'pyocd',
    [int]$Size = 16384,
    [int]$Rounds = 3,
    [int]$Freq = 1000000,          # 只对 pyOCD 有意义（极限档忽略 DAP_SWJ_Clock）
    [string]$Addr = '0x20000000',
    [ValidateSet('usb', 'tcp')] [string]$Backend = 'usb',   # tcp = 无线探针（cherrydap_s31_wifi）
    [string]$HostIp = "",
    [int]$Port = 4441
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if ($Backend -eq 'tcp' -and $Tool -ne 'openocd') {
    Write-Host "无线探针只能走 OpenOCD：pyOCD 不认 cmsis-dap backend tcp" -ForegroundColor Yellow
    Write-Host "（要测无线版就：make dap-wifi-speed WIFI_HOST=<探针IP>）" -ForegroundColor Yellow
    exit 2
}

switch ($Tool) {
    'pyocd' {
        # pyOCD 装在独立 venv 里；优先用它，省掉 cmsis_dap_speed.py 的自换解释器那一步
        $venvPy = Join-Path $env:USERPROFILE '.venvs\pyocd\Scripts\python.exe'
        $py = if (Test-Path $venvPy) { $venvPy } else { (Get-Command python -ErrorAction Stop).Source }
        Write-Host '== pyOCD 测速（探针 cherrydap_s31*，目标 STM32F103，16KB SRAM 逐字节校验）==' -ForegroundColor Cyan
        & $py (Join-Path $root 'tools\cmsis_dap_speed.py') --size $Size --rounds $Rounds --freq $Freq --addr $Addr
        exit $LASTEXITCODE
    }
    'openocd' {
        $mode = if ($Backend -eq 'tcp') { "无线（WiFi/TCP → $HostIp`:$Port）" } else { '有线（USB-HS）' }
        Write-Host "== OpenOCD 测速（$mode；同一套 16KB SRAM + 逐字节校验）==" -ForegroundColor Cyan
        & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\openocd_speed.ps1') `
            -Size $Size -Rounds $Rounds -Addr $Addr -Backend $Backend -HostIp $HostIp -Port $Port
        exit $LASTEXITCODE
    }
}

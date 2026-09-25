# ocd_path.ps1 —— 点源进来的小工具：自动定位本机的 OpenOCD（ESP-IDF 那份 0.12）
#
# 为什么要它：`.espressif\tools\openocd-esp32\<版本>\openocd-esp32\bin\openocd.exe`
# 里带**日期版本号**，写死在脚本里一换机器/一升级就失效（家里是 C:、公司是 D:）。
# 🚨 只有 0.12 那份支持 CMSIS-DAP v2（`D:\sdk_env` 的 0.11 不认我们的探针）。
# 用法：`. (Join-Path $PSScriptRoot 'ocd_path.ps1')` 然后 `$ocd = Get-OpenOcdPath`

function Get-OpenOcdPath {
    param([string]$Hint = "")

    if ($Hint -and (Test-Path $Hint)) { return (Resolve-Path $Hint).Path }

    # ① ESP-IDF 自己装的那份（优先取版本号最大的）
    $pat = Join-Path $env:USERPROFILE '.espressif\tools\openocd-esp32\*\openocd-esp32\bin\openocd.exe'
    $c = Get-ChildItem $pat -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if ($c) { return $c.FullName }

    # ② PATH 里的（可能是 0.11，不支持 v2 —— 调用方自己留意）
    $g = Get-Command openocd -ErrorAction SilentlyContinue
    if ($g) { return $g.Source }

    return $null
}

# 返回 `<脚本目录>\scripts`（openocd 的 -s 参数），顺带校验它像不像一份完整安装
function Get-OpenOcdScriptsDir {
    param([string]$OpenOcd)
    $d = Join-Path (Split-Path (Split-Path $OpenOcd)) 'share\openocd\scripts'
    if (Test-Path (Join-Path $d 'interface\cmsis-dap.cfg')) { return $d }
    return $null
}

# One-shot launcher: virtmic-engine + ctl start + virtmic-feeder
# Usage (from repo root or any cwd):
#   powershell -ExecutionPolicy Bypass -File scripts\start-virtmic.ps1

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $Root "CMakeLists.txt"))) {
  Write-Host "错误: 找不到仓库根目录（应包含 CMakeLists.txt）: $Root" -ForegroundColor Red
  exit 1
}

$Bin = Join-Path $Root "build\engine\Debug"
$Engine = Join-Path $Bin "virtmic-engine.exe"
$Ctl = Join-Path $Bin "virtmic_ctl.exe"
$Feeder = Join-Path $Bin "virtmic-feeder.exe"

$missing = @()
foreach ($p in @($Engine, $Ctl, $Feeder)) {
  if (-not (Test-Path $p)) { $missing += $p }
}
if ($missing.Count -gt 0) {
  Write-Host "错误: 缺少已编译的程序，请先构建 Debug 目标:" -ForegroundColor Red
  Write-Host "  cmake -S . -B build -DVIRTMIC_BUILD_TESTS=ON"
  Write-Host "  cmake --build build --config Debug"
  Write-Host "缺失文件:"
  $missing | ForEach-Object { Write-Host "  $_" }
  exit 1
}

Write-Host "仓库: $Root"
Write-Host "启动 virtmic-engine ..."
$engineProc = Start-Process -FilePath $Engine -WorkingDirectory $Bin -PassThru
Start-Sleep -Milliseconds 400
if ($engineProc.HasExited) {
  Write-Host "错误: virtmic-engine 启动后立刻退出 (exit=$($engineProc.ExitCode))。请手动运行引擎查看报错。" -ForegroundColor Red
  exit 1
}

Write-Host "等待 engine 控制管道并发送 start ..."
$started = $false
$deadline = (Get-Date).AddSeconds(20)
# Native stderr must not abort the retry loop under ErrorAction Stop.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
while ((Get-Date) -lt $deadline) {
  if ($engineProc.HasExited) {
    Write-Host "错误: virtmic-engine 已退出 (exit=$($engineProc.ExitCode))。" -ForegroundColor Red
    $ErrorActionPreference = $prevEap
    exit 1
  }
  cmd /c "`"$Ctl`" start >nul 2>nul"
  if ($LASTEXITCODE -eq 0) {
    $started = $true
    break
  }
  Start-Sleep -Milliseconds 250
}
$ErrorActionPreference = $prevEap

if (-not $started) {
  Write-Host "错误: virtmic_ctl start 超时。请查看 engine 窗口是否已打印 listening on pipe。" -ForegroundColor Red
  exit 1
}

Write-Host "启动 virtmic-feeder ..."
Start-Process -FilePath $Feeder -WorkingDirectory $Bin

Write-Host ""
Write-Host "已启动。请确认:" -ForegroundColor Green
Write-Host "  1) engine 窗口打印 capture from `"真麦名`" (不是 VirtMic)"
Write-Host "  2) 系统/微信默认输入可选 VirtMic"
Write-Host "  3) 停止时关闭 engine 与 feeder 窗口即可"
exit 0

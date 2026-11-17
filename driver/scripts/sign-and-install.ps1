# Test-sign and install VirtMic (requires elevated Admin + testsigning ON).
param(
  [string]$PackageDir = "",
  [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
$kitBin = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
$signtool = Join-Path $kitBin "signtool.exe"
$makecert = Join-Path $kitBin "MakeCert.exe"
$certmgr = Join-Path $kitBin "certmgr.exe"

function Assert-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $p = New-Object Security.Principal.WindowsPrincipal($id)
  if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated PowerShell (Admin)."
  }
}

Assert-Admin

if ($Uninstall) {
  Write-Host "Removing ROOT\VirtMic via pnputil oem packages matching VirtMic / SimpleAudioSample..."
  pnputil /enum-drivers | Out-Host
  Write-Host "Manually: pnputil /delete-driver <oemXX.inf> /uninstall /force"
  Write-Host "Or Device Manager -> VirtMic -> Uninstall device (delete driver)."
  exit 0
}

if (-not $PackageDir) {
  $candidates = @(
    (Join-Path $PSScriptRoot "..\simpleaudiosample\Package\x64\Release\package"),
    (Join-Path $PSScriptRoot "..\simpleaudiosample\x64\Release"),
    (Join-Path $PSScriptRoot "..\out\x64\Release")
  )
  foreach ($c in $candidates) {
    if (Test-Path (Join-Path $c "simpleaudiosample.sys")) { $PackageDir = $c; break }
    $inf = Get-ChildItem $c -Filter "*.inf" -ErrorAction SilentlyContinue | Select-Object -First 1
    $sys = Get-ChildItem $c -Filter "*.sys" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($inf -and $sys) { $PackageDir = $c; break }
  }
}

if (-not $PackageDir -or -not (Test-Path $PackageDir)) {
  throw "PackageDir with .sys/.inf not found. Build first (build-driver.ps1)."
}

$sys = Get-ChildItem $PackageDir -Filter "*.sys" -Recurse | Select-Object -First 1
$inf = Get-ChildItem $PackageDir -Filter "*.inf" -Recurse | Select-Object -First 1
if (-not $sys -or -not $inf) { throw "Missing .sys or .inf under $PackageDir" }

Write-Host "Using INF=$($inf.FullName)"
Write-Host "Using SYS=$($sys.FullName)"

# Ensure testsigning (may require reboot)
$ts = (bcdedit /enum "{current}" | Select-String "testsigning\s+Yes")
if (-not $ts) {
  Write-Host "Enabling testsigning (reboot required if this is the first time)..."
  bcdedit /set testsigning on | Out-Host
  Write-Host "REBOOT required, then re-run this script."
  exit 2
}

$certdir = Join-Path $PSScriptRoot "..\certs"
New-Item -ItemType Directory -Force -Path $certdir | Out-Null
$cer = Join-Path $certdir "VirtMicTest.cer"
$pvk = Join-Path $certdir "VirtMicTest.pvk"

if (-not (Test-Path $cer)) {
  & $makecert -r -pe -ss PrivateCertStore -n "CN=VirtMic Test" -eku 1.3.6.1.5.5.7.3.3 $cer
  if ($LASTEXITCODE -ne 0) { throw "MakeCert failed" }
  & $certmgr /add $cer /s /r localMachine root
  & $certmgr /add $cer /s /r localMachine trustedpublisher
}

& $signtool sign /fd SHA256 /s PrivateCertStore /n "VirtMic Test" $sys.FullName
if ($LASTEXITCODE -ne 0) { throw "signtool failed" }

# Root-enumerated virtual device install
Write-Host "Installing driver package..."
pnputil /add-driver $inf.FullName /install | Out-Host

Write-Host "Creating ROOT\VirtMic if needed (devcon-style via pnputil / scan)..."
# Prefer: pnputil /add-driver already installs; for ROOT devices use:
#   %WDK%\Tools\devcon.exe install path\to.inf ROOT\VirtMic
$devconCandidates = @(
  "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe",
  "C:\Program Files (x86)\Windows Kits\10\Tools\x64\devcon.exe"
)
$devcon = $devconCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($devcon) {
  & $devcon install $inf.FullName "ROOT\VirtMic" | Out-Host
} else {
  Write-Host "devcon.exe not found. Install ROOT device manually:"
  Write-Host "  Device Manager -> Action -> Add legacy hardware -> VirtMic INF"
  Write-Host "  Or obtain WDK Tools devcon and run:"
  Write-Host "  devcon install `"$($inf.FullName)`" ROOT\VirtMic"
}

Write-Host "Done. Verify: Settings -> System -> Sound -> Input -> VirtMic"

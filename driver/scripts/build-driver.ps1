# Build VirtMic (SimpleAudioSample-based) KMDF driver — x64 Debug or Release.
param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Release",
  [ValidateSet("x64")]
  [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
  throw "vswhere.exe not found: $vswhere"
}

$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vs) {
  throw "Visual Studio with MSBuild not found via vswhere"
}

$msbuild = Join-Path $vs "MSBuild\Current\Bin\amd64\MSBuild.exe"
if (-not (Test-Path $msbuild)) {
  $msbuild = Join-Path $vs "MSBuild\Current\Bin\MSBuild.exe"
}
$sln = Join-Path $PSScriptRoot "..\simpleaudiosample\SimpleAudioSample.sln"

if (-not (Test-Path $msbuild)) { throw "MSBuild not found under: $vs" }
if (-not (Test-Path $sln)) { throw "Solution not found: $sln" }

& $msbuild $sln /m /p:Configuration=$Configuration /p:Platform=$Platform /p:SignMode=Off /p:SpectreMitigation=false
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed: $LASTEXITCODE" }

$pkg = Join-Path $PSScriptRoot "..\simpleaudiosample\$Platform\$Configuration\package"
Write-Host "Package: $pkg"
Get-ChildItem $pkg -ErrorAction SilentlyContinue | Select-Object Name, Length

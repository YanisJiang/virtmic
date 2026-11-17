# Build VirtMic (SimpleAudioSample-based) KMDF driver — x64 Debug or Release.
param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Release",
  [ValidateSet("x64")]
  [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"
$vs = "D:\Soft\Microsoft\Visual Studio\2022\Community"
$msbuild = Join-Path $vs "MSBuild\Current\Bin\amd64\MSBuild.exe"
$sln = Join-Path $PSScriptRoot "..\simpleaudiosample\SimpleAudioSample.sln"

if (-not (Test-Path $msbuild)) { throw "MSBuild not found: $msbuild" }
if (-not (Test-Path $sln)) { throw "Solution not found: $sln" }

& $msbuild $sln /m /p:Configuration=$Configuration /p:Platform=$Platform /p:SignMode=Off /p:SpectreMitigation=false
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed: $LASTEXITCODE" }

$pkg = Join-Path $PSScriptRoot "..\simpleaudiosample\$Platform\$Configuration\package"
Write-Host "Package: $pkg"
Get-ChildItem $pkg -ErrorAction SilentlyContinue | Select-Object Name, Length

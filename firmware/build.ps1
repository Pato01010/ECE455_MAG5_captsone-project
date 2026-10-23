# Build the firmware with the tools in STM32CubeCLT (nothing to install).
#   powershell -ExecutionPolicy Bypass -File build.ps1 [release|debug|tcxo]
param([string]$Preset = "release")

$clt = if ($env:CUBECLT_ROOT) { $env:CUBECLT_ROOT } else { "C:\ST\STM32CubeCLT_1.21.0" }
$env:PATH = "$clt\CMake\bin;$clt\Ninja\bin;$clt\GNU-tools-for-STM32\bin;$env:PATH"
Set-Location $PSScriptRoot

cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build --preset $Preset
exit $LASTEXITCODE

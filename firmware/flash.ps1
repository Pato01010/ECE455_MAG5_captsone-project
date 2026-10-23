# Program the Nucleo over its ST-LINK (USB) and reset it.
#   powershell -ExecutionPolicy Bypass -File flash.ps1 [release|debug|tcxo]
param([string]$Preset = "release")

$clt = if ($env:CUBECLT_ROOT) { $env:CUBECLT_ROOT } else { "C:\ST\STM32CubeCLT_1.21.0" }
$elf = Join-Path $PSScriptRoot "build\$Preset\magnetometer.elf"
if (-not (Test-Path $elf)) { Write-Error "No $elf - run build.ps1 $Preset first"; exit 1 }

& "$clt\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe" -c port=SWD mode=UR -w $elf -v -rst
exit $LASTEXITCODE

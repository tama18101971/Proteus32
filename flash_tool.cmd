@echo off
rem Flash firmware using Proteus32Flasher
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM5"
set "BIN=%~2"
if "%BIN%"=="" set "BIN=%~dp0.pio\build\Proteus32\firmware.bin"

"%~dp0dist\Proteus32Flasher.exe" %PORT% "%BIN%"

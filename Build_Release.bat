@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem  HAWK 370 - Build_Release.bat
rem
rem  Collects everything needed to flash a HAWK 370 into the "release" folder:
rem
rem    HAWK370_bootloader.bin  0x0       web installer only
rem    HAWK370_partitions.bin  0x8000    web installer only
rem    boot_app0.bin           0xE000    web installer only
rem    HAWK370_firmware.bin    0x10000   web installer + OTA (Files page)
rem    HAWK370_littlefs.bin    0x290000  web installer + OTA (Files page)
rem
rem  How to use:
rem    1. Arduino IDE: open ESP32_C3_SuperMini.ino and run
rem       "Sketch - Export Compiled Binary" (board settings: see README).
rem    2. Double-click this file. It works from wherever the sketch folder is.
rem
rem  mklittlefs (builds the web-page image) comes with the ESP32 core and is
rem  found automatically in %LOCALAPPDATA%\Arduino15. If your Arduino data
rem  folder is elsewhere, set MKLITTLEFS_EXE to the full path of mklittlefs.exe.
rem ===========================================================================

cd /d "%~dp0"
set "BUILD=build\esp32.esp32.esp32c3"
set "PROJECT=ESP32_C3_SuperMini.ino"
set "OUT=release"
set "FAILED="

rem --- 1. Compiled firmware from "Export Compiled Binary" ----------------------
for %%F in ("%BUILD%\%PROJECT%.bin" "%BUILD%\%PROJECT%.bootloader.bin" "%BUILD%\%PROJECT%.partitions.bin" "%BUILD%\boot_app0.bin") do (
  if not exist "%%~F" (
    echo Missing %%~F
    set "FAILED=1"
  )
)
if defined FAILED (
  echo.
  echo Run "Sketch > Export Compiled Binary" in the Arduino IDE first,
  echo with the board set to "ESP32C3 Dev Module".
  goto :end
)

rem --- 2. mklittlefs from the ESP32 core (newest installed version) -----------
set "MKLITTLEFS=%MKLITTLEFS_EXE%"
if not defined MKLITTLEFS (
  for /d %%D in ("%LOCALAPPDATA%\Arduino15\packages\esp32\tools\mklittlefs\*") do (
    if exist "%%~D\mklittlefs.exe" set "MKLITTLEFS=%%~D\mklittlefs.exe"
  )
)
if not defined MKLITTLEFS (
  for /f "delims=" %%P in ('where mklittlefs 2^>nul') do if not defined MKLITTLEFS set "MKLITTLEFS=%%P"
)
if not defined MKLITTLEFS (
  echo mklittlefs.exe not found. Install the ESP32 core in the Arduino IDE,
  echo or set MKLITTLEFS_EXE to the full path of mklittlefs.exe.
  set "FAILED=1"
  goto :end
)

rem --- 3. LittleFS size straight from partitions.csv (row "spiffs") -----------
set "FS_SIZE="
for /f "usebackq tokens=1,5 delims=, " %%A in ("partitions.csv") do (
  if /i "%%A"=="spiffs" set /a FS_SIZE=%%B
)
if not defined FS_SIZE (
  echo Could not read the spiffs partition size from partitions.csv.
  set "FAILED=1"
  goto :end
)

rem --- 4. Collect the release files --------------------------------------------
if not exist "%OUT%" mkdir "%OUT%"
del /q "%OUT%\*.bin" 2>nul

echo Building the web-page image (%FS_SIZE% bytes) with
echo   !MKLITTLEFS!
"!MKLITTLEFS!" -c data -p 256 -b 4096 -s %FS_SIZE% "%OUT%\HAWK370_littlefs.bin"
if errorlevel 1 (
  echo mklittlefs failed.
  set "FAILED=1"
  goto :end
)

copy /y "%BUILD%\%PROJECT%.bootloader.bin" "%OUT%\HAWK370_bootloader.bin" >nul || set "FAILED=1"
copy /y "%BUILD%\%PROJECT%.partitions.bin" "%OUT%\HAWK370_partitions.bin" >nul || set "FAILED=1"
copy /y "%BUILD%\boot_app0.bin"            "%OUT%\boot_app0.bin"          >nul || set "FAILED=1"
copy /y "%BUILD%\%PROJECT%.bin"            "%OUT%\HAWK370_firmware.bin"   >nul || set "FAILED=1"
if defined FAILED (
  echo Copying the build files failed.
  goto :end
)

echo.
echo Release files in %CD%\%OUT%:
for %%F in ("%OUT%\*.bin") do echo   %%~nxF  %%~zF bytes
echo.
echo Web installer: attach all five files to a GitHub Release.
echo OTA update:    drop HAWK370_littlefs.bin and HAWK370_firmware.bin together on the Files page.

:end
echo.
rem Keep the window open when the script was started by double-click.
set "CMDLINE=!cmdcmdline!"
if /i not "!CMDLINE:/c=!"=="!CMDLINE!" pause
if defined FAILED exit /b 1
exit /b 0

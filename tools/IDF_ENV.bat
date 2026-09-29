@echo off
rem ------------------------------------------------------------
rem CCCP R1A - ESP-IDF environment + ESP32-S3 toolchain bootstrap
rem Cardputer Communication Connector for Postpet
rem ------------------------------------------------------------

if defined IDF_PATH goto HAVE_IDF

if exist "G:\esp-idf-5.5.4\export.bat" (
    set "IDF_PATH=G:\esp-idf-5.5.4"
    goto HAVE_IDF
)

if exist "C:\Espressif\frameworks\esp-idf-v5.5.4\export.bat" (
    set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.4"
    goto HAVE_IDF
)

echo ERROR: IDF_PATH is not set and ESP-IDF 5.5.4 was not found.
echo Set IDF_PATH or install ESP-IDF 5.5.4.
exit /b 2

:HAVE_IDF
if not exist "%IDF_PATH%\export.bat" (
    echo ERROR: "%IDF_PATH%\export.bat" does not exist.
    exit /b 2
)

rem Lock this project to Cardputer ADV / ESP32-S3 even if sdkconfig is absent.
set "IDF_TARGET=esp32s3"

call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b %errorlevel%

where idf.py >nul 2>nul
if errorlevel 1 (
    echo ERROR: idf.py is not available after export.bat.
    exit /b 2
)

rem A P4-only ESP-IDF installation has the RISC-V toolchain but not the
rem Xtensa compiler required by Cardputer ADV / ESP32-S3. Install it once.
where xtensa-esp32s3-elf-gcc >nul 2>nul
if not errorlevel 1 goto TOOLCHAIN_READY

echo.
echo ============================================================
echo ESP32-S3 Xtensa toolchain is missing.
echo Installing ESP-IDF tools for esp32s3 - one-time setup.
echo ============================================================
call "%IDF_PATH%\install.bat" esp32s3
if errorlevel 1 (
    echo.
    echo ERROR: ESP32-S3 toolchain installation failed.
    echo Manual command:
    echo   call "%IDF_PATH%\install.bat" esp32s3
    exit /b 3
)

rem install.bat changes the installed tool set; export again so the new
rem xtensa-esp-elf/bin directory is added to this CMD session's PATH.
call "%IDF_PATH%\export.bat"
if errorlevel 1 exit /b %errorlevel%

:TOOLCHAIN_READY
where xtensa-esp32s3-elf-gcc >nul 2>nul
if errorlevel 1 (
    echo ERROR: xtensa-esp32s3-elf-gcc is still not available.
    echo Try once manually:
    echo   call "%IDF_PATH%\install.bat" esp32s3
    echo   call "%IDF_PATH%\export.bat"
    exit /b 4
)

for /f "delims=" %%V in ('xtensa-esp32s3-elf-gcc --version ^| findstr /R /C:"gcc" /C:"crosstool"') do (
    echo S3 toolchain: %%V
    goto VERSION_DONE
)
:VERSION_DONE
exit /b 0

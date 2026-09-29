@echo off
setlocal
cd /d "%~dp0.."

set "IDF_TARGET=esp32s3"

rem If sdkconfig already says ESP32-S3, there is nothing to change.
if exist "sdkconfig" (
    findstr /C:"CONFIG_IDF_TARGET=\"esp32s3\"" sdkconfig >nul 2>nul
    if not errorlevel 1 (
        echo ESP-IDF target: esp32s3 READY
        exit /b 0
    )
)

rem idf.py set-target always runs fullclean first. A previous failed CMake
rem configure can leave a partial build directory which ESP-IDF refuses to
rem fullclean because it is not a valid CMake build tree. It is safe to delete
rem that stale directory here because we are about to change/create the target.
if exist "build" (
    echo ============================================================
    echo Removing stale/pre-target build directory
    echo ============================================================
    rmdir /s /q "build"
    if exist "build" (
        echo ERROR: Could not remove build directory.
        echo Close any program using G:\cccp\build and retry.
        exit /b 6
    )
)

echo ============================================================
echo Setting ESP-IDF target: esp32s3
 echo ============================================================
idf.py set-target esp32s3
if errorlevel 1 exit /b %errorlevel%

findstr /C:"CONFIG_IDF_TARGET=\"esp32s3\"" sdkconfig >nul 2>nul
if errorlevel 1 (
    echo ERROR: sdkconfig was not created for esp32s3.
    exit /b 7
)

echo ESP-IDF target: esp32s3 READY
exit /b 0

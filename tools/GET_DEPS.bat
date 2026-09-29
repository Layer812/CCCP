@echo off
setlocal EnableExtensions
cd /d "%~dp0.."

set "NEED_FETCH=0"
if not exist "components\M5Cardputer\src\M5Cardputer.cpp" set "NEED_FETCH=1"
if exist "components\M5Cardputer\.git" (
  for /f "delims=" %%V in ('git -C "components\M5Cardputer" describe --tags --exact-match 2^>nul') do set "CARD_TAG=%%V"
  if /i not "%CARD_TAG%"=="1.1.1" set "NEED_FETCH=1"
)

if "%NEED_FETCH%"=="1" (
  where git >nul 2>nul || (echo ERROR: git.exe is required.& exit /b 3)
  echo ============================================================
  echo Fetching M5Cardputer 1.1.1 - vgmM5 baseline
  echo ============================================================
  if exist "components\M5Cardputer" rmdir /s /q "components\M5Cardputer"
  git clone --depth 1 --branch 1.1.1 https://github.com/m5stack/M5Cardputer.git "components\M5Cardputer"
  if errorlevel 1 exit /b 3
)

copy /y "tools\M5Cardputer.CMakeLists.txt" "components\M5Cardputer\CMakeLists.txt" >nul || exit /b 3
findstr /B /C:"# CCCP_R1S_M5CARDPUTER_111_IDF55_WRAPPER" "components\M5Cardputer\CMakeLists.txt" >nul 2>nul || exit /b 8

echo M5Cardputer dependency: READY - 1.1.1 / vgmM5 baseline
exit /b 0

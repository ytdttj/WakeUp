@echo off
setlocal
cd /d "%~dp0"

rem 用法：update.bat [新 exe 的路径]
rem 不传参数时，自动在 bin\ 下寻找 WakeGuardC_v*.exe

set "SRC=%~1"
if not defined SRC (
  for %%F in ("bin\WakeGuardC_v*.exe") do set "SRC=bin\%%~nxF"
)
if not defined SRC (
  echo Usage: update.bat ^<path-to-new-exe^>
  echo e.g.   update.bat bin\WakeGuardC_v12.exe
  pause
  exit /b 1
)
if not exist "%SRC%" (
  echo Update file not found: %SRC%
  pause
  exit /b 1
)

echo Stopping running WakeGuard...
taskkill /F /IM WakeGuardC.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul

echo Replacing executable: %SRC%
copy /Y "%SRC%" "bin\WakeGuardC.exe" >nul
if errorlevel 1 (
  echo Replace failed. Is WakeGuard still running?
  pause
  exit /b 1
)
del /F "%SRC%" >nul 2>&1

echo Starting WakeGuard...
start "" "bin\WakeGuardC.exe"

echo Done.
ping -n 3 127.0.0.1 >nul
endlocal

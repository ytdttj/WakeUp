@echo off
cd /d "%~dp0"

if not exist "bin\WakeGuardC_v11.exe" (
  echo Update file bin\WakeGuardC_v11.exe not found.
  pause
  exit /b 1
)

echo Stopping running WakeGuard...
taskkill /F /IM WakeGuardC.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul

echo Replacing executable...
copy /Y "bin\WakeGuardC_v11.exe" "bin\WakeGuardC.exe" >nul
if errorlevel 1 (
  echo Replace failed. Is WakeGuard still running?
  pause
  exit /b 1
)
del /F "bin\WakeGuardC_v11.exe" >nul 2>&1

echo Starting WakeGuard v1.1...
start "" "bin\WakeGuardC.exe"

echo Done.
ping -n 3 127.0.0.1 >nul

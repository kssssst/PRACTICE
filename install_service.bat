@echo off
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Run this script as Administrator.
    exit /b 1
)

set "SCRIPT_DIR=%~dp0"
set "SERVICE_PATH=%SCRIPT_DIR%TrayService.exe"
set "APP_PATH=%SCRIPT_DIR%TrayApp.exe"

if not exist "%SERVICE_PATH%" set "SERVICE_PATH=%SCRIPT_DIR%build\TrayService.exe"
if not exist "%APP_PATH%" set "APP_PATH=%SCRIPT_DIR%build\TrayApp.exe"

echo Installing TrayAppService...

if not exist "%SERVICE_PATH%" (
    echo ERROR: TrayService.exe not found in "%SCRIPT_DIR%".
    exit /b 1
)

if not exist "%APP_PATH%" (
    echo ERROR: TrayApp.exe not found in "%SCRIPT_DIR%".
    exit /b 1
)

echo Stopping existing service if it is running...
"%APP_PATH%" /stopservice >nul 2>&1
timeout /t 2 /nobreak >nul 2>&1

echo Removing existing service if it exists...
sc delete TrayAppService >nul 2>&1
timeout /t 1 /nobreak >nul 2>&1

echo Creating service from "%SERVICE_PATH%"...
sc create TrayAppService binPath= "\"%SERVICE_PATH%\"" start= auto
if %errorlevel% neq 0 (
    echo ERROR: Failed to create TrayAppService.
    exit /b 1
)

sc description TrayAppService "TrayApp Windows service"

echo Starting service...
sc start TrayAppService
if %errorlevel% neq 0 (
    echo ERROR: Failed to start TrayAppService.
    sc query TrayAppService
    exit /b 1
)

sc query TrayAppService
echo TrayAppService installed and started.

@echo off
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Run this script as Administrator.
    exit /b 1
)

set "SCRIPT_DIR=%~dp0"
set "APP_PATH=%SCRIPT_DIR%TrayApp.exe"

sc query TrayAppService >nul 2>&1
if %errorlevel% neq 0 (
    echo TrayAppService is not installed.
    exit /b 0
)

if exist "%APP_PATH%" (
    echo Stopping TrayAppService through RPC...
    "%APP_PATH%" /stopservice >nul 2>&1
    timeout /t 2 /nobreak >nul 2>&1
)

echo Removing TrayAppService...
sc delete TrayAppService
if %errorlevel% neq 0 (
    echo ERROR: Failed to remove TrayAppService.
    exit /b 1
)

echo TrayAppService removed.

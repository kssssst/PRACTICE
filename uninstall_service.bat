@echo off
REM Uninstall Windows Service
REM Run as Administrator

echo.
echo ========================================
echo TrayApp Uninstall Script
echo ========================================
echo.

REM Check for Administrator privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: This script must be run as Administrator!
    echo Please right-click Command Prompt and select "Run as administrator"
    pause
    exit /b 1
)

REM Check if service exists
sc query TrayAppService >nul 2>&1
if %errorlevel% neq 0 (
    echo Service TrayAppService is not installed.
    pause
    exit /b 0
)

echo Stopping service...
sc stop TrayAppService >nul 2>&1
timeout /t 2 /nobreak >nul 2>&1

echo Deleting service...
sc delete TrayAppService
if %errorlevel% neq 0 (
    echo ERROR: Failed to uninstall service!
    pause
    exit /b 1
)

echo.
echo ========================================
echo Service Uninstalled Successfully!
echo ========================================
echo.

pause

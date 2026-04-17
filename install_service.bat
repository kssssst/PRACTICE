@echo off
REM Installation script for TrayApp Service
REM Run as Administrator

setlocal enabledelayedexpansion

REM Check for Administrator privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: This script must be run as Administrator!
    echo Please right-click Command Prompt and select "Run as administrator"
    pause
    exit /b 1
)

REM Get the directory where this script is located
set SCRIPT_DIR=%~dp0
set SERVICE_PATH=%SCRIPT_DIR%TrayService.exe
set APP_PATH=%SCRIPT_DIR%TrayApp.exe

echo.
echo ========================================
echo TrayApp Installation Script
echo ========================================
echo.

REM Check if service binary exists
if not exist "%SERVICE_PATH%" (
    echo ERROR: Service binary not found at:
    echo   %SERVICE_PATH%
    echo.
    echo Please build the project first:
    echo   mkdir build
    echo   cd build
    echo   cmake -G "Visual Studio 17 2022" -A ARM64 ..
    echo   cmake --build . --config Release
    pause
    exit /b 1
)

REM Check if app binary exists
if not exist "%APP_PATH%" (
    echo ERROR: App binary not found at:
    echo   %APP_PATH%
    echo.
    echo Please build the project first.
    pause
    exit /b 1
)

echo Service binary: %SERVICE_PATH%
echo App binary:     %APP_PATH%
echo.

REM Stop existing service if running
echo Stopping existing service (if running)...
if exist "%APP_PATH%" (
    "%APP_PATH%" /stopservice >nul 2>&1
)

REM Wait a bit for service to stop
timeout /t 2 /nobreak >nul 2>&1

REM Remove existing service if it exists
echo Removing existing service (if exists)...
sc delete TrayAppService >nul 2>&1

REM Wait a bit
timeout /t 1 /nobreak >nul 2>&1

REM Install service
echo Installing service...
sc create TrayAppService binPath= "%SERVICE_PATH%" start= auto
if %errorlevel% neq 0 (
    echo ERROR: Failed to install service!
    pause
    exit /b 1
)

echo Service installed successfully!
echo.

REM Set service description
sc description TrayAppService "Tray Application Service - Manages TrayApp instances in user sessions"

REM Start service
echo Starting service...
sc start TrayAppService
if %errorlevel% neq 0 (
    echo WARNING: Service may not have started immediately.
    echo Use "sc query TrayAppService" to check status.
    echo.
)

echo.
timeout /t 2 /nobreak >nul 2>&1

REM Check service status
echo Checking service status...
sc query TrayAppService

echo.
echo ========================================
echo Installation Complete!
echo ========================================
echo.
echo Service Information:
echo   Name: TrayAppService
echo   Path: %SERVICE_PATH%
echo   Start Type: Auto
echo.
echo To manage the service:
echo   sc query TrayAppService     - Check status
echo   sc start TrayAppService     - Start service
echo   TrayApp.exe /stopservice    - Stop service
echo   sc delete TrayAppService    - Uninstall service
echo.
echo To run the application:
echo   %APP_PATH%
echo.
echo To remove the service:
echo   TrayApp.exe /stopservice
echo   sc delete TrayAppService
echo.

pause

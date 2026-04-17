@echo off
REM Install Windows Service - Run as Administrator

setlocal enabledelayedexpansion

REM Get the directory where the batch file is located
set SERVICE_DIR=%~dp0

REM Register the service
sc create TrayAppService binPath= "!SERVICE_DIR!TrayService.exe" start= auto

REM Set service description
sc description TrayAppService "Tray Application Service - Manages TrayApp instances in user sessions"

REM Start the service
net start TrayAppService

echo Service installed and started successfully
pause

@echo off
REM Uninstall Windows Service - Run as Administrator

REM Stop the service
net stop TrayAppService

REM Delete the service
sc delete TrayAppService

echo Service uninstalled successfully
pause

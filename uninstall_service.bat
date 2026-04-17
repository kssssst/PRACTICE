@echo off
sc query TrayAppService >nul 2>&1
if %errorlevel%==0 (
    net stop TrayAppService
    sc delete TrayAppService
    echo Service removed.
) else echo Service not found.
pause
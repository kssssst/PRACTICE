@echo off
sc query TrayAppService >nul 2>&1
if %errorlevel%==0 (
    if exist "%~dp0build\Release\TrayApp.exe" (
        "%~dp0build\Release\TrayApp.exe" /stopservice
    )
    sc delete TrayAppService
    echo Service removed.
) else echo Service not found.
pause

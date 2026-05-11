# TrayApp Installer

This folder contains the WiX-based installer project for TrayApp.

The MSI installs every file from `src/payload`, including `TrayApp.exe`, `TrayService.exe`, DLLs, resources, and configuration files copied from the runtime build output. It registers `TrayAppService` as an automatic Windows service, starts it after install, stops it through the Service Control Manager during uninstall, removes it from the Service Control Manager, and removes installed application files.

After installation, run the same `TrayAppSetup.msi` again to open maintenance mode and choose Repair or Remove. The application can also be removed from Windows Installed Apps.

## Dependencies

The current TrayApp build links the MSVC runtime statically and uses Windows platform APIs only, so there are no third-party runtime installers to chain at the moment. If future builds add runtime dependencies such as Qt, Windows App SDK, .NET, or VC++ redistributables, add them as WiX Burn packages or MSI merge modules so Windows Installer reference counting can avoid removing shared dependencies still used by other applications.

## Local build on Windows ARM64

From the repository root containing the application source:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A ARM64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

.\TrayAppInstaller\scripts\Prepare-Payload.ps1 -AppBuildBin .\build -PayloadDir .\TrayAppInstaller\src\payload
.\TrayAppInstaller\scripts\Generate-WixPayload.ps1 -PayloadDir .\TrayAppInstaller\src\payload -OutputPath .\TrayAppInstaller\src\Payload.generated.wxs

dotnet build .\TrayAppInstaller\src\TrayAppInstaller.wixproj -c Release -p:ProductVersion=1.0.0
```

The MSI is produced under `TrayAppInstaller\src\bin\Release`.

## CI

The repository workflow builds TrayApp for ARM64, generates the WiX payload from the build output, builds `TrayAppSetup.msi`, verifies that the application and service executables are included, and uploads that MSI as the `TrayAppInstaller-ARM64` artifact.

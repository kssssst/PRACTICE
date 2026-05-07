# TrayApp Installer

This folder is a standalone WiX-based installer project for TrayApp.

The MSI installs every file from `src/payload`, including `TrayApp.exe`, `TrayService.exe`, DLLs, resources, and configuration files copied from the runtime build output. It registers `TrayService` as an automatic Windows service, starts it after install, stops it before uninstall by signaling `Global\TrayServiceStopEvent`, removes the service from the Service Control Manager, and removes installed application files.

## Dependencies

The current TrayApp build links the MSVC runtime statically and uses Windows platform APIs only, so there are no third-party runtime installers to chain at the moment. If future builds add runtime dependencies such as Qt, Windows App SDK, .NET, or VC++ redistributables, add them as WiX Burn packages or MSI merge modules so Windows Installer reference counting can avoid removing shared dependencies still used by other applications.

## Local build on Windows ARM64

From the repository root containing the application source:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A ARM64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

.\TrayAppInstaller\scripts\Prepare-Payload.ps1 -AppBuildBin .\build\bin -PayloadDir .\TrayAppInstaller\src\payload
.\TrayAppInstaller\scripts\Generate-WixPayload.ps1 -PayloadDir .\TrayAppInstaller\src\payload -OutputPath .\TrayAppInstaller\src\Payload.generated.wxs

dotnet build .\TrayAppInstaller\src\TrayAppInstaller.wixproj -c Release -p:ProductVersion=1.0.0
```

The MSI is produced under `TrayAppInstaller\src\bin\Release`.

## CI

`.github/workflows/build.yml` is intentionally inside this standalone folder. When this folder is moved to a repository or branch that contains only the installer, keep it at the repository root.

The workflow checks out the installer and then checks out the TrayApp source from `APP_REPOSITORY` at `APP_REF` (default ref: the current workflow branch; `inst` for manual runs), builds TrayApp for ARM64, generates the WiX payload file, builds `TrayAppSetup.msi`, and uploads that MSI as `TrayAppInstaller-ARM64`.

For standalone installer repositories, configure the GitHub Actions repository variable `APP_REPOSITORY` with the real TrayApp source repository in `owner/repository` format, or pass `app_repository` when running the workflow manually.

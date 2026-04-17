# GitHub Actions Pipeline Configuration

## Overview

The pipeline (`workflows/build.yml`) automatically builds both the GUI application and Windows service, verifies the artifacts, and uploads them as release packages.

## Pipeline Triggers

- ✅ On every push to any branch
- ✅ On pull requests to main/master/develop

## Build Steps

### 1. Checkout Code
```yaml
uses: actions/checkout@v3
```
Clones the repository

### 2. Install CMake
```bash
choco install cmake --installargs 'ADD_CMAKE_TO_PATH=System' -y
```
Installs latest CMake via Chocolatey

### 3. Configure CMake
```bash
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
```
Generates Visual Studio project files for x64 architecture

### 4. Build Project
```bash
cd build
cmake --build . --config Release --verbose
```
Builds both:
- `build/Release/TrayApp.exe` - GUI Application
- `build/service/Release/TrayService.exe` - Windows Service

### 5. Verify Artifacts
```powershell
Test-Path "build\Release\TrayApp.exe"
Test-Path "build\service\Release\TrayService.exe"
```
Confirms both executables were built successfully

### 6. Upload Individual Artifacts
Uploads each executable separately:
- **TrayApp** → `build/Release/TrayApp.exe`
- **TrayService** → `build/service/Release/TrayService.exe`

Retention: 90 days

### 7. Create Release Package
Bundles everything together:
- TrayApp.exe
- TrayService.exe
- install_service.bat
- uninstall_service.bat
- README.md
- BUILD.md
- IMPLEMENTATION_GUIDE.md

### 8. Upload Complete Package
Uploads `TrayApp-Complete-Package` with all files

Retention: 90 days

## Artifacts Generated

### Individual Artifacts
1. **TrayApp** - GUI executable only
2. **TrayService** - Service executable only
3. **TrayApp-Complete-Package** - Full ready-to-deploy package

### Download Locations
In GitHub Actions:
1. Go to Actions tab
2. Select latest workflow run
3. Scroll to "Artifacts" section
4. Download desired artifact

### Package Contents

#### TrayApp-Complete-Package includes:
```
TrayApp.exe
TrayService.exe
install_service.bat
uninstall_service.bat
README.md
BUILD.md
IMPLEMENTATION_GUIDE.md
```

Everything needed for immediate deployment!

## Build Matrix

**Simplified for production:**
- Config: Release only (Debug removed)
- Platform: x64 only
- OS: windows-latest (Windows 2022)

## Environment

- **OS**: Windows Server 2022 (windows-latest)
- **Visual Studio**: 2022 Community Edition
- **CMake**: Latest (auto-installed)
- **Architecture**: x64

## Key Features

✅ **Automatic CMake Generation** - MIDL compiles RPC files
✅ **Artifact Verification** - Confirms both EXEs exist before upload
✅ **Multi-Artifact Upload** - Individual + complete package
✅ **Long Retention** - 90 days for all artifacts
✅ **Verbose Build Output** - Easy troubleshooting
✅ **PowerShell Steps** - Native Windows scripting
✅ **Error Handling** - Fails if artifacts missing

## Troubleshooting

### Build Failed: CMake configuration error
1. Check CMakeLists.txt syntax
2. Verify MIDL is available in pipeline
3. Check build log for specific errors

### Build Failed: Compilation errors
1. View build log in GitHub Actions
2. Look for line numbers and file paths
3. Fix issues locally first, then push

### Missing Artifacts
1. Check "Verify Build Artifacts" step output
2. Ensure TrayApp.exe and TrayService.exe exist in correct paths
3. Review CMake build output

### RPC Compilation Failed
1. MIDL should be available automatically
2. Check IDL files are correct
3. Verify CMake RPC commands in CMakeLists.txt

## Running Locally (Equivalent)

To replicate pipeline locally:
```bash
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
```

Then check:
```powershell
Test-Path "build\Release\TrayApp.exe"
Test-Path "build\service\Release\TrayService.exe"
```

## Security Considerations

- ✅ No secrets exposed
- ✅ Only artifact upload to GitHub
- ✅ No external deployments
- ✅ Safe for public repositories

## Future Enhancements

Potential additions:
- Code signing of executables
- Release notes generation
- Automatic GitHub releases
- Integration testing
- Code coverage reporting
- Performance benchmarking

## CI/CD Integration

The pipeline:
- Runs on every push (validation)
- Runs on PR to main/master/develop (quality gate)
- Automatically uploads artifacts
- No manual intervention needed

Ready for production deployment!

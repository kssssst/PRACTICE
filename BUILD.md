# Build Instructions for TrayApp

## System Requirements

- **OS**: Windows 10 or later (x64)
- **Visual Studio**: 2019 or 2022 Community/Professional
- **Components**:
  - Visual C++ build tools
  - Windows SDK (10.0.19041.0 or later)
  - CMake (3.20 or later)
  - MIDL compiler (included in Visual Studio)

## Prerequisites Check

### 1. Verify CMake is installed
```bash
cmake --version
```
Should show version 3.20 or higher.

### 2. Verify Visual Studio is installed
Check that you have MSVC toolchain available:
```bash
where cl.exe
where midl.exe
```

### 3. Verify Windows SDK
Open "Apps & Features" and ensure "Windows SDK" is installed.

## Build Steps

### Step 1: Navigate to project directory
```bash
cd "C:\Users\ksu\Desktop\Приложение"
```

### Step 2: Create build directory
```bash
mkdir build
cd build
```

### Step 3: Generate build files with CMake
```bash
cmake -G "Visual Studio 16 2019" -A x64 ..
```

Or for Visual Studio 2022:
```bash
cmake -G "Visual Studio 17 2022" -A x64 ..
```

**Expected output:**
```
-- Configuring done
-- Generating done
```

### Step 4: Build the project
```bash
cmake --build . --config Release
```

Or use MSBuild directly:
```bash
msbuild TrayApp.sln /p:Configuration=Release /p:Platform=x64
```

### Step 5: Verify build artifacts
Check that these files were created:

```
build/Release/TrayApp.exe
build/service/Release/TrayService.exe
```

## Installation

### Method 1: Using Installation Script (Recommended)

Run as Administrator:
```bash
cd "C:\Users\ksu\Desktop\Приложение"
install_service.bat
```

This script will:
1. Verify Administrator privileges
2. Check binary existence
3. Stop any existing service
4. Install new service
5. Start the service
6. Display status

### Method 2: Manual Installation

1. **Open Command Prompt as Administrator**

2. **Stop existing service (if any)**:
   ```bash
   sc stop TrayAppService
   ```

3. **Delete existing service (if any)**:
   ```bash
   sc delete TrayAppService
   ```

4. **Install service**:
   ```bash
   sc create TrayAppService binPath= "C:\Users\ksu\Desktop\Приложение\build\service\Release\TrayService.exe" start= auto
   ```

5. **Set service description**:
   ```bash
   sc description TrayAppService "Tray Application Service - Manages TrayApp instances in user sessions"
   ```

6. **Start service**:
   ```bash
   sc start TrayAppService
   ```

7. **Verify service status**:
   ```bash
   sc query TrayAppService
   ```

## Running the Application

### Via GUI
Simply double-click `build\Release\TrayApp.exe`

### Via Command Line
```bash
cd build\Release
TrayApp.exe
```

### With Hidden Window (for service launch)
```bash
TrayApp.exe /hidden
```

## Testing

### 1. Test Single Instance
- Run TrayApp.exe
- Try to run it again - should show existing instance
- Close the window (should hide, not close)
- Left-click tray icon - should show window

### 2. Test Tray Functionality
- Right-click tray icon - context menu should appear
  - "Открыть" should show window
  - "Выход" should stop service and close

### 3. Test File Menu
- Open application
- Click File → Выход
- Service should stop and app should close

### 4. Test Taskbar Recreation
- Restart Windows Explorer
- Tray icon should automatically reappear

### 5. Test Service
- Check service status: `sc query TrayAppService`
- Start service: `sc start TrayAppService`
- Stop service: `sc stop TrayAppService`
- Check Event Viewer for any errors

## Troubleshooting

### Build Fails: CMake not found
```bash
# Install CMake from https://cmake.org/download/
# Then add to PATH or use full path
"C:\Program Files\CMake\bin\cmake.exe" --version
```

### Build Fails: MIDL not found
```bash
# MIDL is part of Visual Studio
# Ensure C++ build tools are installed
# Or run from "Visual Studio Developer Command Prompt"
```

### Build Fails: Cannot open source files
- Ensure all files are present
- Check file encoding (should be UTF-8)
- Verify path contains no special characters

### Service Installation Fails
- Run Command Prompt as Administrator
- Check service name isn't already taken: `sc query TrayAppService`
- Check file path is correct and executable exists
- View error log: `eventvwr.msc`

### Service Doesn't Start
1. Check service status: `sc query TrayAppService`
2. View error details: Open Event Viewer → Windows Logs → System
3. Look for "TrayAppService" entries
4. Common issues:
   - Service binary path is incorrect
   - User account lacks required permissions
   - Port/endpoint conflict (restart may help)

### RPC Connection Failed
- Ensure service is running: `sc query TrayAppService`
- Check status shows "RUNNING"
- Verify ALPC transport: `netsh rpc show settings`
- Try restarting service: `sc stop TrayAppService`, then `sc start TrayAppService`

### Tray Icon Not Showing
- Check Windows Explorer isn't crashed
- Right-click taskbar and toggle "Show system tray icons"
- Ensure TrayApp.exe is running: Check Task Manager
- Restart Explorer: `taskkill /f /im explorer.exe` then `explorer.exe`

## Clean Build

To do a clean rebuild:

```bash
cd "C:\Users\ksu\Desktop\Приложение"
rmdir /s /q build
mkdir build
cd build
cmake -G "Visual Studio 16 2019" -A x64 ..
cmake --build . --config Release
```

## Build Output Locations

After successful build:

| File | Location | Purpose |
|------|----------|---------|
| TrayApp.exe | `build\Release\TrayApp.exe` | Main GUI application |
| TrayService.exe | `build\service\Release\TrayService.exe` | Windows service |
| TrayService_c.c | `build\rpc\TrayService_c.c` | RPC client stub (generated) |
| TrayService_s.c | `build\service\rpc\TrayService_s.c` | RPC server stub (generated) |
| TrayService.h | `build\rpc\TrayService.h` | RPC interface header (generated) |

## CMake Configuration Details

### Main CMakeLists.txt
- Generates RPC client from IDL
- Configures TrayApp.exe with static runtime linking
- Sets UTF-8 compiler flags
- Links required libraries (shell32, rpcrt4, etc.)

### Service/CMakeLists.txt
- Generates RPC server from IDL
- Configures TrayService.exe
- Enables Windows API components (WTS, userenv)
- Compiles as console application

### RPC Configuration
- MIDL is automatically invoked by CMake
- Generates from: `service/src/TrayService.idl`
- Client stub: `TrayService_c.c` (used in TrayApp)
- Server stub: `TrayService_s.c` (used in TrayService)

## Release Configuration

The build is configured for Release optimization:
- `/O2` - Maximum optimization
- `/MT` - Static runtime linking
- `/utf-8` - UTF-8 source file encoding
- Optimized executable size (~500KB-1MB total)

To build Debug version:
```bash
cmake --build . --config Debug
```

## Deployment

To deploy built binaries:

1. Copy `build\Release\TrayApp.exe` to target machine
2. Copy `build\service\Release\TrayService.exe` to same directory
3. Run `install_service.bat` as Administrator
4. Service will start automatically on system boot

## Windows Firewall

No firewall rules needed - app uses only local ALPC communication.

## Windows Defender

Add exclusions if needed:
- `TrayApp.exe`
- `TrayService.exe`

Both are safe local-only applications.

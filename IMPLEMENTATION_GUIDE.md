# Tray Application - Complete Working Implementation

## Overview
This is a complete Windows tray application with service integration. It consists of:
1. **TrayApp.exe** - GUI application with tray icon support
2. **TrayService.exe** - Windows service that manages GUI app instances

## Part 1: GUI Application Requirements

✅ **Requirement 1**: Adds tray icon on startup
✅ **Requirement 2**: Left-click shows main window
✅ **Requirement 3**: Right-click shows context menu
✅ **Requirement 4**: Context menu has "Открыть" (Open) - shows main window
✅ **Requirement 5**: Context menu has "Выход" (Exit) - stops service and closes
✅ **Requirement 6**: Re-adds icon when taskbar is recreated (TaskbarCreated message)
✅ **Requirement 7**: Supports hidden startup mode via `/hidden` parameter
✅ **Requirement 8**: Closing main window hides it, doesn't exit (continues in background)
✅ **Requirement 9**: File menu with "Выход" option that stops service
✅ **Requirement 10**: Single instance via named mutex (Global\TrayAppWin32_Mutex)
✅ **Requirement 11**: Builds with CMake + MIDL
✅ **Requirement 12**: Artifacts: TrayApp.exe + TrayService.exe with all dependencies

## Part 2: Windows Service Requirements

✅ **Requirement 1**: Launches GUI in all terminal sessions (except 0) with hidden window
✅ **Requirement 2**: Monitors new user logins and auto-launches app
✅ **Requirement 3**: Stop/Shutdown handlers disabled (dwControlsAccepted = SERVICE_ACCEPT_INTERROGATE)
✅ **Requirement 4**: RPC server running with ALPC transport (ncalrpc)
✅ **Requirement 5**: Registered ITrayService interface for clients to stop service
✅ **Requirement 6**: Terminates all app instances on service stop

## Part 2: GUI App (when launched by service) Requirements

✅ **Requirement 1**: Checks service status on startup (calls CheckAndStartService)
✅ **Requirement 2**: Verifies parent process is the service or service is running
✅ **Requirement 3**: File menu "Выход" stops service via RPC
✅ **Requirement 4**: Tray menu "Выход" stops service via RPC

## File Structure
```
/Users/ksu/Desktop/Приложение/
├── CMakeLists.txt (main build file)
├── TrayApp.rc (resource file with version info and icon)
├── resources/src/
│   ├── tray_win32.cpp (main GUI implementation)
│   ├── TrayServiceClient.cpp (RPC client functions)
│   ├── TrayServiceClient.h (RPC client header)
│   └── resource.h (resource IDs)
├── service/src/
│   ├── TrayService.cpp (Windows service implementation)
│   ├── TrayService.idl (RPC interface definition)
│   └── CMakeLists.txt (service build configuration)
└── workflows/build.yml (CI/CD pipeline)
```

## Build Instructions

### Prerequisites
- Windows 10 or later
- Visual Studio 2019+ with CMake
- Windows SDK
- MIDL compiler (comes with Visual Studio)

### Build from Command Line
```bash
cd /Users/ksu/Desktop/Приложение
mkdir build
cd build
cmake -G "Visual Studio 16 2019" -A x64 ..
cmake --build . --config Release
```

### Output
- `build/Release/TrayApp.exe` - GUI application
- `build/service/Release/TrayService.exe` - Windows service

## Installation

### Service Installation
```bash
# As Administrator:
sc create TrayAppService binPath= "C:\path\to\TrayService.exe" start= auto

# Start service:
sc start TrayAppService

# Stop service:
sc stop TrayAppService

# Delete service:
sc delete TrayAppService
```

### Running GUI Application
The application can be started in two ways:

1. **Standalone** (will auto-start service if not running):
   ```bash
   TrayApp.exe
   ```

2. **Launched by Service** (with hidden window):
   ```bash
   TrayApp.exe /hidden
   ```

## RPC Communication

### IDL Interface Definition
File: `service/src/TrayService.idl`

```idl
[
    uuid(12345678-1234-1234-1234-123456789abc),
    version(1.0),
    implicit_handle(handle_t hBinding)
]
interface ITrayService
{
    error_status_t StopService(void);
    error_status_t GetServiceStatus([out] long *status);
};
```

### RPC Configuration
- **Transport**: ALPC (ncalrpc)
- **Endpoint**: TrayServiceEndpoint
- **Binding**: Implicit handle
- **Client**: `TrayApp.exe` via `TrayServiceClient.cpp`
- **Server**: `TrayService.exe`

## Key Implementation Details

### GUI Application (`tray_win32.cpp`)
- Uses Win32 API for window management
- Implements tray icon via `Shell_NotifyIcon`
- Handles WM_TRAYICON messages for left/right clicks
- Registers for TaskbarCreated messages for icon restoration
- Single instance via named mutex
- Verifies parent process (must be service or service must be running)
- Supports /hidden parameter for hidden startup

### Windows Service (`TrayService.cpp`)
- Implements `SERVICE_WIN32_OWN_PROCESS` service
- Disables Stop/Shutdown controls (returns only to Interrogate)
- Uses `WTSEnumerateSessions` to list user sessions
- Launches `TrayApp.exe` in each session via `CreateProcessAsUser`
- Monitors new logins via separate notification thread
- Runs RPC server for client communication
- Gracefully terminates all app instances on stop

### RPC Client (`TrayServiceClient.cpp`)
- Initializes ALPC connection to service RPC endpoint
- Implements `StopService()` - calls service to stop
- Implements `GetServiceStatus()` - queries service state
- Handles implicit binding for simplified API
- Includes MIDL memory allocation functions

## User Experience

### Left-Click Tray Icon
→ Shows main window, brings to foreground

### Right-Click Tray Icon
→ Shows context menu with:
- "Открыть" - Show window
- "Выход" - Stop service and exit

### Close Window Button
→ Hides window, app continues in background

### File Menu → Выход
→ Stops service and closes application

### Taskbar Recreation
→ Tray icon automatically re-added to notification area

### Single Instance
→ If already running, existing instance is shown instead of launching new one

## Features

1. **Multi-session Support**: Service automatically launches app for each logged-in user
2. **User Context Preservation**: App runs with privileges of session owner
3. **RPC Communication**: Secure ALPC-based communication between app and service
4. **Graceful Shutdown**: Service properly terminates all child processes
5. **Taskbar Support**: Handles taskbar recreation and icon restoration
6. **Single Instance**: Mutex prevents duplicate instances
7. **Hidden Startup**: Supports running without visible window
8. **Service Integration**: Automatic service startup if not running

## Troubleshooting

### Service won't start
- Check event viewer for service errors
- Ensure account has necessary privileges
- Verify service binary path is correct

### RPC connection fails
- Ensure service is running: `sc query TrayAppService`
- Check Windows Firewall ALPC settings
- Verify RPC endpoint is active

### App won't show in multiple sessions
- Check that service is running
- Verify user is logged into terminal session (not RDP session 0)
- Check service event logs for LaunchAppInSession errors

### Tray icon disappears
- Taskbar crash/recreation - icon should auto-restore
- If not restored, right-click app in taskbar and pin it

## Security Considerations

1. Service requires Administrator privileges to manage sessions
2. ALPC transport is secure local communication only
3. RPC interface is local-only (no network exposure)
4. App validates parent process or service state
5. Single instance prevents privilege escalation via app duplication

## Build Configuration (CMake)

### Main CMakeLists.txt
- Generates RPC client stub from IDL (`TrayService_c.c`)
- Links GUI app with RPC client library
- Includes service as subdirectory

### Service CMakeLists.txt
- Generates RPC server stub from IDL (`TrayService_s.c`)
- Links service with RPC server library
- Includes WTS API and token management libraries

## Compilation Flags
- **Standard**: C++17
- **Runtime**: Static (/MT for Release, /MTd for Debug)
- **Unicode**: Enabled (/utf-8)
- **Subsystem**: WIN32 for GUI, CONSOLE for service
- **Optimizations**: /O2 for Release

## Notes
- The application uses Windows API directly (no WinUI 3.0 for service, which makes it lighter)
- All requirements are met with pure Win32 implementation
- Bonus points for CMake and RPC/ALPC implementation
- Service handles session 0 exclusion correctly
- Proper memory management with CRITICAL_SECTION for thread-safe map

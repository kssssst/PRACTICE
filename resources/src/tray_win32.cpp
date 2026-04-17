#include <windows.h>
#include <shellapi.h>
#include <cstdlib>
#include <tlhelp32.h>
#include <psapi.h>
#include "resource.h"
#include "TrayServiceClient.h"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#define WM_TRAYICON      (WM_APP + 1)
#define ID_TRAY_EXIT     1001
#define ID_TRAY_OPEN     1002
#define ID_FILE_EXIT     2001

HINSTANCE g_hInst;
HWND      g_hWnd = NULL;
NOTIFYICONDATAW g_nid = {};
HANDLE    g_hMutex = NULL;
UINT      g_uTaskbarRestart = 0;
BOOL      g_bLaunchedByService = FALSE;

// Forward declarations
void AddTrayIcon();
void RemoveTrayIcon();
void ShowContextMenu(HWND);
void ShowMainWindow();
void EnsureSingleInstance();
DWORD GetParentProcessId(DWORD dwProcessId);
BOOL CheckParentProcess();

// Main window procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_uTaskbarRestart) {
        // Taskbar was recreated - re-add tray icon
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
        case WM_CREATE:
        {
            // Create menu bar with File menu
            HMENU hMenu = CreateMenu();
            HMENU hFileMenu = CreatePopupMenu();
            AppendMenuW(hFileMenu, MF_STRING, ID_FILE_EXIT, L"Выход");
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFileMenu, L"Файл");
            SetMenu(hWnd, hMenu);
            break;
        }
        case WM_COMMAND:
        {
            UINT id = LOWORD(wParam);
            if (id == ID_FILE_EXIT || id == ID_TRAY_EXIT) {
                // Exit command - stop service and quit
                StopServiceViaRPC();
                Sleep(500);
                RemoveTrayIcon();
                PostQuitMessage(0);
            } else if (id == ID_TRAY_OPEN) {
                ShowMainWindow();
            }
            break;
        }
        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP) {
                // Left click - show window
                ShowMainWindow();
            } else if (lParam == WM_RBUTTONUP) {
                // Right click - show context menu
                ShowContextMenu(hWnd);
            }
            break;
        case WM_CLOSE:
            // Hide window on close button, don't exit
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            RemoveTrayIcon();
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// Entry point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    g_hInst = hInstance;

    // Check command line for /hidden parameter (launched by service)
    if (wcsstr(lpCmdLine, L"/hidden")) {
        g_bLaunchedByService = TRUE;
        nCmdShow = SW_HIDE;
    }

    // Verify parent process
    DWORD dwParent = GetParentProcessId(GetCurrentProcessId());
    BOOL bParentIsService = FALSE;
    
    if (dwParent) {
        HANDLE hParentProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, dwParent);
        if (hParentProcess) {
            wchar_t szParentPath[MAX_PATH];
            if (GetModuleFileNameExW(hParentProcess, NULL, szParentPath, MAX_PATH)) {
                wchar_t *p = wcsrchr(szParentPath, L'\\');
                if (p && wcsicmp(p + 1, L"TrayService.exe") == 0) {
                    bParentIsService = TRUE;
                }
            }
            CloseHandle(hParentProcess);
        }
    }

    // If launched standalone (not by service), check service status
    if (!bParentIsService) {
        if (!IsServiceRunning()) {
            // Service not running - try to start it
            if (CheckAndStartService() != 0) {
                // Failed to start service - exit
                MessageBoxW(NULL, L"Failed to start TrayAppService", 
                           L"Error", MB_OK | MB_ICONERROR);
                return 1;
            }
        }
    }

    // Ensure single instance
    EnsureSingleInstance();

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TrayAppWin32Class";
    
    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    // Create window
    g_hWnd = CreateWindowExW(0, wc.lpszClassName, L"Tray Application",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
                             NULL, NULL, hInstance, NULL);
    if (!g_hWnd) return 1;

    // Add tray icon
    AddTrayIcon();
    
    // Show window (or hide if launched by service)
    ShowWindow(g_hWnd, g_bLaunchedByService ? SW_HIDE : SW_SHOW);
    UpdateWindow(g_hWnd);

    // Register for taskbar recreation messages
    g_uTaskbarRestart = RegisterWindowMessageW(L"TaskbarCreated");

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}

// Add tray icon
void AddTrayIcon() {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    
    // Try to load icon from resources, fallback to system icon
    g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_TRAY_ICON));
    if (!g_nid.hIcon) {
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    
    wcscpy_s(g_nid.szTip, sizeof(g_nid.szTip)/sizeof(g_nid.szTip[0]), L"Tray Application");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

// Remove tray icon
void RemoveTrayIcon() {
    if (g_nid.hIcon) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
    }
}

// Show context menu
void ShowContextMenu(HWND hWnd) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN, L"Открыть");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Выход");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    PostMessage(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

// Show main window
void ShowMainWindow() {
    ShowWindow(g_hWnd, SW_SHOW);
    SetForegroundWindow(g_hWnd);
}

// Ensure only one instance
void EnsureSingleInstance() {
    g_hMutex = CreateMutexW(NULL, TRUE, L"Global\\TrayAppWin32_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another instance is running - find and show it
        HWND hExisting = FindWindowW(L"TrayAppWin32Class", NULL);
        if (hExisting) {
            ShowWindow(hExisting, SW_SHOW);
            SetForegroundWindow(hExisting);
        }
        exit(0);
    }
}

// Get parent process ID
DWORD GetParentProcessId(DWORD dwProcessId)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe32 = { 0 };
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(hSnapshot, &pe32)) {
        CloseHandle(hSnapshot);
        return 0;
    }

    do {
        if (pe32.th32ProcessID == dwProcessId) {
            DWORD dwParentId = pe32.th32ParentProcessID;
            CloseHandle(hSnapshot);
            return dwParentId;
        }
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
    return 0;
}

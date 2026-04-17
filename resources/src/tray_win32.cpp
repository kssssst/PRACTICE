#include <windows.h>
#include <shellapi.h>
#include <cstdlib>
#include <tlhelp32.h>
#include <psapi.h>
#include "resource.h"
#include "TrayServiceClient.h"

#pragma comment(lib, "psapi.lib")

#define WM_TRAYICON      (WM_USER + 1)
#define ID_TRAY_EXIT     1001
#define ID_TRAY_OPEN     1002
#define ID_FILE_EXIT     2001

HINSTANCE g_hInst;
HWND      g_hWnd = NULL;
NOTIFYICONDATAW g_nid = {};
HANDLE    g_hMutex = NULL;
UINT      g_uTaskbarRestart = 0;

void AddTrayIcon();
void RemoveTrayIcon();
void ShowContextMenu(HWND);
void ShowMainWindow();
void EnsureSingleInstance();
DWORD GetParentProcessId(DWORD dwProcessId);
int CheckServiceOnStartup();

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_uTaskbarRestart) {
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
        case WM_CREATE:
        {
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
                if (StopServiceViaRPC() == 0) {
                    Sleep(500); // Give service time to shutdown
                }
                RemoveTrayIcon();
                PostQuitMessage(0);
            } else if (id == ID_TRAY_OPEN) {
                ShowMainWindow();
            }
            break;
        }
        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP) {
                ShowMainWindow();
            } else if (lParam == WM_RBUTTONUP) {
                ShowContextMenu(hWnd);
            }
            break;
        case WM_CLOSE:
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    g_hInst = hInstance;
    
    // Check if parent process is the service
    DWORD dwParent = GetParentProcessId(GetCurrentProcessId());
    HANDLE hParentProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, dwParent);
    if (hParentProcess) {
        wchar_t szParentPath[MAX_PATH];
        if (GetModuleFileNameExW(hParentProcess, NULL, szParentPath, MAX_PATH)) {
            wchar_t *p = wcsrchr(szParentPath, L'\\');
            if (!p || wcsicmp(p + 1, L"TrayService.exe") != 0) {
                CloseHandle(hParentProcess);
                exit(1); // Parent is not the service
            }
        }
        CloseHandle(hParentProcess);
    } else {
        // If we can't verify, check if service is running
        if (!IsServiceRunning()) {
            exit(1); // Service is not running, exit
        }
    }
    
    // Check and start service if needed
    if (!IsServiceRunning()) {
        CheckAndStartService();
    }
    
    EnsureSingleInstance();

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TrayAppWin32Class";
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(0, wc.lpszClassName, L"Tray Application",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
                             NULL, NULL, hInstance, NULL);
    if (!g_hWnd) return 1;

    AddTrayIcon();
    ShowWindow(g_hWnd, SW_HIDE);
    UpdateWindow(g_hWnd);

    g_uTaskbarRestart = RegisterWindowMessageW(L"TaskbarCreated");

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}

void AddTrayIcon() {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    
    // Пытаемся загрузить иконку из ресурсов, если не найдена - используем системную иконку
    g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_TRAY_ICON));
    if (!g_nid.hIcon) {
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    
    wcscpy_s(g_nid.szTip, L"Tray Application");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

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

void ShowMainWindow() {
    ShowWindow(g_hWnd, SW_SHOW);
    SetForegroundWindow(g_hWnd);
}

void EnsureSingleInstance() {
    g_hMutex = CreateMutexW(NULL, TRUE, L"Global\\TrayAppWin32_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        exit(0);
    }
}

// Get parent process ID
DWORD GetParentProcessId(DWORD dwProcessId)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(hSnapshot, &pe32)) {
        CloseHandle(hSnapshot);
        return 0;
    }

    do {
        if (pe32.th32ProcessID == dwProcessId) {
            CloseHandle(hSnapshot);
            return pe32.th32ParentProcessID;
        }
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
    return 0;
}

// Check service on startup
int CheckServiceOnStartup()
{
    if (IsServiceRunning()) {
        return 1;
    }

    if (CheckAndStartService() == 0) {
        return 1;
    }

    return 0;
}
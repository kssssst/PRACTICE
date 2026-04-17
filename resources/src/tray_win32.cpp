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

void AddTrayIcon();
void RemoveTrayIcon();
void ShowContextMenu(HWND);
void ShowMainWindow();
void EnsureSingleInstance();
DWORD GetParentProcessId(DWORD dwProcessId);

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_uTaskbarRestart) {
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_CREATE: {
            HMENU hMenu = CreateMenu();
            HMENU hFileMenu = CreatePopupMenu();
            AppendMenuW(hFileMenu, MF_STRING, ID_FILE_EXIT, L"Выход");
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFileMenu, L"Файл");
            SetMenu(hWnd, hMenu);
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_FILE_EXIT || LOWORD(wParam) == ID_TRAY_EXIT) {
                StopServiceViaRPC();
                RemoveTrayIcon();
                PostQuitMessage(0);
            } else if (LOWORD(wParam) == ID_TRAY_OPEN) {
                ShowMainWindow();
            }
            break;
        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP) ShowMainWindow();
            else if (lParam == WM_RBUTTONUP) ShowContextMenu(hWnd);
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    g_hInst = hInstance;
    if (wcsstr(lpCmdLine, L"/hidden")) {
        g_bLaunchedByService = TRUE;
        nCmdShow = SW_HIDE;
    }

    // Проверка родительского процесса
    DWORD dwParent = GetParentProcessId(GetCurrentProcessId());
    BOOL bParentIsService = FALSE;
    if (dwParent) {
        HANDLE hParent = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, dwParent);
        if (hParent) {
            wchar_t szPath[MAX_PATH];
            if (GetModuleFileNameExW(hParent, NULL, szPath, MAX_PATH)) {
                wchar_t *p = wcsrchr(szPath, L'\\');
                if (p && wcsicmp(p+1, L"TrayService.exe") == 0) bParentIsService = TRUE;
            }
            CloseHandle(hParent);
        }
    }

    if (!bParentIsService) {
        // Запущено вручную – только запускаем службу и выходим
        if (!IsServiceRunning()) {
            if (CheckAndStartService() != 0) {
                MessageBoxW(NULL, L"Не удалось запустить службу TrayAppService", L"Ошибка", MB_OK|MB_ICONERROR);
                return 1;
            }
        }
        // Завершаем работу приложения (требование: запустить службу и закрыться)
        return 0;
    }

    // Запущено службой – нормальная работа
    EnsureSingleInstance();

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = L"TrayAppWin32Class";
    if (!RegisterClassExW(&wc)) return 1;

    g_hWnd = CreateWindowExW(0, wc.lpszClassName, L"Tray Application",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             400, 300, NULL, NULL, hInstance, NULL);
    if (!g_hWnd) return 1;

    AddTrayIcon();
    ShowWindow(g_hWnd, nCmdShow);
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
    g_nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_TRAY_ICON));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Tray Application");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    if (g_nid.hIcon) Shell_NotifyIconW(NIM_DELETE, &g_nid);
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
        HWND hExisting = FindWindowW(L"TrayAppWin32Class", NULL);
        if (hExisting) {
            ShowWindow(hExisting, SW_SHOW);
            SetForegroundWindow(hExisting);
        }
        exit(0);
    }
}

DWORD GetParentProcessId(DWORD dwProcessId) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe32 = { sizeof(PROCESSENTRY32W) };
    if (Process32FirstW(hSnapshot, &pe32)) {
        do if (pe32.th32ProcessID == dwProcessId) {
            DWORD pid = pe32.th32ParentProcessID;
            CloseHandle(hSnapshot);
            return pid;
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    return 0;
}
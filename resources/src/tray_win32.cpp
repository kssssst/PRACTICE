#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winsvc.h>

#include "resource.h"
#include "TrayServiceClient.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace
{
constexpr UINT kTrayIconMessage = WM_APP + 1;
constexpr UINT kTrayExitId = 1001;
constexpr UINT kTrayOpenId = 1002;
constexpr UINT kFileExitId = 2001;
constexpr wchar_t kWindowClassName[] = L"TrayAppWin32Class";
constexpr wchar_t kWindowTitle[] = L"Tray Application";
constexpr wchar_t kServiceName[] = L"TrayAppService";

HINSTANCE g_instanceHandle = nullptr;
HWND g_mainWindow = nullptr;
NOTIFYICONDATAW g_trayIconData = {};
HANDLE g_singleInstanceMutex = nullptr;
UINT g_taskbarRestartMessage = 0;
bool g_startHidden = false;

DWORD GetParentProcessId(DWORD processId)
{
    HANDLE snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshotHandle == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    DWORD parentProcessId = 0;
    if (Process32FirstW(snapshotHandle, &entry))
    {
        do
        {
            if (entry.th32ProcessID == processId)
            {
                parentProcessId = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshotHandle, &entry));
    }

    CloseHandle(snapshotHandle);
    return parentProcessId;
}

bool GetRunningServiceProcessId(DWORD* serviceProcessId)
{
    if (serviceProcessId == nullptr)
    {
        return false;
    }

    SC_HANDLE scmHandle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        return false;
    }

    SC_HANDLE serviceHandle = OpenServiceW(scmHandle, kServiceName, SERVICE_QUERY_STATUS);
    if (serviceHandle == nullptr)
    {
        CloseServiceHandle(scmHandle);
        return false;
    }

    SERVICE_STATUS_PROCESS status = {};
    DWORD bytesNeeded = 0;
    const BOOL ok = QueryServiceStatusEx(
        serviceHandle,
        SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&status),
        sizeof(status),
        &bytesNeeded);

    CloseServiceHandle(serviceHandle);
    CloseServiceHandle(scmHandle);

    if (!ok || status.dwCurrentState != SERVICE_RUNNING || status.dwProcessId == 0)
    {
        return false;
    }

    *serviceProcessId = status.dwProcessId;
    return true;
}

bool IsServiceParentProcess()
{
    const DWORD parentProcessId = GetParentProcessId(GetCurrentProcessId());
    if (parentProcessId == 0)
    {
        return false;
    }

    DWORD serviceProcessId = 0;
    return GetRunningServiceProcessId(&serviceProcessId) && parentProcessId == serviceProcessId;
}

void AddTrayIcon()
{
    g_trayIconData.cbSize = sizeof(g_trayIconData);
    g_trayIconData.hWnd = g_mainWindow;
    g_trayIconData.uID = 1;
    g_trayIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIconData.uCallbackMessage = kTrayIconMessage;
    g_trayIconData.hIcon = LoadIconW(g_instanceHandle, MAKEINTRESOURCEW(IDI_TRAY_ICON));
    if (g_trayIconData.hIcon == nullptr)
    {
        g_trayIconData.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    wcscpy_s(g_trayIconData.szTip, L"Tray Application");
    Shell_NotifyIconW(NIM_ADD, &g_trayIconData);
}

void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_trayIconData);
}

void ShowMainWindow()
{
    ShowWindow(g_mainWindow, SW_SHOW);
    SetForegroundWindow(g_mainWindow);
}

void StopServiceAndExit()
{
    RequestServiceStopAndWait();
    RemoveTrayIcon();
    DestroyWindow(g_mainWindow);
}

void ShowContextMenu(HWND windowHandle)
{
    HMENU menuHandle = CreatePopupMenu();
    AppendMenuW(menuHandle, MF_STRING, kTrayOpenId, L"Открыть");
    AppendMenuW(menuHandle, MF_STRING, kTrayExitId, L"Выход");

    POINT cursorPoint = {};
    GetCursorPos(&cursorPoint);

    SetForegroundWindow(windowHandle);
    TrackPopupMenu(menuHandle, TPM_RIGHTBUTTON, cursorPoint.x, cursorPoint.y, 0, windowHandle, nullptr);
    PostMessageW(windowHandle, WM_NULL, 0, 0);
    DestroyMenu(menuHandle);
}

void EnsureSingleInstancePerSession()
{
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);

    wchar_t mutexName[64] = {};
    wsprintfW(mutexName, L"Local\\TrayAppWin32_Mutex_Session_%lu", sessionId);

    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, mutexName);
    if (GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return;
    }

    HWND existingWindow = FindWindowW(kWindowClassName, nullptr);
    if (existingWindow != nullptr)
    {
        ShowWindow(existingWindow, SW_SHOW);
        SetForegroundWindow(existingWindow);
    }

    ExitProcess(0);
}

bool ShouldStartHidden(LPCWSTR commandLine)
{
    return commandLine != nullptr && wcsstr(commandLine, L"/hidden") != nullptr;
}

bool ShouldStopServiceOnly(LPCWSTR commandLine)
{
    return commandLine != nullptr && wcsstr(commandLine, L"/stopservice") != nullptr;
}

LRESULT CALLBACK WindowProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == g_taskbarRestartMessage)
    {
        AddTrayIcon();
        return 0;
    }

    switch (message)
    {
    case WM_CREATE:
    {
        HMENU mainMenu = CreateMenu();
        HMENU fileMenu = CreatePopupMenu();
        AppendMenuW(fileMenu, MF_STRING, kFileExitId, L"Выход");
        AppendMenuW(mainMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"Файл");
        SetMenu(windowHandle, mainMenu);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kTrayOpenId:
            ShowMainWindow();
            return 0;

        case kTrayExitId:
        case kFileExitId:
            StopServiceAndExit();
            return 0;

        default:
            break;
        }
        break;

    case kTrayIconMessage:
        if (lParam == WM_LBUTTONUP)
        {
            ShowMainWindow();
            return 0;
        }

        if (lParam == WM_RBUTTONUP)
        {
            ShowContextMenu(windowHandle);
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(windowHandle, SW_HIDE);
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(windowHandle, message, wParam, lParam);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instanceHandle, HINSTANCE, LPWSTR commandLine, int)
{
    g_instanceHandle = instanceHandle;

    if (ShouldStopServiceOnly(commandLine))
    {
        return RequestServiceStopAndWait() == 0 ? 0 : 1;
    }

    const int startResult = CheckAndStartService();
    if (startResult != 0)
    {
        MessageBoxW(
            nullptr,
            L"Не удалось запустить службу TrayAppService или дождаться состояния Running.",
            L"Ошибка",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!IsServiceParentProcess())
    {
        return 0;
    }

    g_startHidden = ShouldStartHidden(commandLine);

    EnsureSingleInstancePerSession();

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instanceHandle;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&windowClass))
    {
        return 1;
    }

    g_mainWindow = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        400,
        300,
        nullptr,
        nullptr,
        instanceHandle,
        nullptr);
    if (g_mainWindow == nullptr)
    {
        return 1;
    }

    g_taskbarRestartMessage = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    ShowWindow(g_mainWindow, g_startHidden ? SW_HIDE : SW_SHOW);
    UpdateWindow(g_mainWindow);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_singleInstanceMutex != nullptr)
    {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
    }

    CleanupRPCClient();
    return static_cast<int>(message.wParam);
}

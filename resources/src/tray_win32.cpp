#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <commctrl.h>

#include <iterator>
#include <string>

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
constexpr UINT kLoginButtonId = 3001;
constexpr UINT kLogoutButtonId = 3002;
constexpr UINT kActivateButtonId = 3003;
constexpr UINT kRefreshButtonId = 3004;
constexpr UINT kScanFileButtonId = 3005;
constexpr UINT kScanDirectoryButtonId = 3006;
constexpr UINT kScanDrivesButtonId = 3007;
constexpr UINT kScheduleButtonId = 3008;
constexpr UINT kMonitorButtonId = 3009;
constexpr UINT kPollTimerId = 4001;
constexpr wchar_t kWindowClassName[] = L"TrayAppWin32Class";
constexpr wchar_t kWindowTitle[] = L"ZIOVPO Security";
constexpr wchar_t kServiceName[] = L"TrayAppService";

HINSTANCE g_instanceHandle = nullptr;
HWND g_mainWindow = nullptr;
NOTIFYICONDATAW g_trayIconData = {};
HANDLE g_singleInstanceMutex = nullptr;
UINT g_taskbarRestartMessage = 0;
bool g_startHidden = false;
HWND g_statusLabel = nullptr;
HWND g_emailEdit = nullptr;
HWND g_passwordEdit = nullptr;
HWND g_loginButton = nullptr;
HWND g_logoutButton = nullptr;
HWND g_licenseLabel = nullptr;
HWND g_activationEdit = nullptr;
HWND g_activateButton = nullptr;
HWND g_refreshButton = nullptr;
HWND g_antivirusLabel = nullptr;
HWND g_databaseLabel = nullptr;
HWND g_scanPathEdit = nullptr;
HWND g_scanFileButton = nullptr;
HWND g_scanDirectoryButton = nullptr;
HWND g_scanDrivesButton = nullptr;
HWND g_scanResultLabel = nullptr;
HWND g_scheduleIntervalEdit = nullptr;
HWND g_scheduleButton = nullptr;
HWND g_monitorButton = nullptr;
bool g_authenticated = false;
bool g_hasLicense = false;
bool g_licenseBlocked = false;
HFONT g_uiFont = nullptr;

void ApplyUiFont(HWND controlHandle)
{
    if (controlHandle != nullptr && g_uiFont != nullptr)
    {
        SendMessageW(controlHandle, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    }
}

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

void SetWindowTextSafe(HWND windowHandle, const wchar_t* text)
{
    if (windowHandle != nullptr)
    {
        SetWindowTextW(windowHandle, text != nullptr ? text : L"");
    }
}

std::wstring ReadEditText(HWND editHandle)
{
    int length = GetWindowTextLengthW(editHandle);
    std::wstring value(static_cast<size_t>(length + 1), L'\0');
    if (length > 0)
    {
        GetWindowTextW(editHandle, value.data(), length + 1);
    }
    value.resize(static_cast<size_t>(length));
    return value;
}

void UpdateControlVisibility()
{
    ShowWindow(g_emailEdit, g_authenticated ? SW_HIDE : SW_SHOW);
    ShowWindow(g_passwordEdit, g_authenticated ? SW_HIDE : SW_SHOW);
    ShowWindow(g_loginButton, g_authenticated ? SW_HIDE : SW_SHOW);
    ShowWindow(g_logoutButton, g_authenticated ? SW_SHOW : SW_HIDE);
    ShowWindow(g_activationEdit, (g_authenticated && !g_hasLicense) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_activateButton, (g_authenticated && !g_hasLicense) ? SW_SHOW : SW_HIDE);
}

void RefreshUiState()
{
    wchar_t email[256] = {};
    wchar_t message[512] = {};
    int authenticated = 0;
    GetCurrentUserViaRPC(email, static_cast<int>(std::size(email)), message, static_cast<int>(std::size(message)), &authenticated);
    g_authenticated = authenticated != 0;

    if (!g_authenticated)
    {
        g_hasLicense = false;
        g_licenseBlocked = false;
        SetWindowTextSafe(g_statusLabel, L"Войдите в учетную запись");
        SetWindowTextSafe(g_licenseLabel, L"Лицензия: недоступна без входа");
        SetWindowTextSafe(g_antivirusLabel, L"Антивирус: заблокирован");
        SetWindowTextSafe(g_databaseLabel, L"Базы: недоступны без активации");
        UpdateControlVisibility();
        return;
    }

    wchar_t statusText[512] = {};
    swprintf_s(statusText, L"Пользователь: %s", email[0] != L'\0' ? email : L"(неизвестно)");
    SetWindowTextSafe(g_statusLabel, statusText);

    wchar_t expiration[64] = {};
    int hasLicense = 0;
    int blocked = 0;
    GetLicenseInfoViaRPC(&hasLicense, &blocked, expiration, static_cast<int>(std::size(expiration)),
                         message, static_cast<int>(std::size(message)));
    g_hasLicense = hasLicense != 0 && blocked == 0;
    g_licenseBlocked = blocked != 0;

    wchar_t licenseText[640] = {};
    if (g_hasLicense)
    {
        swprintf_s(licenseText, L"Лицензия активна. Действует до: %s", expiration[0] != L'\0' ? expiration : L"(дата не указана)");
        SetWindowTextSafe(g_antivirusLabel, L"Антивирус: разблокирован");
        int loaded = 0;
        int recordCount = 0;
        wchar_t releaseDate[64] = {};
        wchar_t avMessage[512] = {};
        GetAvDatabaseInfoViaRPC(&loaded, &recordCount, releaseDate, static_cast<int>(std::size(releaseDate)),
                                avMessage, static_cast<int>(std::size(avMessage)));
        wchar_t databaseText[512] = {};
        swprintf_s(databaseText, L"Базы: %s, записей: %d, дата: %s",
                   loaded ? L"загружены" : L"не загружены",
                   recordCount,
                   releaseDate[0] != L'\0' ? releaseDate : L"(нет)");
        SetWindowTextSafe(g_databaseLabel, databaseText);
    }
    else if (g_licenseBlocked)
    {
        swprintf_s(licenseText, L"Лицензия заблокирована");
        SetWindowTextSafe(g_antivirusLabel, L"Антивирус: заблокирован");
        SetWindowTextSafe(g_databaseLabel, L"Базы: недоступны");
    }
    else
    {
        swprintf_s(licenseText, L"Лицензия отсутствует: %s", message[0] != L'\0' ? message : L"требуется активация");
        SetWindowTextSafe(g_antivirusLabel, L"Антивирус: заблокирован");
        SetWindowTextSafe(g_databaseLabel, L"Базы: не загружены");
    }
    SetWindowTextSafe(g_licenseLabel, licenseText);
    UpdateControlVisibility();
}

void CreateMainControls(HWND windowHandle)
{
    auto controlId = [](UINT id) -> HMENU {
        return reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id));
    };

    if (g_uiFont == nullptr)
    {
        g_uiFont = CreateFontW(
            -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }

    g_statusLabel = CreateWindowW(L"STATIC", L"Проверка состояния...", WS_CHILD | WS_VISIBLE,
                                  24, 24, 690, 24, windowHandle, nullptr, g_instanceHandle, nullptr);
    g_emailEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                  24, 64, 260, 26, windowHandle, nullptr, g_instanceHandle, nullptr);
    g_passwordEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                     24, 100, 260, 26, windowHandle, nullptr, g_instanceHandle, nullptr);
    g_loginButton = CreateWindowW(L"BUTTON", L"Войти", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  300, 64, 140, 62, windowHandle, controlId(kLoginButtonId), g_instanceHandle, nullptr);
    g_logoutButton = CreateWindowW(L"BUTTON", L"Выйти", WS_CHILD | BS_PUSHBUTTON,
                                   410, 64, 140, 30, windowHandle, controlId(kLogoutButtonId), g_instanceHandle, nullptr);
    g_licenseLabel = CreateWindowW(L"STATIC", L"Лицензия: проверка...", WS_CHILD | WS_VISIBLE,
                                   24, 150, 690, 24, windowHandle, nullptr, g_instanceHandle, nullptr);
    g_activationEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                                       24, 190, 260, 26, windowHandle, nullptr, g_instanceHandle, nullptr);
    g_activateButton = CreateWindowW(L"BUTTON", L"Активировать", WS_CHILD | BS_PUSHBUTTON,
                                     300, 190, 160, 30, windowHandle, controlId(kActivateButtonId), g_instanceHandle, nullptr);
    g_refreshButton = CreateWindowW(L"BUTTON", L"Обновить", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    570, 64, 140, 30, windowHandle, controlId(kRefreshButtonId), g_instanceHandle, nullptr);
    g_antivirusLabel = CreateWindowW(L"STATIC", L"Антивирус: заблокирован", WS_CHILD | WS_VISIBLE,
                                     24, 240, 690, 24, windowHandle, nullptr, g_instanceHandle, nullptr);
    g_databaseLabel = CreateWindowW(L"STATIC", L"Базы: проверка...", WS_CHILD | WS_VISIBLE,
                                    24, 268, 690, 24, windowHandle, nullptr, g_instanceHandle, nullptr);
    g_scanPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                     24, 306, 500, 28, windowHandle, nullptr, g_instanceHandle, nullptr);
    g_scanFileButton = CreateWindowW(L"BUTTON", L"Сканировать файл", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     540, 306, 174, 28, windowHandle, controlId(kScanFileButtonId), g_instanceHandle, nullptr);
    g_scanDirectoryButton = CreateWindowW(L"BUTTON", L"Сканировать папку", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          24, 346, 170, 30, windowHandle, controlId(kScanDirectoryButtonId), g_instanceHandle, nullptr);
    g_scanDrivesButton = CreateWindowW(L"BUTTON", L"Все диски", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       206, 346, 120, 30, windowHandle, controlId(kScanDrivesButtonId), g_instanceHandle, nullptr);
    g_scheduleIntervalEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"60", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                             338, 346, 60, 30, windowHandle, nullptr, g_instanceHandle, nullptr);
    g_scheduleButton = CreateWindowW(L"BUTTON", L"Расписание", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     410, 346, 130, 30, windowHandle, controlId(kScheduleButtonId), g_instanceHandle, nullptr);
    g_monitorButton = CreateWindowW(L"BUTTON", L"Мониторинг", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    552, 346, 162, 30, windowHandle, controlId(kMonitorButtonId), g_instanceHandle, nullptr);
    g_scanResultLabel = CreateWindowW(L"STATIC", L"Сканирование: готово", WS_CHILD | WS_VISIBLE,
                                      24, 390, 690, 52, windowHandle, nullptr, g_instanceHandle, nullptr);

    ApplyUiFont(g_statusLabel);
    ApplyUiFont(g_emailEdit);
    ApplyUiFont(g_passwordEdit);
    ApplyUiFont(g_loginButton);
    ApplyUiFont(g_logoutButton);
    ApplyUiFont(g_licenseLabel);
    ApplyUiFont(g_activationEdit);
    ApplyUiFont(g_activateButton);
    ApplyUiFont(g_refreshButton);
    ApplyUiFont(g_antivirusLabel);
    ApplyUiFont(g_databaseLabel);
    ApplyUiFont(g_scanPathEdit);
    ApplyUiFont(g_scanFileButton);
    ApplyUiFont(g_scanDirectoryButton);
    ApplyUiFont(g_scanDrivesButton);
    ApplyUiFont(g_scheduleIntervalEdit);
    ApplyUiFont(g_scheduleButton);
    ApplyUiFont(g_monitorButton);
    ApplyUiFont(g_scanResultLabel);

    SendMessageW(g_emailEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Email"));
    SendMessageW(g_passwordEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Пароль"));
    SendMessageW(g_activationEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Код активации"));
    SendMessageW(g_scanPathEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Путь к файлу или папке"));
    SendMessageW(g_scheduleIntervalEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"мин"));
}

void StopServiceAndExit()
{
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

bool ShouldStartServiceOnly(LPCWSTR commandLine)
{
    return commandLine != nullptr && wcsstr(commandLine, L"/startservice") != nullptr;
}

bool StartServiceElevatedAndWait()
{
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
    {
        return false;
    }

    SHELLEXECUTEINFOW shellExecuteInfo = {};
    shellExecuteInfo.cbSize = sizeof(shellExecuteInfo);
    shellExecuteInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    shellExecuteInfo.lpVerb = L"runas";
    shellExecuteInfo.lpFile = modulePath;
    shellExecuteInfo.lpParameters = L"/startservice";
    shellExecuteInfo.nShow = SW_HIDE;

    if (!ShellExecuteExW(&shellExecuteInfo))
    {
        return false;
    }

    DWORD exitCode = 1;
    if (shellExecuteInfo.hProcess != nullptr)
    {
        WaitForSingleObject(shellExecuteInfo.hProcess, INFINITE);
        GetExitCodeProcess(shellExecuteInfo.hProcess, &exitCode);
        CloseHandle(shellExecuteInfo.hProcess);
    }

    return exitCode == 0;
}

void ShowScanOutcome(int resultCode, int scannedFiles, int infectedFiles, const wchar_t* threatName, const wchar_t* objectPath, const wchar_t* message)
{
    wchar_t text[1024] = {};
    if (resultCode != 0 && infectedFiles == 0)
    {
        swprintf_s(text, L"Ошибка сканирования (%d): %s", resultCode, message != nullptr && message[0] != L'\0' ? message : L"нет описания");
    }
    else if (infectedFiles > 0)
    {
        swprintf_s(text, L"Обнаружено: %s\nФайл: %s",
                   threatName != nullptr && threatName[0] != L'\0' ? threatName : L"(сигнатура)",
                   objectPath != nullptr && objectPath[0] != L'\0' ? objectPath : L"(не указан)");
    }
    else
    {
        swprintf_s(text, L"Угрозы не обнаружены. Проверено файлов: %d", scannedFiles);
    }
    SetWindowTextSafe(g_scanResultLabel, text);
}

void RefreshBackgroundScanOutcome()
{
    int scanned = 0;
    int infected = 0;
    wchar_t threat[128] = {};
    wchar_t objectPath[MAX_PATH] = {};
    wchar_t message[1024] = {};
    int result = GetLastBackgroundScanResultViaRPC(&scanned, &infected, threat, static_cast<int>(std::size(threat)),
                                                   objectPath, static_cast<int>(std::size(objectPath)),
                                                   message, static_cast<int>(std::size(message)));
    if (result == 0 && (scanned > 0 || infected > 0))
    {
        ShowScanOutcome(result, scanned, infected, threat, objectPath, message);
    }
}

void RunPathScan(bool directoryMode)
{
    const std::wstring path = ReadEditText(g_scanPathEdit);
    if (path.empty())
    {
        MessageBoxW(g_mainWindow, L"Укажите путь к файлу или папке.", L"Сканирование", MB_OK | MB_ICONINFORMATION);
        return;
    }

    int scanned = 0;
    int infected = 0;
    wchar_t threat[128] = {};
    wchar_t objectPath[MAX_PATH] = {};
    wchar_t message[1024] = {};
    int result = directoryMode
        ? ScanDirectoryViaRPC(path.c_str(), &scanned, &infected, threat, static_cast<int>(std::size(threat)),
                              objectPath, static_cast<int>(std::size(objectPath)), message, static_cast<int>(std::size(message)))
        : ScanFileViaRPC(path.c_str(), &scanned, &infected, threat, static_cast<int>(std::size(threat)),
                         objectPath, static_cast<int>(std::size(objectPath)), message, static_cast<int>(std::size(message)));
    ShowScanOutcome(result, scanned, infected, threat, objectPath, message);
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
        CreateMainControls(windowHandle);
        SetTimer(windowHandle, kPollTimerId, 30000, nullptr);
        RefreshUiState();
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

        case kLoginButtonId:
        {
            const std::wstring email = ReadEditText(g_emailEdit);
            const std::wstring password = ReadEditText(g_passwordEdit);
            wchar_t message[512] = {};
            int authenticated = 0;
            int result = LoginViaRPC(email.c_str(), password.c_str(), message, static_cast<int>(std::size(message)), &authenticated);
            if (result != 0 || !authenticated)
            {
                MessageBoxW(windowHandle, message[0] != L'\0' ? message : L"Не удалось войти", L"Ошибка входа", MB_OK | MB_ICONERROR);
            }
            SetWindowTextW(g_passwordEdit, L"");
            RefreshUiState();
            return 0;
        }

        case kLogoutButtonId:
            LogoutViaRPC();
            RefreshUiState();
            return 0;

        case kActivateButtonId:
        {
            const std::wstring key = ReadEditText(g_activationEdit);
            wchar_t expiration[64] = {};
            wchar_t message[512] = {};
            int hasLicense = 0;
            int blocked = 0;
            int result = ActivateProductViaRPC(key.c_str(), &hasLicense, &blocked, expiration, static_cast<int>(std::size(expiration)),
                                               message, static_cast<int>(std::size(message)));
            if (result != 0 || !hasLicense || blocked)
            {
                MessageBoxW(windowHandle, message[0] != L'\0' ? message : L"Не удалось активировать продукт", L"Ошибка активации", MB_OK | MB_ICONERROR);
            }
            SetWindowTextW(g_activationEdit, L"");
            RefreshUiState();
            return 0;
        }

        case kRefreshButtonId:
        {
            wchar_t updateMessage[512] = {};
            UpdateAvDatabaseViaRPC(updateMessage, static_cast<int>(std::size(updateMessage)));
            if (updateMessage[0] != L'\0')
            {
                SetWindowTextSafe(g_scanResultLabel, updateMessage);
            }
            RefreshUiState();
            RefreshBackgroundScanOutcome();
            return 0;
        }

        case kScanFileButtonId:
            RunPathScan(false);
            return 0;

        case kScanDirectoryButtonId:
            RunPathScan(true);
            return 0;

        case kScanDrivesButtonId:
        {
            int scanned = 0;
            int infected = 0;
            wchar_t threat[128] = {};
            wchar_t objectPath[MAX_PATH] = {};
            wchar_t message[1024] = {};
            int result = ScanFixedDrivesViaRPC(&scanned, &infected, threat, static_cast<int>(std::size(threat)),
                                               objectPath, static_cast<int>(std::size(objectPath)),
                                               message, static_cast<int>(std::size(message)));
            ShowScanOutcome(result, scanned, infected, threat, objectPath, message);
            return 0;
        }

        case kScheduleButtonId:
        {
            const std::wstring path = ReadEditText(g_scanPathEdit);
            const std::wstring intervalText = ReadEditText(g_scheduleIntervalEdit);
            const int interval = intervalText.empty() ? 60 : _wtoi(intervalText.c_str());
            wchar_t message[512] = {};
            ConfigureScheduledScanViaRPC(1, interval > 0 ? interval : 60, path.c_str(), message, static_cast<int>(std::size(message)));
            SetWindowTextSafe(g_scanResultLabel, message);
            return 0;
        }

        case kMonitorButtonId:
        {
            const std::wstring path = ReadEditText(g_scanPathEdit);
            wchar_t message[512] = {};
            ConfigureDirectoryMonitoringViaRPC(1, path.c_str(), message, static_cast<int>(std::size(message)));
            SetWindowTextSafe(g_scanResultLabel, message);
            return 0;
        }

        default:
            break;
        }
        break;

    case WM_TIMER:
        if (wParam == kPollTimerId)
        {
            RefreshUiState();
            RefreshBackgroundScanOutcome();
            return 0;
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
        KillTimer(windowHandle, kPollTimerId);
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

    if (ShouldStartServiceOnly(commandLine))
    {
        return CheckAndStartService() == 0 ? 0 : 1;
    }

    const int startResult = CheckAndStartService();
    if (startResult != 0)
    {
        if (!(startResult == -6 && StartServiceElevatedAndWait()))
        {
            MessageBoxW(
                nullptr,
                L"Не удалось запустить службу TrayAppService или дождаться состояния Running.",
                L"Ошибка",
                MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    g_startHidden = ShouldStartHidden(commandLine);

    EnsureSingleInstancePerSession();

    INITCOMMONCONTROLSEX commonControls = {};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);

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
        760,
        520,
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
    if (g_uiFont != nullptr)
    {
        DeleteObject(g_uiFont);
        g_uiFont = nullptr;
    }

    CleanupRPCClient();
    return static_cast<int>(message.wParam);
}

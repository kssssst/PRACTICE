#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <map>
#include <string>
#include <memory>
#include <rpc.h>
#include <cstdlib>
#include "TrayService.h"

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "rpcrt4.lib")

#define SERVICE_NAME L"TrayAppService"
#define APP_PATH L"TrayApp.exe"
#define ALPC_ENDPOINT L"TrayServiceEndpoint"

// Global variables
SERVICE_STATUS g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_hServiceStopEvent = NULL;
std::map<DWORD, HANDLE> g_ProcessMap; // SessionID -> Process Handle
CRITICAL_SECTION g_ProcessMapLock;

// Forward declarations
VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv);
VOID WINAPI ServiceCtrlHandler(DWORD dwCtrl);
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);
DWORD WINAPI WTSNotificationThread(LPVOID lpParam);
void LaunchAppInSession(DWORD dwSessionId);
void LaunchAppInAllSessions();
void TerminateAllApps();

// RPC stub functions - must use extern "C" for MIDL compatibility
extern "C" {
    void StopService(void)
    {
        if (g_hServiceStopEvent) {
            SetEvent(g_hServiceStopEvent);
        }
    }

    void GetServiceStatus(long *status)
    {
        if (status) {
            *status = (long)g_ServiceStatus.dwCurrentState;
        }
    }

    // MIDL memory management
    void __RPC_FAR * __RPC_USER MIDL_user_allocate(size_t cBytes)
    {
        return malloc(cBytes);
    }

    void __RPC_USER MIDL_user_free(void __RPC_FAR * p)
    {
        free(p);
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

// Launch application in specific session
void LaunchAppInSession(DWORD dwSessionId)
{
    if (dwSessionId == 0) return; // Skip session 0

    HANDLE hToken = NULL;
    HANDLE hUserToken = NULL;

    if (!WTSQueryUserToken(dwSessionId, &hToken)) {
        return;
    }

    HANDLE hDuplicate = NULL;
    if (!DuplicateTokenEx(hToken, TOKEN_ALL_ACCESS, NULL, SecurityIdentification, TokenPrimary, &hDuplicate)) {
        CloseHandle(hToken);
        return;
    }
    CloseHandle(hToken);

    STARTUPINFOW si = { 0 };
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = { 0 };

    wchar_t szAppPath[MAX_PATH];
    GetModuleFileNameW(NULL, szAppPath, MAX_PATH);
    wchar_t *p = wcsrchr(szAppPath, L'\\');
    if (p) {
        wcscpy_s(p + 1, MAX_PATH - (p - szAppPath + 1), APP_PATH);
    }

    if (CreateProcessAsUserW(hDuplicate, szAppPath, NULL, NULL, NULL, FALSE,
                            0, NULL, NULL, &si, &pi)) {
        EnterCriticalSection(&g_ProcessMapLock);
        g_ProcessMap[dwSessionId] = pi.hProcess;
        LeaveCriticalSection(&g_ProcessMapLock);
        CloseHandle(pi.hThread);
    } else {
        CloseHandle(pi.hProcess);
    }

    CloseHandle(hDuplicate);
}

// Launch app in all active sessions
void LaunchAppInAllSessions()
{
    PWTS_SESSION_INFOW pSessionInfo = NULL;
    DWORD dwSessionCount = 0;

    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionInfo, &dwSessionCount)) {
        for (DWORD i = 0; i < dwSessionCount; i++) {
            if (pSessionInfo[i].SessionId != 0 && pSessionInfo[i].State == WTSActive) {
                LaunchAppInSession(pSessionInfo[i].SessionId);
            }
        }
        WTSFreeMemory(pSessionInfo);
    }
}

// Terminate all launched applications
void TerminateAllApps()
{
    EnterCriticalSection(&g_ProcessMapLock);
    
    for (auto &pair : g_ProcessMap) {
        if (pair.second) {
            TerminateProcess(pair.second, 0);
            CloseHandle(pair.second);
        }
    }
    g_ProcessMap.clear();
    
    LeaveCriticalSection(&g_ProcessMapLock);
}

// WTS notification thread - monitor for new user sessions
DWORD WINAPI WTSNotificationThread(LPVOID lpParam)
{
    HWND hMsgWindow = FindWindowW(L"STATIC", NULL);
    if (!hMsgWindow) {
        hMsgWindow = HWND_MESSAGE;
    }

    WTSRegisterSessionNotification(hMsgWindow, NOTIFY_FOR_ALL_SESSIONS);

    while (WaitForSingleObject(g_hServiceStopEvent, 1000) == WAIT_TIMEOUT) {
        PWTS_SESSION_INFOW pSessionInfo = NULL;
        DWORD dwSessionCount = 0;

        if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionInfo, &dwSessionCount)) {
            for (DWORD i = 0; i < dwSessionCount; i++) {
                if (pSessionInfo[i].SessionId != 0 && pSessionInfo[i].State == WTSActive) {
                    EnterCriticalSection(&g_ProcessMapLock);
                    if (g_ProcessMap.find(pSessionInfo[i].SessionId) == g_ProcessMap.end()) {
                        LeaveCriticalSection(&g_ProcessMapLock);
                        LaunchAppInSession(pSessionInfo[i].SessionId);
                    } else {
                        LeaveCriticalSection(&g_ProcessMapLock);
                    }
                }
            }
            WTSFreeMemory(pSessionInfo);
        }
    }

    if (hMsgWindow != HWND_MESSAGE) {
        WTSUnRegisterSessionNotification(hMsgWindow);
    }
    return 0;
}

// Service control handler
VOID WINAPI ServiceCtrlHandler(DWORD dwCtrl)
{
    switch (dwCtrl) {
        case SERVICE_CONTROL_SHUTDOWN:
        case SERVICE_CONTROL_STOP:
            TerminateAllApps();
            SetEvent(g_hServiceStopEvent);
            break;
        default:
            break;
    }
}

// Service worker thread
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam)
{
    // Start WTS notification
    HANDLE hWTSThread = CreateThread(NULL, 0, WTSNotificationThread, NULL, 0, NULL);

    // Launch app in all current sessions
    LaunchAppInAllSessions();

    // Start RPC server
    RPC_STATUS status = RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc", 10,
                                                (RPC_WSTR)ALPC_ENDPOINT, NULL);

    if (status == RPC_S_OK) {
        status = RpcServerRegisterIf(ITrayService_v1_0_s_ifspec, NULL, NULL);
    }

    if (status == RPC_S_OK) {
        status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, FALSE);
    }

    if (status == RPC_S_OK) {
        WaitForSingleObject(g_hServiceStopEvent, INFINITE);
        RpcMgmtStopServerListening(NULL);
        RpcServerUnregisterIf(ITrayService_v1_0_s_ifspec, NULL, FALSE);
    }

    if (hWTSThread) {
        SetEvent(g_hServiceStopEvent);
        WaitForSingleObject(hWTSThread, 5000);
        CloseHandle(hWTSThread);
    }

    TerminateAllApps();

    return 0;
}

// Service main
VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv)
{
    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    g_hServiceStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    g_ServiceStatus.dwControlsAccepted = 0; // Disable Stop/Shutdown
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwCheckPoint = 0;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    HANDLE hWorkerThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    if (hWorkerThread) {
        WaitForSingleObject(hWorkerThread, INFINITE);
        CloseHandle(hWorkerThread);
    }

    if (g_hServiceStopEvent) {
        CloseHandle(g_hServiceStopEvent);
        g_hServiceStopEvent = NULL;
    }

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

// Entry point
int wmain(int argc, wchar_t *argv[])
{
    InitializeCriticalSection(&g_ProcessMapLock);

    SERVICE_TABLE_ENTRYW ServiceTable[] = {
        { (wchar_t *)SERVICE_NAME, ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(ServiceTable)) {
        DWORD dwErr = GetLastError();
        if (dwErr == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // Running as console app for testing
            g_hServiceStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            g_StatusHandle = 0;

            g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
            g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
            g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
            g_ServiceStatus.dwWin32ExitCode = 0;
            g_ServiceStatus.dwServiceSpecificExitCode = 0;
            g_ServiceStatus.dwCheckPoint = 0;

            ServiceWorkerThread(NULL);

            if (g_hServiceStopEvent) {
                CloseHandle(g_hServiceStopEvent);
            }
        }
    }

    DeleteCriticalSection(&g_ProcessMapLock);
    return 0;
}

// Legacy entry point for compatibility
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                    LPWSTR lpCmdLine, int nCmdShow)
{
    return wmain(__argc, __wargv);
}

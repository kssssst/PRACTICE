#define _WIN32_WINNT 0x0601
#define WINVER 0x0601

#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <map>
#include <string>
#include <memory>
#include <rpc.h>
#include <rpcndr.h>
#include <cstdlib>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "rpcrt4.lib")

#define SERVICE_NAME L"TrayAppService"
#define APP_PATH L"TrayApp.exe"
#define ALPC_ENDPOINT L"TrayServiceEndpoint"

// UUID интерфейса (должен совпадать с IDL: 12345678-1234-1234-1234-123456789abc)
const IID ITrayService_IID = {0x12345678,0x1234,0x1234,{0x12,0x34,0x56,0x78,0x9a,0xbc,0x00,0x00}};

// Прототипы RPC-функций (реализация ниже)
extern "C" error_status_t StopService(handle_t hBinding);
extern "C" error_status_t GetServiceStatus(handle_t hBinding, long *status);

// MIDL memory management
void __RPC_FAR * __RPC_USER MIDL_user_allocate(size_t cBytes) { return malloc(cBytes); }
void __RPC_USER MIDL_user_free(void __RPC_FAR * p) { free(p); }

// Глобальные переменные службы
SERVICE_STATUS g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_hServiceStopEvent = NULL;
std::map<DWORD, HANDLE> g_ProcessMap;
CRITICAL_SECTION g_ProcessMapLock;

void LaunchAppInSession(DWORD dwSessionId);
void LaunchAppInAllSessions();
void TerminateAllApps();
DWORD WINAPI WTSNotificationThread(LPVOID lpParam);
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);
VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv);
VOID WINAPI ServiceCtrlHandler(DWORD dwCtrl);

// Реализация RPC-функций
extern "C" error_status_t StopService(handle_t hBinding) {
    if (g_hServiceStopEvent) SetEvent(g_hServiceStopEvent);
    return RPC_S_OK;
}

extern "C" error_status_t GetServiceStatus(handle_t hBinding, long *status) {
    if (status) *status = (long)g_ServiceStatus.dwCurrentState;
    return RPC_S_OK;
}

void LaunchAppInSession(DWORD dwSessionId) {
    if (dwSessionId == 0) return;
    HANDLE hToken = NULL;
    if (!WTSQueryUserToken(dwSessionId, &hToken)) return;
    HANDLE hDuplicate = NULL;
    if (!DuplicateTokenEx(hToken, TOKEN_ALL_ACCESS, NULL, SecurityIdentification, TokenPrimary, &hDuplicate)) {
        CloseHandle(hToken);
        return;
    }
    CloseHandle(hToken);
    STARTUPINFOW si = { sizeof(STARTUPINFOW), 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    wchar_t szAppPath[MAX_PATH];
    GetModuleFileNameW(NULL, szAppPath, MAX_PATH);
    wchar_t *p = wcsrchr(szAppPath, L'\\');
    if (p) wcscpy_s(p + 1, MAX_PATH - (p - szAppPath + 1), APP_PATH);
    if (CreateProcessAsUserW(hDuplicate, szAppPath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        EnterCriticalSection(&g_ProcessMapLock);
        if (g_ProcessMap.count(dwSessionId)) {
            TerminateProcess(g_ProcessMap[dwSessionId], 0);
            CloseHandle(g_ProcessMap[dwSessionId]);
        }
        g_ProcessMap[dwSessionId] = pi.hProcess;
        LeaveCriticalSection(&g_ProcessMapLock);
        CloseHandle(pi.hThread);
    } else {
        CloseHandle(pi.hProcess);
    }
    CloseHandle(hDuplicate);
}

void LaunchAppInAllSessions() {
    PWTS_SESSION_INFOW pSessionInfo = NULL;
    DWORD dwSessionCount = 0;
    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionInfo, &dwSessionCount)) {
        for (DWORD i = 0; i < dwSessionCount; ++i)
            if (pSessionInfo[i].SessionId != 0 && pSessionInfo[i].State == WTSActive)
                LaunchAppInSession(pSessionInfo[i].SessionId);
        WTSFreeMemory(pSessionInfo);
    }
}

void TerminateAllApps() {
    EnterCriticalSection(&g_ProcessMapLock);
    for (auto &pair : g_ProcessMap) {
        if (pair.second) {
            TerminateProcess(pair.second, 0);
            WaitForSingleObject(pair.second, 1000);
            CloseHandle(pair.second);
        }
    }
    g_ProcessMap.clear();
    LeaveCriticalSection(&g_ProcessMapLock);
}

DWORD WINAPI WTSNotificationThread(LPVOID) {
    while (WaitForSingleObject(g_hServiceStopEvent, 500) == WAIT_TIMEOUT) {
        PWTS_SESSION_INFOW pSessionInfo = NULL;
        DWORD dwSessionCount = 0;
        if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionInfo, &dwSessionCount)) {
            for (DWORD i = 0; i < dwSessionCount; ++i) {
                if (pSessionInfo[i].SessionId != 0 && pSessionInfo[i].State == WTSActive) {
                    EnterCriticalSection(&g_ProcessMapLock);
                    bool exists = g_ProcessMap.count(pSessionInfo[i].SessionId) > 0;
                    LeaveCriticalSection(&g_ProcessMapLock);
                    if (!exists) LaunchAppInSession(pSessionInfo[i].SessionId);
                }
            }
            WTSFreeMemory(pSessionInfo);
        }
    }
    return 0;
}

VOID WINAPI ServiceCtrlHandler(DWORD dwCtrl) {
    if (dwCtrl == SERVICE_CONTROL_INTERROGATE)
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

DWORD WINAPI ServiceWorkerThread(LPVOID) {
    LaunchAppInAllSessions();
    HANDLE hWTSThread = CreateThread(NULL, 0, WTSNotificationThread, NULL, 0, NULL);
    
    RPC_STATUS status = RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc", 10, (RPC_WSTR)ALPC_ENDPOINT, NULL);
    if (status == RPC_S_OK) {
        // Регистрация интерфейса по UUID (не требует сгенерированного символа)
        status = RpcServerRegisterIfEx(
            NULL, (RPC_IF_ID*)&ITrayService_IID, NULL,
            RPC_IF_ALLOW_CALLBACKS_WITH_NO_AUTH,
            RPC_C_LISTEN_MAX_CALLS_DEFAULT, NULL);
    }
    if (status == RPC_S_OK) {
        status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, FALSE);
    }
    
    WaitForSingleObject(g_hServiceStopEvent, INFINITE);
    RpcMgmtStopServerListening(NULL);
    RpcServerUnregisterIf(NULL, NULL, FALSE);
    
    if (hWTSThread) {
        SetEvent(g_hServiceStopEvent);
        WaitForSingleObject(hWTSThread, 5000);
        CloseHandle(hWTSThread);
    }
    TerminateAllApps();
    return 0;
}

VOID WINAPI ServiceMain(DWORD, LPTSTR*) {
    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_StatusHandle) return;
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    g_hServiceStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwControlsAccepted = 0x00000080; // SERVICE_ACCEPT_INTERROGATE
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    HANDLE hWorker = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    if (hWorker) WaitForSingleObject(hWorker, INFINITE), CloseHandle(hWorker);
    if (g_hServiceStopEvent) CloseHandle(g_hServiceStopEvent);
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

int wmain(int argc, wchar_t* argv[]) {
    InitializeCriticalSection(&g_ProcessMapLock);
    SERVICE_TABLE_ENTRYW table[] = { {(wchar_t*)SERVICE_NAME, ServiceMain}, {NULL, NULL} };
    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            g_hServiceStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            ServiceWorkerThread(NULL);
            if (g_hServiceStopEvent) CloseHandle(g_hServiceStopEvent);
        }
    }
    DeleteCriticalSection(&g_ProcessMapLock);
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    return wmain(__argc, __wargv);
}
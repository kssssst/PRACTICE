#define _WIN32_WINNT 0x0601
#define WINVER 0x0601

#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <rpc.h>
#include <rpcndr.h>
#include <cstdlib>
#include <map>
#include <string>

extern "C" {
#include "TrayService.h"
}

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "rpcrt4.lib")

namespace {
    constexpr wchar_t kServiceName[] = L"TrayAppService";
    constexpr wchar_t kRpcEndpoint[] = L"TrayServiceEndpoint";
    constexpr wchar_t kAppName[] = L"TrayApp.exe";

    SERVICE_STATUS g_serviceStatus = {};
    SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
    HANDLE g_stopEvent = nullptr;
    CRITICAL_SECTION g_processesLock;
    std::map<DWORD, HANDLE> g_sessionProcesses;

    std::wstring GetTrayAppPath() {
        wchar_t modulePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        std::wstring serviceDir = modulePath;
        auto pos = serviceDir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) serviceDir.erase(pos);

        std::wstring candidates[] = {
            serviceDir + L"\\" + kAppName,
            serviceDir + L"\\..\\" + kAppName,
            serviceDir + L"\\..\\Release\\" + kAppName
        };

        for (const auto& c : candidates) {
            if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES)
                return c;
        }
        return candidates[0];
    }

    void UpdateServiceState(DWORD state, DWORD win32ExitCode = NO_ERROR) {
        g_serviceStatus.dwCurrentState = state;
        g_serviceStatus.dwWin32ExitCode = win32ExitCode;
        g_serviceStatus.dwWaitHint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : 5000;
        g_serviceStatus.dwControlsAccepted = (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_SESSIONCHANGE : 0;
        if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_serviceStatus);
    }

    void LaunchAppInSession(DWORD sessionId) {
        if (sessionId == 0) return;

        HANDLE userToken = nullptr;
        if (!WTSQueryUserToken(sessionId, &userToken)) return;

        HANDLE primaryToken = nullptr;
        if (!DuplicateTokenEx(userToken, TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                              nullptr, SecurityImpersonation, TokenPrimary, &primaryToken)) {
            CloseHandle(userToken);
            return;
        }
        CloseHandle(userToken);

        LPVOID env = nullptr;
        CreateEnvironmentBlock(&env, primaryToken, FALSE);

        STARTUPINFOW si = { sizeof(si) };
        si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        std::wstring appPath = GetTrayAppPath();
        std::wstring cmdLine = L"\"" + appPath + L"\" /hidden";

        BOOL ok = CreateProcessAsUserW(primaryToken, appPath.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                                       CREATE_UNICODE_ENVIRONMENT, env, nullptr, &si, &pi);

        if (env) DestroyEnvironmentBlock(env);
        CloseHandle(primaryToken);

        if (ok) {
            EnterCriticalSection(&g_processesLock);
            if (g_sessionProcesses.count(sessionId)) {
                TerminateProcess(g_sessionProcesses[sessionId], 0);
                CloseHandle(g_sessionProcesses[sessionId]);
            }
            g_sessionProcesses[sessionId] = pi.hProcess;
            LeaveCriticalSection(&g_processesLock);
            CloseHandle(pi.hThread);
        } else {
            if (pi.hProcess) CloseHandle(pi.hProcess);
        }
    }

    void LaunchAppInExistingSessions() {
        PWTS_SESSION_INFOW sessions = nullptr;
        DWORD count = 0;
        if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
            for (DWORD i = 0; i < count; ++i) {
                if (sessions[i].SessionId != 0)
                    LaunchAppInSession(sessions[i].SessionId);
            }
            WTSFreeMemory(sessions);
        }
    }

    void TerminateAllApps() {
        EnterCriticalSection(&g_processesLock);
        for (auto& p : g_sessionProcesses) {
            if (p.second) {
                TerminateProcess(p.second, 0);
                WaitForSingleObject(p.second, 5000);
                CloseHandle(p.second);
            }
        }
        g_sessionProcesses.clear();
        LeaveCriticalSection(&g_processesLock);
    }

    DWORD WINAPI ServiceWorkerThread(LPVOID) {
        RPC_STATUS status = RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, (RPC_WSTR)kRpcEndpoint, nullptr);
        if (status == RPC_S_OK)
            status = RpcServerRegisterIf(ITrayService_v1_0_s_ifspec, nullptr, nullptr);
        if (status == RPC_S_OK)
            status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
        if (status != RPC_S_OK) {
            UpdateServiceState(SERVICE_STOPPED, status);
            return status;
        }

        UpdateServiceState(SERVICE_RUNNING);
        LaunchAppInExistingSessions();

        WaitForSingleObject(g_stopEvent, INFINITE);

        UpdateServiceState(SERVICE_STOP_PENDING);
        RpcMgmtStopServerListening(nullptr);
        RpcServerUnregisterIf(ITrayService_v1_0_s_ifspec, nullptr, FALSE);
        TerminateAllApps();

        return 0;
    }
}

extern "C" {
    error_status_t StopService() {
        if (g_stopEvent) SetEvent(g_stopEvent);
        return RPC_S_OK;
    }

    error_status_t GetServiceStatus(long* status) {
        if (status) *status = g_serviceStatus.dwCurrentState;
        return RPC_S_OK;
    }
}

void* __RPC_USER MIDL_user_allocate(size_t s) { return malloc(s); }
void __RPC_USER MIDL_user_free(void* p) { free(p); }

DWORD WINAPI ServiceControlHandlerEx(DWORD ctrl, DWORD evtType, LPVOID evtData, LPVOID) {
    if (ctrl == SERVICE_CONTROL_SESSIONCHANGE && (evtType == WTS_SESSION_LOGON || evtType == WTS_CONSOLE_CONNECT || evtType == WTS_REMOTE_CONNECT)) {
        auto* notif = static_cast<WTSSESSION_NOTIFICATION*>(evtData);
        if (notif && notif->dwSessionId != 0)
            LaunchAppInSession(notif->dwSessionId);
    }
    return NO_ERROR;
}

VOID WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceControlHandlerEx, nullptr);
    if (!g_statusHandle) return;

    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    UpdateServiceState(SERVICE_START_PENDING);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        UpdateServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    HANDLE worker = CreateThread(nullptr, 0, ServiceWorkerThread, nullptr, 0, nullptr);
    if (worker) {
        WaitForSingleObject(worker, INFINITE);
        DWORD workerExitCode = NO_ERROR;
        GetExitCodeThread(worker, &workerExitCode);
        CloseHandle(worker);
        CloseHandle(g_stopEvent);
        UpdateServiceState(SERVICE_STOPPED, workerExitCode);
        return;
    } else {
        const DWORD error = GetLastError();
        CloseHandle(g_stopEvent);
        UpdateServiceState(SERVICE_STOPPED, error);
        return;
    }
}

int wmain() {
    InitializeCriticalSection(&g_processesLock);

    SERVICE_TABLE_ENTRYW table[] = { {const_cast<LPWSTR>(kServiceName), ServiceMain}, {nullptr, nullptr} };
    StartServiceCtrlDispatcherW(table);

    DeleteCriticalSection(&g_processesLock);
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) { return wmain(); }

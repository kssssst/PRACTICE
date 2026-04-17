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

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace
{
constexpr wchar_t kServiceName[] = L"TrayAppService";
constexpr wchar_t kRpcEndpoint[] = L"TrayServiceEndpoint";
constexpr wchar_t kAppName[] = L"TrayApp.exe";

struct SessionProcess
{
    HANDLE processHandle = nullptr;
};

SERVICE_STATUS g_serviceStatus = {};
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_workerStartedEvent = nullptr;
CRITICAL_SECTION g_processesLock;
std::map<DWORD, SessionProcess> g_sessionProcesses;
DWORD g_workerStartupResult = ERROR_GEN_FAILURE;

std::wstring GetDirectoryPath(const std::wstring& path)
{
    const std::wstring::size_type separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
    {
        return L"";
    }

    return path.substr(0, separator);
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring CombinePath(const std::wstring& left, const std::wstring& right)
{
    if (left.empty())
    {
        return right;
    }

    if (left.back() == L'\\')
    {
        return left + right;
    }

    return left + L'\\' + right;
}

std::wstring GetTrayAppPath()
{
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

    const std::wstring serviceDir = GetDirectoryPath(modulePath);
    const std::wstring candidates[] = {
        CombinePath(serviceDir, kAppName),
        CombinePath(GetDirectoryPath(serviceDir), kAppName),
        CombinePath(CombinePath(GetDirectoryPath(GetDirectoryPath(serviceDir)), L"Release"), kAppName),
    };

    for (const std::wstring& candidate : candidates)
    {
        if (FileExists(candidate))
        {
            return candidate;
        }
    }

    return candidates[0];
}

void UpdateServiceState(DWORD currentState, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0)
{
    g_serviceStatus.dwCurrentState = currentState;
    g_serviceStatus.dwWin32ExitCode = win32ExitCode;
    g_serviceStatus.dwWaitHint = waitHint;
    g_serviceStatus.dwCheckPoint =
        (currentState == SERVICE_RUNNING || currentState == SERVICE_STOPPED) ? 0 : g_serviceStatus.dwCheckPoint + 1;

    if (g_statusHandle != nullptr)
    {
        SetServiceStatus(g_statusHandle, &g_serviceStatus);
    }
}

bool RemoveExitedProcessLocked(DWORD sessionId)
{
    const auto it = g_sessionProcesses.find(sessionId);
    if (it == g_sessionProcesses.end())
    {
        return false;
    }

    DWORD exitCode = STILL_ACTIVE;
    if (GetExitCodeProcess(it->second.processHandle, &exitCode) && exitCode == STILL_ACTIVE)
    {
        return false;
    }

    CloseHandle(it->second.processHandle);
    g_sessionProcesses.erase(it);
    return true;
}

void RegisterProcessForSession(DWORD sessionId, HANDLE processHandle)
{
    EnterCriticalSection(&g_processesLock);

    RemoveExitedProcessLocked(sessionId);

    const auto existing = g_sessionProcesses.find(sessionId);
    if (existing != g_sessionProcesses.end())
    {
        TerminateProcess(existing->second.processHandle, 0);
        WaitForSingleObject(existing->second.processHandle, 5000);
        CloseHandle(existing->second.processHandle);
        g_sessionProcesses.erase(existing);
    }

    g_sessionProcesses.emplace(sessionId, SessionProcess{processHandle});
    LeaveCriticalSection(&g_processesLock);
}

bool IsSessionAlreadyTracked(DWORD sessionId)
{
    EnterCriticalSection(&g_processesLock);
    RemoveExitedProcessLocked(sessionId);
    const bool exists = g_sessionProcesses.find(sessionId) != g_sessionProcesses.end();
    LeaveCriticalSection(&g_processesLock);
    return exists;
}

void LaunchAppInSession(DWORD sessionId)
{
    if (sessionId == 0 || IsSessionAlreadyTracked(sessionId))
    {
        return;
    }

    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken))
    {
        return;
    }

    HANDLE primaryToken = nullptr;
    if (!DuplicateTokenEx(
            userToken,
            TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
            nullptr,
            SecurityImpersonation,
            TokenPrimary,
            &primaryToken))
    {
        CloseHandle(userToken);
        return;
    }
    CloseHandle(userToken);

    LPVOID environment = nullptr;
    CreateEnvironmentBlock(&environment, primaryToken, FALSE);

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION processInfo = {};
    std::wstring applicationPath = GetTrayAppPath();
    std::wstring commandLine = L"\"" + applicationPath + L"\" /hidden";

    const DWORD creationFlags = CREATE_UNICODE_ENVIRONMENT;
    const BOOL created = CreateProcessAsUserW(
        primaryToken,
        applicationPath.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        creationFlags,
        environment,
        nullptr,
        &startupInfo,
        &processInfo);

    if (environment != nullptr)
    {
        DestroyEnvironmentBlock(environment);
    }

    CloseHandle(primaryToken);

    if (!created)
    {
        return;
    }

    CloseHandle(processInfo.hThread);
    RegisterProcessForSession(sessionId, processInfo.hProcess);
}

bool ShouldLaunchForSessionState(WTS_CONNECTSTATE_CLASS state)
{
    return state == WTSActive || state == WTSConnected;
}

void LaunchAppInExistingSessions()
{
    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD sessionCount = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &sessionCount))
    {
        return;
    }

    for (DWORD index = 0; index < sessionCount; ++index)
    {
        if (sessions[index].SessionId == 0)
        {
            continue;
        }

        if (ShouldLaunchForSessionState(sessions[index].State))
        {
            LaunchAppInSession(sessions[index].SessionId);
        }
    }

    WTSFreeMemory(sessions);
}

void TerminateAllApps()
{
    EnterCriticalSection(&g_processesLock);
    for (auto& [sessionId, sessionProcess] : g_sessionProcesses)
    {
        (void)sessionId;
        if (sessionProcess.processHandle == nullptr)
        {
            continue;
        }

        TerminateProcess(sessionProcess.processHandle, 0);
        WaitForSingleObject(sessionProcess.processHandle, 5000);
        CloseHandle(sessionProcess.processHandle);
    }

    g_sessionProcesses.clear();
    LeaveCriticalSection(&g_processesLock);
}

RPC_STATUS StartRpcServer()
{
    RPC_STATUS status = RpcServerUseProtseqEpW(
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
        nullptr);
    if (status != RPC_S_OK)
    {
        return status;
    }

    status = RpcServerRegisterIf(ITrayService_v1_0_s_ifspec, nullptr, nullptr);
    if (status != RPC_S_OK)
    {
        return status;
    }

    return RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
}

void StopRpcServer()
{
    RpcMgmtStopServerListening(nullptr);
    RpcServerUnregisterIf(ITrayService_v1_0_s_ifspec, nullptr, FALSE);
}

DWORD WINAPI ServiceWorkerThread(LPVOID)
{
    const RPC_STATUS rpcStatus = StartRpcServer();
    if (rpcStatus != RPC_S_OK)
    {
        g_workerStartupResult = rpcStatus;
        if (g_workerStartedEvent != nullptr)
        {
            SetEvent(g_workerStartedEvent);
        }
        SetEvent(g_stopEvent);
        return rpcStatus;
    }

    LaunchAppInExistingSessions();
    g_workerStartupResult = ERROR_SUCCESS;
    if (g_workerStartedEvent != nullptr)
    {
        SetEvent(g_workerStartedEvent);
    }
    WaitForSingleObject(g_stopEvent, INFINITE);

    UpdateServiceState(SERVICE_STOP_PENDING, NO_ERROR, 5000);
    StopRpcServer();
    TerminateAllApps();
    return 0;
}
}  // namespace

extern "C"
{
error_status_t StopService()
{
    if (g_stopEvent != nullptr)
    {
        SetEvent(g_stopEvent);
    }

    return RPC_S_OK;
}

error_status_t GetServiceStatus(long* status)
{
    if (status != nullptr)
    {
        *status = static_cast<long>(g_serviceStatus.dwCurrentState);
    }

    return RPC_S_OK;
}
}

void* __RPC_USER MIDL_user_allocate(size_t bytes)
{
    return std::malloc(bytes);
}

void __RPC_USER MIDL_user_free(void* memory)
{
    std::free(memory);
}

DWORD WINAPI ServiceControlHandlerEx(DWORD control, DWORD eventType, LPVOID eventData, LPVOID)
{
    switch (control)
    {
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(g_statusHandle, &g_serviceStatus);
        return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
        if (eventType == WTS_SESSION_LOGON || eventType == WTS_CONSOLE_CONNECT || eventType == WTS_REMOTE_CONNECT)
        {
            const auto* notification = static_cast<WTSSESSION_NOTIFICATION*>(eventData);
            if (notification != nullptr && notification->dwSessionId != 0)
            {
                LaunchAppInSession(notification->dwSessionId);
            }
        }
        return NO_ERROR;

    default:
        return NO_ERROR;
    }
}

VOID WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceControlHandlerEx, nullptr);
    if (g_statusHandle == nullptr)
    {
        return;
    }

    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwControlsAccepted = 0;
    UpdateServiceState(SERVICE_START_PENDING, NO_ERROR, 5000);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stopEvent == nullptr)
    {
        UpdateServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    g_workerStartedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_workerStartedEvent == nullptr)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        UpdateServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    g_workerStartupResult = ERROR_GEN_FAILURE;
    HANDLE workerThread = CreateThread(nullptr, 0, ServiceWorkerThread, nullptr, 0, nullptr);
    if (workerThread == nullptr)
    {
        CloseHandle(g_workerStartedEvent);
        g_workerStartedEvent = nullptr;
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        UpdateServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    WaitForSingleObject(g_workerStartedEvent, INFINITE);
    CloseHandle(g_workerStartedEvent);
    g_workerStartedEvent = nullptr;

    if (g_workerStartupResult != ERROR_SUCCESS)
    {
        WaitForSingleObject(workerThread, INFINITE);
        CloseHandle(workerThread);
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        UpdateServiceState(SERVICE_STOPPED, g_workerStartupResult);
        return;
    }

    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_SESSIONCHANGE;
    UpdateServiceState(SERVICE_RUNNING);
    WaitForSingleObject(workerThread, INFINITE);
    CloseHandle(workerThread);

    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
    UpdateServiceState(SERVICE_STOPPED);
}

int wmain()
{
    InitializeCriticalSection(&g_processesLock);

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr},
    };

    if (!StartServiceCtrlDispatcherW(serviceTable) &&
        GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
    {
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_stopEvent != nullptr)
        {
            ServiceWorkerThread(nullptr);
            CloseHandle(g_stopEvent);
            g_stopEvent = nullptr;
        }
    }

    DeleteCriticalSection(&g_processesLock);
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    return wmain();
}

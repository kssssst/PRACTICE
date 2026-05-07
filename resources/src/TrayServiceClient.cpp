#include "TrayServiceClient.h"

#include <windows.h>
#include <winsvc.h>
#include <rpc.h>
#include <rpcndr.h>

#include <cstdlib>

extern "C" {
#include "TrayService.h"
}

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "rpcrt4.lib")

namespace
{
constexpr wchar_t kServiceName[] = L"TrayAppService";
constexpr wchar_t kRpcEndpoint[] = L"TrayServiceEndpoint";

handle_t g_bindingHandle = nullptr;

bool QueryServiceStatusValue(SC_HANDLE serviceHandle, DWORD* currentState)
{
    if (currentState == nullptr)
    {
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
    if (!ok)
    {
        return false;
    }

    *currentState = status.dwCurrentState;
    return true;
}
}  // namespace

void* __RPC_USER MIDL_user_allocate(size_t bytes)
{
    return std::malloc(bytes);
}

void __RPC_USER MIDL_user_free(void* memory)
{
    std::free(memory);
}

int InitializeRPCClient()
{
    if (g_bindingHandle != nullptr)
    {
        return 0;
    }

    RPC_WSTR stringBinding = nullptr;
    RPC_STATUS status = RpcStringBindingComposeW(
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
        nullptr,
        &stringBinding);
    if (status != RPC_S_OK)
    {
        return -1;
    }

    status = RpcBindingFromStringBindingW(stringBinding, &g_bindingHandle);
    RpcStringFreeW(&stringBinding);
    if (status != RPC_S_OK)
    {
        return -2;
    }

    TrayServiceBinding = g_bindingHandle;
    return 0;
}

int StopServiceViaRPC()
{
    if (InitializeRPCClient() != 0)
    {
        return -1;
    }

    __try
    {
        StopService();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -2;
    }

    return 0;
}

int GetServiceStatusViaRPC(long* status)
{
    if (status == nullptr)
    {
        return -1;
    }

    if (InitializeRPCClient() != 0)
    {
        return -2;
    }

    __try
    {
        GetServiceStatus(status);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -3;
    }

    return 0;
}

void CleanupRPCClient()
{
    if (g_bindingHandle != nullptr)
    {
        RpcBindingFree(&g_bindingHandle);
        g_bindingHandle = nullptr;
        TrayServiceBinding = nullptr;
    }
}

int IsServiceRunning()
{
    SC_HANDLE scmHandle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        return 0;
    }

    SC_HANDLE serviceHandle = OpenServiceW(scmHandle, kServiceName, SERVICE_QUERY_STATUS);
    if (serviceHandle == nullptr)
    {
        CloseServiceHandle(scmHandle);
        return 0;
    }

    DWORD currentState = SERVICE_STOPPED;
    const bool ok = QueryServiceStatusValue(serviceHandle, &currentState);

    CloseServiceHandle(serviceHandle);
    CloseServiceHandle(scmHandle);
    return ok && currentState == SERVICE_RUNNING;
}

int WaitForServiceState(DWORD expectedState, DWORD timeoutMs)
{
    SC_HANDLE scmHandle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        return -1;
    }

    SC_HANDLE serviceHandle = OpenServiceW(scmHandle, kServiceName, SERVICE_QUERY_STATUS);
    if (serviceHandle == nullptr)
    {
        CloseServiceHandle(scmHandle);
        return -2;
    }

    const DWORD startTick = GetTickCount();
    for (;;)
    {
        DWORD currentState = SERVICE_STOPPED;
        if (!QueryServiceStatusValue(serviceHandle, &currentState))
        {
            CloseServiceHandle(serviceHandle);
            CloseServiceHandle(scmHandle);
            return -3;
        }

        if (currentState == expectedState)
        {
            CloseServiceHandle(serviceHandle);
            CloseServiceHandle(scmHandle);
            return 0;
        }

        if (GetTickCount() - startTick >= timeoutMs)
        {
            CloseServiceHandle(serviceHandle);
            CloseServiceHandle(scmHandle);
            return -4;
        }

        Sleep(250);
    }
}

int CheckAndStartService()
{
    SC_HANDLE scmHandle = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scmHandle == nullptr)
    {
        return -1;
    }

    SC_HANDLE serviceHandle = OpenServiceW(
        scmHandle,
        kServiceName,
        SERVICE_QUERY_STATUS);
    if (serviceHandle == nullptr)
    {
        CloseServiceHandle(scmHandle);
        return -2;
    }

    DWORD currentState = SERVICE_STOPPED;
    if (!QueryServiceStatusValue(serviceHandle, &currentState))
    {
        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(scmHandle);
        return -3;
    }

    if (currentState == SERVICE_RUNNING)
    {
        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(scmHandle);
        return 0;
    }

    if (currentState == SERVICE_STOP_PENDING)
    {
        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(scmHandle);
        return -4;
    }

    if (currentState == SERVICE_START_PENDING)
    {
        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(scmHandle);
        return WaitForServiceState(SERVICE_RUNNING, 30000);
    }

    CloseServiceHandle(serviceHandle);

    serviceHandle = OpenServiceW(
        scmHandle,
        kServiceName,
        SERVICE_START | SERVICE_QUERY_STATUS);
    if (serviceHandle == nullptr)
    {
        const DWORD error = GetLastError();
        CloseServiceHandle(scmHandle);
        return error == ERROR_ACCESS_DENIED ? -6 : -2;
    }

    if (currentState != SERVICE_START_PENDING)
    {
        if (!StartServiceW(serviceHandle, 0, nullptr))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_SERVICE_ALREADY_RUNNING)
            {
                CloseServiceHandle(serviceHandle);
                CloseServiceHandle(scmHandle);
                return error == ERROR_ACCESS_DENIED ? -6 : -5;
            }
        }
    }

    CloseServiceHandle(serviceHandle);
    CloseServiceHandle(scmHandle);
    return WaitForServiceState(SERVICE_RUNNING, 30000);
}

int RequestServiceStopAndWait()
{
    const int rpcResult = StopServiceViaRPC();
    if (rpcResult != 0)
    {
        return rpcResult;
    }

    return WaitForServiceState(SERVICE_STOPPED, 30000);
}

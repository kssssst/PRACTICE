#include <windows.h>
#include <winsvc.h>
#include <stdio.h>
#include "TrayServiceClient.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "rpcrt4.lib")

#define SERVICE_NAME L"TrayAppService"
#define ALPC_ENDPOINT L"TrayServiceEndpoint"

// RPC client declarations
static RPC_BINDING_HANDLE g_hBinding = NULL;

extern "C" {
    extern void * __MIDL_user_allocate(size_t);
    extern void __MIDL_user_free(void *);
    extern error_status_t StopService(void);
    extern error_status_t GetServiceStatus(long *status);
}

// Initialize RPC client
int InitializeRPCClient()
{
    RPC_STATUS status;
    RPC_CSTR pszUuid = NULL;
    RPC_CSTR pszProtSeq = (RPC_CSTR)L"ncalrpc";
    RPC_CSTR pszNetAddr = NULL;
    RPC_CSTR pszEndpoint = (RPC_CSTR)ALPC_ENDPOINT;
    RPC_CSTR pszOptions = NULL;

    status = RpcStringBindingComposeW(pszUuid, pszProtSeq, pszNetAddr, pszEndpoint,
                                       pszOptions, (RPC_WSTR *)&g_hBinding);
    if (status) {
        return -1;
    }

    return 0;
}

// Stop service via RPC
int StopServiceViaRPC()
{
    if (!g_hBinding) {
        if (InitializeRPCClient() != 0) {
            return -1;
        }
    }

    RPC_STATUS status = RPC_S_OK;
    
    __try {
        StopService();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = RpcExceptionCode();
    }

    return status == RPC_S_OK ? 0 : -1;
}

// Cleanup RPC client
void CleanupRPCClient()
{
    if (g_hBinding) {
        RpcBindingFree(&g_hBinding);
        g_hBinding = NULL;
    }
}

// Check if service is running
int IsServiceRunning()
{
    SC_HANDLE hSCManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCManager) {
        return 0;
    }

    SC_HANDLE hService = OpenServiceW(hSCManager, SERVICE_NAME,
                                       SERVICE_QUERY_STATUS);
    if (!hService) {
        CloseServiceHandle(hSCManager);
        return 0;
    }

    SERVICE_STATUS status = { 0 };
    BOOL bResult = QueryServiceStatus(hService, &status);

    int isRunning = bResult && status.dwCurrentState == SERVICE_RUNNING ? 1 : 0;

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);

    return isRunning;
}

// Check and start service if needed
int CheckAndStartService()
{
    if (IsServiceRunning()) {
        return 0; // Service already running
    }

    SC_HANDLE hSCManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCManager) {
        return -1;
    }

    SC_HANDLE hService = OpenServiceW(hSCManager, SERVICE_NAME,
                                       SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hService) {
        CloseServiceHandle(hSCManager);
        return -2;
    }

    if (!StartServiceW(hService, 0, NULL)) {
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCManager);
        return -3;
    }

    // Wait for service to start
    for (int i = 0; i < 30; i++) {
        Sleep(1000);

        SERVICE_STATUS status = { 0 };
        if (QueryServiceStatus(hService, &status)) {
            if (status.dwCurrentState == SERVICE_RUNNING) {
                CloseServiceHandle(hService);
                CloseServiceHandle(hSCManager);
                return 0;
            }
        }
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);
    return -4; // Timeout
}

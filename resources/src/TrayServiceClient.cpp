#include <windows.h>
#include <winsvc.h>
#include <rpc.h>
#include <rpcndr.h>
#include <stdio.h>
#include <cstdlib>
#include <tlhelp32.h>
#include <psapi.h>
#include "TrayServiceClient.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "psapi.lib")

// Forward declarations from IDL
extern "C" {
error_status_t StopService(void);
error_status_t GetServiceStatus(long *status);
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

#define SERVICE_NAME L"TrayAppService"
#define ALPC_ENDPOINT L"TrayServiceEndpoint"

// Global RPC binding handle
handle_t hBinding = NULL;

// Initialize RPC client
int InitializeRPCClient()
{
    RPC_STATUS status;
    RPC_WSTR pszStringBinding = NULL;

    status = RpcStringBindingComposeW(
        NULL,
        (RPC_WSTR)L"ncalrpc",
        NULL,
        (RPC_WSTR)ALPC_ENDPOINT,
        NULL,
        &pszStringBinding);
    
    if (status) {
        return -1;
    }

    status = RpcBindingFromStringBindingW(pszStringBinding, &hBinding);
    RpcStringFreeW(&pszStringBinding);

    if (status) {
        return -2;
    }

    return 0;
}

// Stop service via RPC
int StopServiceViaRPC()
{
    if (!hBinding) {
        if (InitializeRPCClient() != 0) {
            return -1;
        }
    }

    __try {
        StopService();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }

    return 0;
}

// Get service status via RPC
int GetServiceStatusViaRPC(long *pStatus)
{
    if (!hBinding) {
        if (InitializeRPCClient() != 0) {
            return -1;
        }
    }

    if (!pStatus) {
        return -1;
    }

    __try {
        GetServiceStatus(pStatus);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }

    return 0;
}

// Clean up RPC client
void CleanupRPCClient()
{
    if (hBinding) {
        RpcBindingFree(&hBinding);
        hBinding = NULL;
    }
}

// Check if service is running
int IsServiceRunning()
{
    SC_HANDLE hSCManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCManager) {
        return 0;
    }

    SC_HANDLE hService = OpenServiceW(hSCManager, SERVICE_NAME, SERVICE_QUERY_STATUS);
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

// Check and start service if necessary
int CheckAndStartService()
{
    if (IsServiceRunning()) {
        return 0;
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
        DWORD dwErr = GetLastError();
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCManager);
        return -3;
    }

    // Wait for service to start
    for (int i = 0; i < 30; i++) {
        Sleep(1000);

        SERVICE_STATUS svc_status = { 0 };
        if (QueryServiceStatus(hService, &svc_status)) {
            if (svc_status.dwCurrentState == SERVICE_RUNNING) {
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

// Get parent process ID
DWORD GetParentProcessIdW(DWORD dwProcessId)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe32 = { 0 };
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(hSnapshot, &pe32)) {
        CloseHandle(hSnapshot);
        return 0;
    }

    do {
        if (pe32.th32ProcessID == dwProcessId) {
            DWORD dwParentId = pe32.th32ParentProcessID;
            CloseHandle(hSnapshot);
            return dwParentId;
        }
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);
    return 0;
}

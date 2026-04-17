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

handle_t hBinding = NULL;

extern "C" {
error_status_t StopService(handle_t hBinding);
error_status_t GetServiceStatus(handle_t hBinding, long *status);
}

void __RPC_FAR * __RPC_USER MIDL_user_allocate(size_t cBytes) { return malloc(cBytes); }
void __RPC_USER MIDL_user_free(void __RPC_FAR * p) { free(p); }

#define SERVICE_NAME L"TrayAppService"
#define ALPC_ENDPOINT L"TrayServiceEndpoint"

int InitializeRPCClient() {
    RPC_WSTR pszStringBinding = NULL;
    RPC_STATUS status = RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncalrpc", NULL, (RPC_WSTR)ALPC_ENDPOINT, NULL, &pszStringBinding);
    if (status) return -1;
    status = RpcBindingFromStringBindingW(pszStringBinding, &hBinding);
    RpcStringFreeW(&pszStringBinding);
    return status ? -2 : 0;
}

int StopServiceViaRPC() {
    if (!hBinding && InitializeRPCClient() != 0) return -1;
    __try { StopService(hBinding); } __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return 0;
}

int GetServiceStatusViaRPC(long *pStatus) {
    if (!pStatus) return -1;
    if (!hBinding && InitializeRPCClient() != 0) return -1;
    __try { GetServiceStatus(hBinding, pStatus); } __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return 0;
}

void CleanupRPCClient() {
    if (hBinding) RpcBindingFree(&hBinding), hBinding = NULL;
}

int IsServiceRunning() {
    SC_HANDLE hSC = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSC) return 0;
    SC_HANDLE hSvc = OpenServiceW(hSC, SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (!hSvc) { CloseServiceHandle(hSC); return 0; }
    SERVICE_STATUS ss = {0};
    BOOL ok = QueryServiceStatus(hSvc, &ss);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSC);
    return ok && ss.dwCurrentState == SERVICE_RUNNING;
}

int CheckAndStartService() {
    if (IsServiceRunning()) return 0;
    SC_HANDLE hSC = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSC) return -1;
    SC_HANDLE hSvc = OpenServiceW(hSC, SERVICE_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hSvc) { CloseServiceHandle(hSC); return -2; }
    if (!StartServiceW(hSvc, 0, NULL)) {
        CloseServiceHandle(hSvc);
        CloseServiceHandle(hSC);
        return -3;
    }
    for (int i = 0; i < 30; ++i) {
        Sleep(1000);
        SERVICE_STATUS ss = {0};
        if (QueryServiceStatus(hSvc, &ss) && ss.dwCurrentState == SERVICE_RUNNING) {
            CloseServiceHandle(hSvc);
            CloseServiceHandle(hSC);
            return 0;
        }
    }
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSC);
    return -4;
}

DWORD GetParentProcessIdW(DWORD dwProcessId) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
    if (Process32FirstW(hSnap, &pe)) {
        do if (pe.th32ProcessID == dwProcessId) {
            DWORD pid = pe.th32ParentProcessID;
            CloseHandle(hSnap);
            return pid;
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return 0;
}
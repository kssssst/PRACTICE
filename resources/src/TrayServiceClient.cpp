#include <windows.h>
#include <winsvc.h>
#include <rpc.h>
#include <rpcndr.h>
#include <stdio.h>
#include <cstdlib>
#include "TrayServiceClient.h"
#include "TrayService.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "rpcrt4.lib")

// Управление памятью MIDL для клиентского stub'а
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

// Глобальный RPC дескриптор привязки - требуется для implicit_handle
handle_t hBinding = NULL;

// Инициализация RPC клиента
int InitializeRPCClient()
{
    RPC_STATUS status;
    RPC_WSTR pszStringBinding = NULL;

    status = RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncalrpc", NULL,
                                       (RPC_WSTR)ALPC_ENDPOINT, NULL, &pszStringBinding);
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

// Остановка сервиса через RPC
int StopServiceViaRPC()
{
    if (!hBinding) {
        if (InitializeRPCClient() != 0) {
            return -1;
        }
    }

    __try {
        StopService(hBinding);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }

    return 0;
}

// Очистка RPC клиента
void CleanupRPCClient()
{
    if (hBinding) {
        RpcBindingFree(&hBinding);
        hBinding = NULL;
    }
}

// Получение статуса сервиса через RPC
int GetServiceStatusViaRPC(long *pStatus)
{
    if (!hBinding) {
        if (InitializeRPCClient() != 0) {
            return -1;
        }
    }

    __try {
        GetServiceStatus(hBinding, pStatus);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }

    return 0;
}

// Проверка, запущен ли сервис
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

// Проверка и запуск сервиса, если необходимо
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

    // Ожидание запуска сервиса
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

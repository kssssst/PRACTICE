#ifndef TRAYSERVICE_H
#define TRAYSERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

// RPC interface functions (implementation in TrayService.cpp)
error_status_t StopService(void);
error_status_t GetServiceStatus(long *status);

// Service handle will be declared by RPC generated code
extern RPC_IF_HANDLE ITrayService_ServerIfHandle;

#ifdef __cplusplus
}
#endif

#endif // TRAYSERVICE_H


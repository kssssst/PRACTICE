#ifndef TRAYSERVICE_CLIENT_H
#define TRAYSERVICE_CLIENT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

int InitializeRPCClient();
int StopServiceViaRPC();
int GetServiceStatusViaRPC(long *pStatus);
void CleanupRPCClient();
int CheckAndStartService();
int IsServiceRunning();
int WaitForServiceState(DWORD expectedState, DWORD timeoutMs);
int RequestServiceStopAndWait();

#ifdef __cplusplus
}
#endif

#endif

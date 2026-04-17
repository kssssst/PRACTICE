#ifndef TRAYSERVICE_CLIENT_H
#define TRAYSERVICE_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

int InitializeRPCClient();
int StopServiceViaRPC();
int GetServiceStatusViaRPC(long *pStatus);
void CleanupRPCClient();
int CheckAndStartService();
int IsServiceRunning();
DWORD GetParentProcessIdW(DWORD dwProcessId);

#ifdef __cplusplus
}
#endif

#endif
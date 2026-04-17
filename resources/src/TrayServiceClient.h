#ifndef TRAYSERVICE_CLIENT_H
#define TRAYSERVICE_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

// RPC client initialization and communication
int InitializeRPCClient();
int StopServiceViaRPC();
int GetServiceStatusViaRPC(long *pStatus);
void CleanupRPCClient();

// Service management functions
int CheckAndStartService();
int IsServiceRunning();
DWORD GetParentProcessIdW(DWORD dwProcessId);

#ifdef __cplusplus
}
#endif

#endif // TRAYSERVICE_CLIENT_H

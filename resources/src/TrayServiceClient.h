#ifndef TRAYSERVICE_CLIENT_H
#define TRAYSERVICE_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

// RPC client functions
int InitializeRPCClient();
int StopServiceViaRPC();
void CleanupRPCClient();

// Service management functions
int CheckAndStartService();
int IsServiceRunning();

#ifdef __cplusplus
}
#endif

#endif // TRAYSERVICE_CLIENT_H

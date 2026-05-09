#ifndef TRAYSERVICE_CLIENT_H
#define TRAYSERVICE_CLIENT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

int InitializeRPCClient();
int StopServiceViaRPC();
int GetServiceStatusViaRPC(long *pStatus);
int GetCurrentUserViaRPC(wchar_t *email, int emailChars, wchar_t *message, int messageChars, int *authenticated);
int LoginViaRPC(const wchar_t *email, const wchar_t *password, wchar_t *message, int messageChars, int *authenticated);
int LogoutViaRPC();
int GetLicenseInfoViaRPC(int *hasLicense, int *blocked, wchar_t *expirationDate, int expirationChars, wchar_t *message, int messageChars);
int ActivateProductViaRPC(const wchar_t *activationKey, int *hasLicense, int *blocked, wchar_t *expirationDate, int expirationChars, wchar_t *message, int messageChars);
int GetAvDatabaseInfoViaRPC(int *loaded, int *recordCount, wchar_t *releaseDate, int releaseDateChars, wchar_t *message, int messageChars);
int ScanFileViaRPC(const wchar_t *path, int *scannedFiles, int *infectedFiles, wchar_t *threatName, int threatNameChars, wchar_t *objectPath, int objectPathChars, wchar_t *message, int messageChars);
int ScanDirectoryViaRPC(const wchar_t *path, int *scannedFiles, int *infectedFiles, wchar_t *threatName, int threatNameChars, wchar_t *objectPath, int objectPathChars, wchar_t *message, int messageChars);
int ScanFixedDrivesViaRPC(int *scannedFiles, int *infectedFiles, wchar_t *threatName, int threatNameChars, wchar_t *objectPath, int objectPathChars, wchar_t *message, int messageChars);
int ConfigureScheduledScanViaRPC(int enabled, int intervalMinutes, const wchar_t *path, wchar_t *message, int messageChars);
int ConfigureDirectoryMonitoringViaRPC(int enabled, const wchar_t *path, wchar_t *message, int messageChars);
void CleanupRPCClient();
int CheckAndStartService();
int IsServiceRunning();
int WaitForServiceState(DWORD expectedState, DWORD timeoutMs);
int RequestServiceStopAndWait();

#ifdef __cplusplus
}
#endif

#endif

#ifndef TRAYSERVICE_H
#define TRAYSERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

error_status_t StopService(void);
error_status_t GetServiceStatus(long *status);

#ifdef __cplusplus
}
#endif

#endif // TRAYSERVICE_H

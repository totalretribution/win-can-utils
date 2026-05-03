#ifndef WINCAN_CLIENT_H
#define WINCAN_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "wincan/wincan.h"

/* -------------------------------------------------------------------------
 * Extended open configuration
 *
 * use_server = 0: open USB device directly (same as wincan_open)
 * use_server = 1: connect to wincan_server at 127.0.0.1:29526
 *
 * When use_server = 0, device_index is ignored (defaults to 0).
 * Use wincan_open() directly if you need a specific device index.
 * ---------------------------------------------------------------------- */
typedef struct {
    int      channel;
    int      bitrate_kbps;
    int      rx_buffer_size;
    int      use_server;
} wincan_config_ex_t;

/* Open a CAN channel using the extended config.
   Returns a handle on success, NULL on failure.
   The returned handle is compatible with all wincan_* functions. */
wincan_bus_t *wincan_open_ex(const wincan_config_ex_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* WINCAN_CLIENT_H */

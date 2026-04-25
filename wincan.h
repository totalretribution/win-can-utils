#ifndef WINCAN_H
#define WINCAN_H

#include <windows.h>
#include <winusb.h>
#include "gs_usb.h"

#define WINCAN_VID  0x1D50
#define WINCAN_PID  0x606F

#define WINCAN_EP_IN   0x81
#define WINCAN_EP_OUT  0x02

typedef struct {
    HANDLE                dev;
    WINUSB_INTERFACE_HANDLE usb;
} WINCAN_DEV;

/* Return the number of candleLight devices currently connected. */
int wincan_device_count(void);

/* Open the Nth candleLight device (device_index 0 = first).
   Returns NULL on failure. */
WINCAN_DEV *wincan_open(int device_index);

/* Close device and free handle. */
void wincan_close(WINCAN_DEV *d);

/* Set bitrate (kbps: 125, 250, 500, 1000) and enable a channel (0 or 1).
   Returns 0 on success, -1 on error. */
int wincan_channel_init(WINCAN_DEV *d, int channel, int bitrate_kbps);

/* Reset (disable) a channel. */
void wincan_channel_reset(WINCAN_DEV *d, int channel);

/* Send a frame. Returns 0 on success. */
int wincan_send(WINCAN_DEV *d, const struct gs_host_frame *f);

/* Receive a frame. timeout_ms=0 means block indefinitely.
   Returns 1 on success, 0 on timeout/error. */
int wincan_recv(WINCAN_DEV *d, struct gs_host_frame *f, DWORD timeout_ms);

#endif /* WINCAN_H */

#ifndef WINCAN_INTERNAL_H
#define WINCAN_INTERNAL_H

#ifdef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

#include <winsock2.h>
#include <windows.h>
#include <winusb.h>
#include "wincan/wincan.h"

/* -------------------------------------------------------------------------
 * Backend tag
 * ---------------------------------------------------------------------- */
typedef enum { BACKEND_USB = 0, BACKEND_NET = 1 } wincan_backend_t;

/* -------------------------------------------------------------------------
 * Receive ring buffer
 * ---------------------------------------------------------------------- */
typedef struct {
    wincan_frame_t  *frames;
    int              capacity;
    int              head, tail, count;
    uint32_t         overflows;
    CRITICAL_SECTION lock;
    HANDLE           not_empty;
} rx_ring_t;

int  ring_init(rx_ring_t *r, int cap);
void ring_destroy(rx_ring_t *r);
void ring_push(rx_ring_t *r, const wincan_frame_t *f);
int  ring_pop(rx_ring_t *r, wincan_frame_t *f, uint32_t timeout_ms);

/* -------------------------------------------------------------------------
 * Software filter matching (shared by USB and net backends)
 * ---------------------------------------------------------------------- */
int frame_passes_filters(wincan_bus_t *bus, const wincan_frame_t *f);

/* -------------------------------------------------------------------------
 * Shared USB device slot
 * ---------------------------------------------------------------------- */
#define DEV_SLOTS 8

typedef struct {
    char                    path[512];
    HANDLE                  dev;
    WINUSB_INTERFACE_HANDLE usb;
    int                     refs;
    struct wincan_bus      *ch_bus[2];
    volatile int            rx_running;
    volatile int            device_lost; /* set by rx thread on unrecoverable USB error */
    HANDLE                  rx_thread;
    HANDLE                  rx_event;    /* manual-reset event for overlapped reads */
    CRITICAL_SECTION        rx_lock;
} dev_slot_t;

/* -------------------------------------------------------------------------
 * wincan_bus_t definition (opaque to public consumers)
 * ---------------------------------------------------------------------- */
struct wincan_bus {
    wincan_backend_t        backend;

    /* USB backend */
    dev_slot_t             *slot;
    HANDLE                  dev;
    WINUSB_INTERFACE_HANDLE usb;
    int                     channel;

    /* Software filters (both backends) */
    wincan_filter_t        *filters;
    int                     filter_count;
    CRITICAL_SECTION        filter_lock;

    /* Receive ring (both backends) */
    rx_ring_t               ring;

    /* Net backend */
    SOCKET                  net_sock;
    volatile int            net_rx_running;
    HANDLE                  net_rx_thread;
};

/* -------------------------------------------------------------------------
 * Net backend entry points (wincan_net.c)
 * ---------------------------------------------------------------------- */
wincan_bus_t *wincan_net_open(uint8_t channel, uint32_t bitrate_kbps,
                              uint32_t rx_buffer_size);
void          wincan_net_close(wincan_bus_t *bus);
int           wincan_net_send(wincan_bus_t *bus, const wincan_frame_t *frame,
                              uint32_t timeout_ms);
int           wincan_net_set_filters(wincan_bus_t *bus,
                                     const wincan_filter_t *filters, int count);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* WINCAN_INTERNAL_H */

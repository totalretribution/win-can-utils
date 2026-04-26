#ifndef WINCAN_H
#define WINCAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Error codes
 * ---------------------------------------------------------------------- */
#define WINCAN_OK            0
#define WINCAN_ERR_NOTFOUND -1   /* device not found / not plugged in     */
#define WINCAN_ERR_IO       -2   /* USB I/O error                         */
#define WINCAN_ERR_TIMEOUT  -3   /* operation timed out                   */
#define WINCAN_ERR_PARAM    -4   /* invalid parameter                     */
#define WINCAN_ERR_OVERFLOW -5   /* rx ring buffer overflow               */
#define WINCAN_ERR_NOMEM    -6   /* memory allocation failed              */

/* -------------------------------------------------------------------------
 * Receive buffer sizing
 *
 * SocketCAN's default per-socket receive buffer is 212992 bytes.
 * A classic CAN frame (struct can_frame) is 16 bytes, giving ~13 300 frames.
 *
 * WINCAN_DEFAULT_RX_BUFFER matches that capacity.
 * WINCAN_MIN_RX_BUFFER  — absolute minimum; smaller values are rejected.
 * WINCAN_MAX_RX_BUFFER  — sanity cap (~1 million frames, ~16 MB).
 *
 * Pass one of these as wincan_config_t.rx_buffer_size, or choose your own
 * value between WINCAN_MIN and WINCAN_MAX.
 * ---------------------------------------------------------------------- */
#define WINCAN_DEFAULT_RX_BUFFER  13312   /* ≈ SocketCAN default           */
#define WINCAN_MIN_RX_BUFFER         64   /* minimum sensible size         */
#define WINCAN_MAX_RX_BUFFER    1048576   /* 1 M frames (~16 MB)           */

/* -------------------------------------------------------------------------
 * CAN bus state (from device hardware counters)
 * ---------------------------------------------------------------------- */
typedef enum {
    WINCAN_STATE_ERROR_ACTIVE  = 0,
    WINCAN_STATE_ERROR_WARNING = 1,
    WINCAN_STATE_ERROR_PASSIVE = 2,
    WINCAN_STATE_BUS_OFF       = 3,
} wincan_state_t;

/* -------------------------------------------------------------------------
 * Bit timing  (used when wincan_config_t.bitrate_kbps == 0)
 *
 * All fields are for the STM32 bxCAN peripheral.
 * bit_time = (1 + prop_seg + phase_seg1 + phase_seg2) TQ
 * baud     = can_clock_hz / (brp * bit_time)
 *
 * Common presets at 48 MHz CAN clock (16 TQ/bit):
 *   125 kbps: brp=24  500 kbps: brp=6
 *   250 kbps: brp=12  1 Mbps:   brp=3
 *   prop_seg=1, phase_seg1=11, phase_seg2=3, sjw=1 for all above
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t prop_seg;
    uint32_t phase_seg1;
    uint32_t phase_seg2;
    uint32_t sjw;
    uint32_t brp;
} wincan_bittiming_t;

/* -------------------------------------------------------------------------
 * Open configuration
 * ---------------------------------------------------------------------- */
typedef struct {
    /* When multiple candleLight adapters are connected, select which one.
       0 = first found (default), 1 = second, etc.
       Use wincan_device_count() to discover how many are present. */
    int device_index;

    int channel;                /* 0 = can0, 1 = can1                      */

    /* Bitrate — set one or the other, not both.
       bitrate_kbps: 125 / 250 / 500 / 1000  (0 = use custom timing below) */
    int                bitrate_kbps;
    wincan_bittiming_t timing;      /* used when bitrate_kbps == 0         */

    int listen_only;            /* 1 = silent monitoring, no TX            */
    int loopback;               /* 1 = hardware loopback (for testing)     */

    /* Receive ring buffer size in frames.
       0 = no background thread; wincan_recv() reads directly from USB.
       > 0 = background thread fills this ring; wincan_recv() pops from it. */
    int rx_buffer_size;
} wincan_config_t;

/* -------------------------------------------------------------------------
 * CAN frame
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t id;        /* 11-bit SFF or 29-bit EFF identifier            */
    int      ext;       /* 1 = extended 29-bit frame (EFF)                */
    int      rtr;       /* 1 = remote transmission request                */
    uint8_t  dlc;       /* data length code (0–8)                         */
    uint8_t  data[8];
} wincan_frame_t;

/* -------------------------------------------------------------------------
 * Receive filter
 *
 * A frame is accepted when: (frame_id & mask) == (id & mask)
 * ext: 1 = match EFF frames only
 *      0 = match SFF frames only
 *     -1 = match both
 *
 * Call wincan_set_filters(bus, NULL, 0) to clear all filters (accept all).
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t id;
    uint32_t mask;
    int      ext;
} wincan_filter_t;

/* -------------------------------------------------------------------------
 * Connection status
 * ---------------------------------------------------------------------- */
typedef struct {
    wincan_state_t state;       /* hardware error state                   */
    uint32_t rx_errors;         /* hardware RX error counter              */
    uint32_t tx_errors;         /* hardware TX error counter              */
    uint32_t rx_buffered;       /* frames currently in the ring buffer    */
    uint32_t rx_overflows;      /* frames dropped due to full ring buffer */
} wincan_status_t;

/* Opaque bus handle */
typedef struct wincan_bus wincan_bus_t;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/* Return the number of candleLight adapters currently connected. */
int wincan_device_count(void);

/* Open and initialise a CAN channel.
   Returns a handle on success, NULL on failure. */
wincan_bus_t *wincan_open(const wincan_config_t *cfg);

/* Transmit a frame.
   timeout_ms: milliseconds to wait for the USB write to complete.
               0 = use a short default (non-blocking attempt).
   Returns WINCAN_OK or an error code. */
int wincan_send(wincan_bus_t *bus, const wincan_frame_t *frame,
                uint32_t timeout_ms);

/* Receive a frame.
   timeout_ms: 0          = return immediately (WINCAN_ERR_TIMEOUT if empty)
               UINT32_MAX = block until a frame arrives
               other      = wait up to that many milliseconds
   Returns WINCAN_OK, WINCAN_ERR_TIMEOUT, or an error code. */
int wincan_recv(wincan_bus_t *bus, wincan_frame_t *frame, uint32_t timeout_ms);

/* Replace the software receive filter list.
   filters=NULL / count=0 clears all filters (accept everything).
   Returns WINCAN_OK or WINCAN_ERR_NOMEM. */
int wincan_set_filters(wincan_bus_t *bus,
                       const wincan_filter_t *filters, int count);

/* Query current hardware state and buffer statistics.
   Returns WINCAN_OK or WINCAN_ERR_IO if the device does not respond. */
int wincan_get_status(wincan_bus_t *bus, wincan_status_t *status);

/* Reset the channel and free all resources. Safe to call with NULL. */
void wincan_close(wincan_bus_t *bus);

/* Return a human-readable string for an error code. */
const char *wincan_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* WINCAN_H */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include "wincan/wincan.h"
#include "wincan/wincan_client.h"
#include "wincan_internal.h"

/* =========================================================================
 * gs_usb protocol definitions
 * ====================================================================== */

#define GS_USB_VID  0x1D50
#define GS_USB_PID  0x606F

#define GS_EP_IN    0x81
#define GS_EP_OUT   0x02

#define GS_BREQ_HOST_FORMAT 0x00
#define GS_BREQ_BITTIMING   0x01
#define GS_BREQ_MODE        0x02
#define GS_BREQ_BT_CONST    0x04
#define GS_BREQ_DEVICE_CFG  0x05
#define GS_BREQ_GET_STATE   0x0E

#define GS_HOST_BYTE_ORDER  0x0000BEEFu

#define GS_MODE_RESET  0
#define GS_MODE_START  1

#define GS_FEATURE_LISTEN_ONLY  (1u << 0)
#define GS_FEATURE_LOOP_BACK    (1u << 1)

#define GS_CAN_EFF_FLAG  0x80000000U
#define GS_CAN_RTR_FLAG  0x40000000U
#define GS_CAN_SFF_MASK  0x000007FFU
#define GS_CAN_EFF_MASK  0x1FFFFFFFU
#define GS_ECHO_RX       0xFFFFFFFFU

#pragma pack(push, 1)
typedef struct { uint32_t prop_seg, phase_seg1, phase_seg2, sjw, brp; } gs_bittiming_t;
typedef struct { uint32_t mode, feature; }                               gs_mode_t;
typedef struct { uint32_t state, rxerr, txerr; }                         gs_state_t;
typedef struct {
    uint8_t  reserved[3];
    uint8_t  icount;     /* number of channels minus 1 */
    uint32_t sw_version;
    uint32_t hw_version;
} gs_device_config_t;
typedef struct {
    uint32_t feature;
    uint32_t fclk_can;
    uint32_t tseg1_min, tseg1_max;
    uint32_t tseg2_min, tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min, brp_max, brp_inc;
} gs_bt_const_t;
typedef struct {
    uint32_t echo_id, can_id;
    uint8_t  can_dlc, channel, flags, reserved;
    uint8_t  data[8];
} gs_frame_t;
#pragma pack(pop)

/* TQ count used for standard bittiming: sync(1)+prop(1)+seg1(12)+seg2(2)=16.
   Sample point = 14/16 = 87.5%, matches SocketCAN defaults. */
#define GS_STD_TQ   16u
#define GS_PROP_SEG  1u
#define GS_SEG1     12u
#define GS_SEG2      2u
#define GS_SJW       1u

/* WinUSB GUID for candleLight / gs_usb devices */
static const GUID WINUSB_GUID =
    { 0xDEE824EF, 0x729B, 0x4A0E,
      { 0x9C, 0x14, 0xB7, 0x11, 0x7D, 0x33, 0xA8, 0x17 } };

/* =========================================================================
 * Shared device registry
 * ====================================================================== */

static dev_slot_t        g_devs[DEV_SLOTS];
static CRITICAL_SECTION  g_dev_lock;
static INIT_ONCE         g_dev_init = INIT_ONCE_STATIC_INIT;

static BOOL WINAPI dev_lock_init(PINIT_ONCE io, PVOID p, PVOID *ctx)
{
    (void)io; (void)p; (void)ctx;
    InitializeCriticalSection(&g_dev_lock);
    memset(g_devs, 0, sizeof(g_devs));
    return TRUE;
}

static void slot_send_host_format(WINUSB_INTERFACE_HANDLE usb)
{
    WINUSB_SETUP_PACKET pkt = {
        .RequestType = 0x41,
        .Request     = GS_BREQ_HOST_FORMAT,
        .Value       = 1,
        .Index       = 0,
        .Length      = 4
    };
    uint32_t byte_order = GS_HOST_BYTE_ORDER;
    ULONG t;
    WinUsb_ControlTransfer(usb, pkt, (PUCHAR)&byte_order, 4, &t, NULL);
}

static dev_slot_t *dev_acquire(const char *path)
{
    InitOnceExecuteOnce(&g_dev_init, dev_lock_init, NULL, NULL);
    EnterCriticalSection(&g_dev_lock);

    for (int i = 0; i < DEV_SLOTS; i++) {
        if (g_devs[i].refs > 0 && strcmp(g_devs[i].path, path) == 0) {
            g_devs[i].refs++;
            LeaveCriticalSection(&g_dev_lock);
            return &g_devs[i];
        }
    }

    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "wincan: CreateFile failed: %lu\n", GetLastError());
        LeaveCriticalSection(&g_dev_lock);
        return NULL;
    }
    WINUSB_INTERFACE_HANDLE usb;
    if (!WinUsb_Initialize(h, &usb)) {
        fprintf(stderr, "wincan: WinUsb_Initialize failed: %lu\n", GetLastError());
        CloseHandle(h);
        LeaveCriticalSection(&g_dev_lock);
        return NULL;
    }
    slot_send_host_format(usb);

    for (int i = 0; i < DEV_SLOTS; i++) {
        if (g_devs[i].refs == 0) {
            strncpy(g_devs[i].path, path, sizeof(g_devs[i].path) - 1);
            g_devs[i].dev        = h;
            g_devs[i].usb        = usb;
            g_devs[i].refs       = 1;
            g_devs[i].ch_bus[0]  = NULL;
            g_devs[i].ch_bus[1]  = NULL;
            g_devs[i].rx_running  = 0;
            g_devs[i].rx_thread   = NULL;
            g_devs[i].device_lost = 0;
            g_devs[i].rx_event    = CreateEvent(NULL, TRUE, FALSE, NULL);
            InitializeCriticalSection(&g_devs[i].rx_lock);
            LeaveCriticalSection(&g_dev_lock);
            return &g_devs[i];
        }
    }

    fprintf(stderr, "wincan: too many open devices (max %d)\n", DEV_SLOTS);
    WinUsb_Free(usb);
    CloseHandle(h);
    LeaveCriticalSection(&g_dev_lock);
    return NULL;
}

static void dev_release(dev_slot_t *slot)
{
    EnterCriticalSection(&g_dev_lock);
    if (--slot->refs == 0) {
        WinUsb_Free(slot->usb);
        CloseHandle(slot->dev);
        if (slot->rx_event) { CloseHandle(slot->rx_event); slot->rx_event = NULL; }
        DeleteCriticalSection(&slot->rx_lock);
        slot->path[0] = '\0';
    }
    LeaveCriticalSection(&g_dev_lock);
}

/* =========================================================================
 * USB helpers
 * ====================================================================== */

static char *find_device_path(int device_index)
{
    HDEVINFO dev_info;
    SP_DEVICE_INTERFACE_DATA iface;
    PSP_DEVICE_INTERFACE_DETAIL_DATA detail;
    DWORD required;
    char *path = NULL;
    int found = 0;

    dev_info = SetupDiGetClassDevs(&WINUSB_GUID, NULL, NULL,
                                   DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) return NULL;

    iface.cbSize = sizeof(iface);
    for (int idx = 0; ; idx++) {
        if (!SetupDiEnumDeviceInterfaces(dev_info, NULL, &WINUSB_GUID, idx, &iface))
            break;
        SetupDiGetDeviceInterfaceDetail(dev_info, &iface, NULL, 0, &required, NULL);
        detail = malloc(required);
        if (!detail) break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        if (SetupDiGetDeviceInterfaceDetail(dev_info, &iface, detail,
                                             required, NULL, NULL)) {
            if (strstr(detail->DevicePath, "vid_1d50") &&
                strstr(detail->DevicePath, "pid_606f")) {
                if (found == device_index) {
                    path = _strdup(detail->DevicePath);
                    free(detail);
                    break;
                }
                found++;
            }
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(dev_info);
    return path;
}

int wincan_device_count(void)
{
    HDEVINFO dev_info;
    SP_DEVICE_INTERFACE_DATA iface;
    PSP_DEVICE_INTERFACE_DETAIL_DATA detail;
    DWORD required;
    int count = 0;

    dev_info = SetupDiGetClassDevs(&WINUSB_GUID, NULL, NULL,
                                   DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) return 0;

    iface.cbSize = sizeof(iface);
    for (int idx = 0; ; idx++) {
        if (!SetupDiEnumDeviceInterfaces(dev_info, NULL, &WINUSB_GUID, idx, &iface))
            break;
        SetupDiGetDeviceInterfaceDetail(dev_info, &iface, NULL, 0, &required, NULL);
        detail = malloc(required);
        if (!detail) break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        if (SetupDiGetDeviceInterfaceDetail(dev_info, &iface, detail,
                                             required, NULL, NULL)) {
            if (strstr(detail->DevicePath, "vid_1d50") &&
                strstr(detail->DevicePath, "pid_606f"))
                count++;
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(dev_info);
    return count;
}

static int ctrl_out(wincan_bus_t *b, uint8_t req, uint16_t val,
                    void *data, uint16_t len)
{
    WINUSB_SETUP_PACKET pkt = {
        .RequestType = 0x41,
        .Request = req, .Value = val, .Index = 0, .Length = len
    };
    ULONG t;
    return WinUsb_ControlTransfer(b->usb, pkt, data, len, &t, NULL) ? 0 : -1;
}

static int ctrl_in(wincan_bus_t *b, uint8_t req, uint16_t val,
                   void *data, uint16_t len)
{
    WINUSB_SETUP_PACKET pkt = {
        .RequestType = 0xC1,
        .Request = req, .Value = val, .Index = 0, .Length = len
    };
    ULONG t = 0;
    return WinUsb_ControlTransfer(b->usb, pkt, data, len, &t, NULL) ? 0 : -1;
}

/* =========================================================================
 * Ring buffer (shared by USB and net backends)
 * ====================================================================== */

int ring_init(rx_ring_t *r, int cap)
{
    r->frames = malloc(sizeof(wincan_frame_t) * cap);
    if (!r->frames) return WINCAN_ERR_NOMEM;
    r->capacity = cap;
    r->head = r->tail = r->count = 0;
    r->overflows = 0;
    InitializeCriticalSection(&r->lock);
    r->not_empty = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!r->not_empty) { free(r->frames); r->frames = NULL; return WINCAN_ERR_IO; }
    return WINCAN_OK;
}

void ring_destroy(rx_ring_t *r)
{
    if (!r->capacity) return;
    free(r->frames); r->frames = NULL;
    CloseHandle(r->not_empty); r->not_empty = NULL;
    DeleteCriticalSection(&r->lock);
    r->capacity = 0;
}

void ring_push(rx_ring_t *r, const wincan_frame_t *f)
{
    EnterCriticalSection(&r->lock);
    if (r->count < r->capacity) {
        r->frames[r->tail] = *f;
        r->tail = (r->tail + 1) % r->capacity;
        r->count++;
        SetEvent(r->not_empty);
    } else {
        r->overflows++;
    }
    LeaveCriticalSection(&r->lock);
}

int ring_pop(rx_ring_t *r, wincan_frame_t *f, uint32_t timeout_ms)
{
    DWORD w = (timeout_ms == UINT32_MAX) ? INFINITE : (DWORD)timeout_ms;
    if (WaitForSingleObject(r->not_empty, w) == WAIT_TIMEOUT)
        return WINCAN_ERR_TIMEOUT;
    EnterCriticalSection(&r->lock);
    if (r->count == 0) {
        LeaveCriticalSection(&r->lock);
        return WINCAN_ERR_TIMEOUT;
    }
    *f = r->frames[r->head];
    r->head = (r->head + 1) % r->capacity;
    r->count--;
    if (r->count > 0) SetEvent(r->not_empty);
    LeaveCriticalSection(&r->lock);
    return WINCAN_OK;
}

/* =========================================================================
 * Filter matching
 * ====================================================================== */

int frame_passes_filters(wincan_bus_t *b, const wincan_frame_t *f)
{
    EnterCriticalSection(&b->filter_lock);
    if (b->filter_count == 0) {
        LeaveCriticalSection(&b->filter_lock);
        return 1;
    }
    for (int i = 0; i < b->filter_count; i++) {
        const wincan_filter_t *fl = &b->filters[i];
        if (fl->ext == 1 && !f->ext) continue;
        if (fl->ext == 0 &&  f->ext) continue;
        if ((f->id & fl->mask) == (fl->id & fl->mask)) {
            LeaveCriticalSection(&b->filter_lock);
            return 1;
        }
    }
    LeaveCriticalSection(&b->filter_lock);
    return 0;
}

/* =========================================================================
 * Frame conversion
 * ====================================================================== */

static void gs_to_frame(const gs_frame_t *g, wincan_frame_t *f)
{
    f->ext = (g->can_id & GS_CAN_EFF_FLAG) != 0;
    f->rtr = (g->can_id & GS_CAN_RTR_FLAG) != 0;
    f->id  = g->can_id & (f->ext ? GS_CAN_EFF_MASK : GS_CAN_SFF_MASK);
    f->dlc = g->can_dlc > 8 ? 8 : g->can_dlc;
    memcpy(f->data, g->data, f->dlc);
}

static void frame_to_gs(const wincan_frame_t *f, int channel,
                        uint32_t echo_id, gs_frame_t *g)
{
    memset(g, 0, sizeof(*g));
    g->echo_id = echo_id;
    g->can_id  = f->id;
    if (f->ext) g->can_id |= GS_CAN_EFF_FLAG;
    if (f->rtr) g->can_id |= GS_CAN_RTR_FLAG;
    g->can_dlc  = f->dlc;
    g->channel  = (uint8_t)channel;
    memcpy(g->data, f->data, f->dlc);
}

/* =========================================================================
 * Per-device RX dispatch thread
 * ====================================================================== */

/* Returns: 1=frame received, 0=no data (timeout), -1=device removed/error.
   Uses overlapped I/O so the read can be cancelled and never hangs. */
static int raw_recv(dev_slot_t *slot, gs_frame_t *g)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = slot->rx_event;
    ResetEvent(ov.hEvent);

    ULONG transferred = 0;
    BOOL ok = WinUsb_ReadPipe(slot->usb, GS_EP_IN,
                               (PUCHAR)g, sizeof(*g), &transferred, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING)
            return -1; /* immediate failure — device gone */

        /* Wait up to 150 ms for data */
        int timed_out = (WaitForSingleObject(ov.hEvent, 150) == WAIT_TIMEOUT);
        if (timed_out) {
            WinUsb_AbortPipe(slot->usb, GS_EP_IN);
            WaitForSingleObject(ov.hEvent, 2000);
        }

        if (!WinUsb_GetOverlappedResult(slot->usb, &ov, &transferred, FALSE)) {
            DWORD err2 = GetLastError();
            /* OPERATION_ABORTED = we cancelled it ourselves (timeout) */
            if (timed_out && err2 == ERROR_OPERATION_ABORTED) return 0;
            return -1; /* any other error = device gone */
        }
    }

    return (int)transferred == (int)sizeof(*g) ? 1 : 0;
}

static DWORD WINAPI slot_rx_thread(LPVOID param)
{
    dev_slot_t *slot = (dev_slot_t *)param;
    gs_frame_t g;
    wincan_frame_t f;
    int err_count = 0;

    while (slot->rx_running) {
        int r = raw_recv(slot, &g);
        if (r == 0) { err_count = 0; continue; }
        if (r < 0) {
            /* Require 3 consecutive errors — filters transient USB errors on
               reconnect / CAN error frames that briefly look like USB failures. */
            if (++err_count >= 3) {
                slot->device_lost = 1;
                break;
            }
            Sleep(100);
            continue;
        }
        err_count = 0;

        if (g.echo_id != GS_ECHO_RX) continue;

        int ch = (int)(uint8_t)g.channel;
        if (ch > 1) continue;

        EnterCriticalSection(&slot->rx_lock);
        wincan_bus_t *b = slot->ch_bus[ch];
        if (b) {
            gs_to_frame(&g, &f);
            if (frame_passes_filters(b, &f))
                ring_push(&b->ring, &f);
        }
        LeaveCriticalSection(&slot->rx_lock);
    }

    slot->rx_running = 0;
    return 0;
}

/* =========================================================================
 * Public API — wincan_open
 * ====================================================================== */

wincan_bus_t *wincan_open(const wincan_config_t *cfg)
{
    if (!cfg || cfg->channel < 0 || cfg->channel > 1) return NULL;

    gs_bittiming_t bt;
    memset(&bt, 0, sizeof(bt));

    char *path = find_device_path(cfg->device_index);
    if (!path) {
        int total = wincan_device_count();
        if (total == 0)
            fprintf(stderr, "wincan: no candleLight device found\n");
        else
            fprintf(stderr, "wincan: device index %d not found "
                            "(%d device(s) connected, indices 0-%d)\n",
                    cfg->device_index, total, total - 1);
        return NULL;
    }
    dev_slot_t *slot = dev_acquire(path);
    free(path);
    if (!slot) return NULL;

    wincan_bus_t *b = calloc(1, sizeof(*b));
    if (!b) { dev_release(slot); return NULL; }
    b->backend = BACKEND_USB;
    b->slot    = slot;
    b->dev     = slot->dev;
    b->usb     = slot->usb;
    b->channel = cfg->channel;
    b->net_sock = INVALID_SOCKET;
    InitializeCriticalSection(&b->filter_lock);

    {
        gs_device_config_t dcfg;
        if (ctrl_in(b, GS_BREQ_DEVICE_CFG, 1,
                    &dcfg, sizeof(dcfg)) == 0) {
            if (cfg->channel > (int)dcfg.icount) {
                fprintf(stderr, "wincan: channel %d not available "
                                "(device has %d channel(s), indices 0-%d)\n",
                        cfg->channel, (int)dcfg.icount + 1, (int)dcfg.icount);
                goto fail;
            }
        }
    }

    if (cfg->bitrate_kbps != 0) {
        gs_bt_const_t btc;
        uint32_t fclk = 48000000u;
        if (ctrl_in(b, GS_BREQ_BT_CONST, (uint16_t)cfg->channel,
                    &btc, sizeof(btc)) == 0 && btc.fclk_can > 0)
            fclk = btc.fclk_can;

        uint32_t hz = (uint32_t)cfg->bitrate_kbps * 1000u;
        if (fclk % (hz * GS_STD_TQ) != 0) {
            fprintf(stderr, "wincan: %d kbps not achievable exactly with "
                            "device clock %lu Hz (use .timing for custom bittiming)\n",
                    cfg->bitrate_kbps, (unsigned long)fclk);
            goto fail;
        }
        bt.prop_seg   = GS_PROP_SEG;
        bt.phase_seg1 = GS_SEG1;
        bt.phase_seg2 = GS_SEG2;
        bt.sjw        = GS_SJW;
        bt.brp        = fclk / (hz * GS_STD_TQ);
    } else {
        bt.prop_seg   = cfg->timing.prop_seg;
        bt.phase_seg1 = cfg->timing.phase_seg1;
        bt.phase_seg2 = cfg->timing.phase_seg2;
        bt.sjw        = cfg->timing.sjw;
        bt.brp        = cfg->timing.brp;
    }

    gs_mode_t reset_mode = { GS_MODE_RESET, 0 };
    ctrl_out(b, GS_BREQ_MODE, (uint16_t)cfg->channel,
             &reset_mode, sizeof(reset_mode));

    if (ctrl_out(b, GS_BREQ_BITTIMING, (uint16_t)cfg->channel,
                 &bt, sizeof(bt)) != 0) {
        fprintf(stderr, "wincan: BITTIMING failed: %lu\n", GetLastError());
        goto fail;
    }

    uint32_t features = 0;
    if (cfg->listen_only) features |= GS_FEATURE_LISTEN_ONLY;
    if (cfg->loopback)    features |= GS_FEATURE_LOOP_BACK;
    gs_mode_t mode = { GS_MODE_START, features };
    if (ctrl_out(b, GS_BREQ_MODE, (uint16_t)cfg->channel,
                 &mode, sizeof(mode)) != 0) {
        fprintf(stderr, "wincan: MODE START failed: %lu\n", GetLastError());
        goto fail;
    }

    {
        int cap = cfg->rx_buffer_size > 0 ? cfg->rx_buffer_size
                                          : WINCAN_DEFAULT_RX_BUFFER;
        if (cap < WINCAN_MIN_RX_BUFFER) {
            fprintf(stderr, "wincan: rx_buffer_size %d below minimum %d, "
                            "using %d\n", cap, WINCAN_MIN_RX_BUFFER,
                            WINCAN_MIN_RX_BUFFER);
            cap = WINCAN_MIN_RX_BUFFER;
        } else if (cap > WINCAN_MAX_RX_BUFFER) {
            fprintf(stderr, "wincan: rx_buffer_size %d exceeds maximum %d, "
                            "clamping\n", cap, WINCAN_MAX_RX_BUFFER);
            cap = WINCAN_MAX_RX_BUFFER;
        }
        if (ring_init(&b->ring, cap) != WINCAN_OK) {
            fprintf(stderr, "wincan: ring buffer allocation failed\n");
            goto fail;
        }
    }

    EnterCriticalSection(&slot->rx_lock);
    slot->ch_bus[cfg->channel] = b;
    int need_thread = !slot->rx_running;
    if (need_thread) slot->rx_running = 1;
    LeaveCriticalSection(&slot->rx_lock);

    if (need_thread) {
        slot->rx_thread = CreateThread(NULL, 0, slot_rx_thread, slot, 0, NULL);
        if (!slot->rx_thread) {
            fprintf(stderr, "wincan: CreateThread failed: %lu\n", GetLastError());
            EnterCriticalSection(&slot->rx_lock);
            slot->ch_bus[cfg->channel] = NULL;
            slot->rx_running = 0;
            LeaveCriticalSection(&slot->rx_lock);
            goto fail;
        }
    }

    return b;

fail:
    wincan_close(b);
    return NULL;
}

/* =========================================================================
 * Public API — wincan_open_ex (extended open with backend selection)
 * ====================================================================== */

wincan_bus_t *wincan_open_ex(const wincan_config_ex_t *cfg)
{
    if (!cfg) return NULL;

    if (cfg->use_server) {
        return wincan_net_open((uint8_t)cfg->channel,
                               (uint32_t)cfg->bitrate_kbps,
                               (uint32_t)cfg->rx_buffer_size);
    }

    wincan_config_t base = {0};
    /* Protocol channels: 0=vcan0, 1=vcan1, 2=can0, 3=can1.
       USB device only has physical channels 0 and 1. */
    base.channel        = (cfg->channel >= 2) ? cfg->channel - 2 : cfg->channel;
    base.bitrate_kbps   = cfg->bitrate_kbps;
    base.rx_buffer_size = (int)cfg->rx_buffer_size;
    return wincan_open(&base);
}

/* =========================================================================
 * Public API — wincan_send / wincan_recv / wincan_set_filters / etc.
 * ====================================================================== */

int wincan_send(wincan_bus_t *bus, const wincan_frame_t *frame,
                uint32_t timeout_ms)
{
    if (!bus || !frame) return WINCAN_ERR_PARAM;

    if (bus->backend == BACKEND_NET)
        return wincan_net_send(bus, frame, timeout_ms);

    static volatile uint32_t echo_seq = 1;
    gs_frame_t g;
    frame_to_gs(frame, bus->channel, echo_seq++, &g);

    DWORD t = timeout_ms > 0 ? (DWORD)timeout_ms : 500;
    WinUsb_SetPipePolicy(bus->usb, GS_EP_OUT,
                         PIPE_TRANSFER_TIMEOUT, sizeof(t), &t);

    ULONG transferred;
    if (!WinUsb_WritePipe(bus->usb, GS_EP_OUT,
                          (PUCHAR)&g, sizeof(g), &transferred, NULL))
        return WINCAN_ERR_IO;
    return WINCAN_OK;
}

int wincan_recv(wincan_bus_t *bus, wincan_frame_t *frame, uint32_t timeout_ms)
{
    if (!bus || !frame) return WINCAN_ERR_PARAM;
    if (bus->backend == BACKEND_USB && bus->slot && bus->slot->device_lost)
        return WINCAN_ERR_IO;
    return ring_pop(&bus->ring, frame, timeout_ms);
}

int wincan_set_filters(wincan_bus_t *bus,
                       const wincan_filter_t *filters, int count)
{
    if (!bus) return WINCAN_ERR_PARAM;

    if (bus->backend == BACKEND_NET)
        return wincan_net_set_filters(bus, filters, count);

    wincan_filter_t *copy = NULL;
    if (filters && count > 0) {
        copy = malloc(sizeof(wincan_filter_t) * count);
        if (!copy) return WINCAN_ERR_NOMEM;
        memcpy(copy, filters, sizeof(wincan_filter_t) * count);
    }

    EnterCriticalSection(&bus->filter_lock);
    free(bus->filters);
    bus->filters      = copy;
    bus->filter_count = (copy ? count : 0);
    LeaveCriticalSection(&bus->filter_lock);
    return WINCAN_OK;
}

int wincan_get_status(wincan_bus_t *bus, wincan_status_t *status)
{
    if (!bus || !status) return WINCAN_ERR_PARAM;

    if (bus->backend == BACKEND_NET)
        return WINCAN_ERR_IO; /* not implemented for net backend */

    memset(status, 0, sizeof(*status));

    gs_state_t gs;
    if (ctrl_in(bus, GS_BREQ_GET_STATE, (uint16_t)bus->channel,
                &gs, sizeof(gs)) != 0)
        return WINCAN_ERR_IO;

    status->state     = (wincan_state_t)gs.state;
    status->rx_errors = gs.rxerr;
    status->tx_errors = gs.txerr;

    EnterCriticalSection(&bus->ring.lock);
    status->rx_buffered  = (uint32_t)bus->ring.count;
    status->rx_overflows = bus->ring.overflows;
    LeaveCriticalSection(&bus->ring.lock);

    return WINCAN_OK;
}

void wincan_close(wincan_bus_t *bus)
{
    if (!bus) return;

    if (bus->backend == BACKEND_NET) {
        wincan_net_close(bus);
        return;
    }

    dev_slot_t *slot = bus->slot;

    if (slot) {
        EnterCriticalSection(&slot->rx_lock);
        if (slot->ch_bus[bus->channel] == bus)
            slot->ch_bus[bus->channel] = NULL;
        int stop = (slot->ch_bus[0] == NULL && slot->ch_bus[1] == NULL);
        if (stop) slot->rx_running = 0;
        LeaveCriticalSection(&slot->rx_lock);

        if (stop && slot->rx_thread) {
            WaitForSingleObject(slot->rx_thread, 2000);
            CloseHandle(slot->rx_thread);
            slot->rx_thread = NULL;
        }
    }

    gs_mode_t mode = { GS_MODE_RESET, 0 };
    ctrl_out(bus, GS_BREQ_MODE, (uint16_t)bus->channel, &mode, sizeof(mode));

    ring_destroy(&bus->ring);

    EnterCriticalSection(&bus->filter_lock);
    free(bus->filters);
    bus->filters = NULL;
    LeaveCriticalSection(&bus->filter_lock);
    DeleteCriticalSection(&bus->filter_lock);

    if (slot) dev_release(slot);
    free(bus);
}

const char *wincan_strerror(int err)
{
    switch (err) {
        case WINCAN_OK:           return "success";
        case WINCAN_ERR_NOTFOUND: return "device not found";
        case WINCAN_ERR_IO:       return "USB I/O error";
        case WINCAN_ERR_TIMEOUT:  return "timeout";
        case WINCAN_ERR_PARAM:    return "invalid parameter";
        case WINCAN_ERR_OVERFLOW: return "rx buffer overflow";
        case WINCAN_ERR_NOMEM:    return "out of memory";
        default:                  return "unknown error";
    }
}

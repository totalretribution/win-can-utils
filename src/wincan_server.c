#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "wincan/wincan.h"
#include "wincan/wincan_proto.h"

/*
 * Protocol channel numbering:
 *   0 = vcan0  (virtual, always available)
 *   1 = vcan1  (virtual, always available)
 *   2 = can0   (USB, may not be present)
 *   3 = can1   (USB, may not be present)
 */

#define MAX_CLIENTS  8
#define MAX_FILT     64
#define NUM_CHANNELS 4

/* USB channels occupy protocol slots 2 and 3.
   g_bus[0] = can0 (protocol ch 2), g_bus[1] = can1 (protocol ch 3). */
#define USB_CH_BASE  2
#define IS_VCAN(ch)  ((ch) < USB_CH_BASE)
#define IS_USB(ch)   ((ch) >= USB_CH_BASE && (ch) < NUM_CHANNELS)
#define USB_IDX(ch)  ((ch) - USB_CH_BASE)

typedef struct {
    SOCKET              sock;
    uint8_t             subscribed[NUM_CHANNELS];
    wincan_net_filter_t filters[MAX_FILT];
    int                 filter_count;
    HANDLE              thread;
    volatile int        active;
} client_slot_t;

static client_slot_t    g_clients[MAX_CLIENTS];
static CRITICAL_SECTION g_clients_lock;
static volatile int     g_server_running = 0;

static wincan_bus_t    *g_bus[2];        /* [0]=can0, [1]=can1 */
static CRITICAL_SECTION g_bus_lock;

static int g_bitrate_kbps;
static int g_channels_mask;  /* bit 0 = can0, bit 1 = can1 */
static int g_device_index;

/* =========================================================================
 * Per-client filter check
 * ====================================================================== */

static int client_passes_filters(const client_slot_t *slot,
                                  const wincan_frame_t *f)
{
    if (slot->filter_count == 0) return 1;
    for (int i = 0; i < slot->filter_count; i++) {
        const wincan_net_filter_t *fl = &slot->filters[i];
        if (fl->ext == 1 && !f->ext) continue;
        if (fl->ext == 0 &&  f->ext) continue;
        if ((f->id & fl->mask) == (fl->id & fl->mask)) return 1;
    }
    return 0;
}

/* =========================================================================
 * Open one USB channel — ch is the protocol channel number (2 or 3)
 * ====================================================================== */

static wincan_bus_t *open_channel(int ch)
{
    wincan_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.device_index   = g_device_index;
    cfg.channel        = USB_IDX(ch);   /* physical channel: 0 or 1 */
    cfg.bitrate_kbps   = g_bitrate_kbps;
    cfg.rx_buffer_size = WINCAN_DEFAULT_RX_BUFFER;
    return wincan_open(&cfg);
}

/* =========================================================================
 * vcan loopback: deliver frame to all subscribers of a vcan channel
 * ====================================================================== */

static void vcan_loopback(uint8_t ch, const wincan_frame_t *f)
{
    wincan_net_frame_t nf;
    wincan_frame_to_net(f, &nf, (uint64_t)GetTickCount64() * 1000ULL);

    SOCKET to_send[MAX_CLIENTS];
    int n_send = 0;

    EnterCriticalSection(&g_clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].active)          continue;
        if (!g_clients[i].subscribed[ch])  continue;
        if (!client_passes_filters(&g_clients[i], f)) continue;
        to_send[n_send++] = g_clients[i].sock;
    }
    LeaveCriticalSection(&g_clients_lock);

    for (int i = 0; i < n_send; i++)
        wincan_proto_send(to_send[i], WINCAN_PKT_FRAME, ch, &nf, sizeof(nf));
}

/* =========================================================================
 * Per-channel USB RX thread — param is protocol channel number (2 or 3)
 * ====================================================================== */

static DWORD WINAPI server_rx_thread(LPVOID param)
{
    int ch      = (int)(intptr_t)param;   /* protocol channel: 2 or 3 */
    int usb_idx = USB_IDX(ch);            /* g_bus index: 0 or 1 */
    wincan_frame_t f;
    wincan_net_frame_t nf;
    SOCKET to_send[MAX_CLIENTS];
    int n_send;

    while (g_server_running) {

        EnterCriticalSection(&g_bus_lock);
        wincan_bus_t *bus = g_bus[usb_idx];
        LeaveCriticalSection(&g_bus_lock);

        if (!bus) {
            Sleep(2000);
            if (!g_server_running) break;
            if (wincan_device_count() == 0) continue;
            bus = open_channel(ch);
            if (bus) {
                EnterCriticalSection(&g_bus_lock);
                g_bus[usb_idx] = bus;
                LeaveCriticalSection(&g_bus_lock);
                printf("wincan_server: can%d connected\n", usb_idx);
            } else {
                Sleep(3000);
            }
            continue;
        }

        int rc = wincan_recv(bus, &f, 100);

        if (rc == WINCAN_ERR_TIMEOUT) continue;

        if (rc == WINCAN_ERR_IO) {
            printf("wincan_server: can%d disconnected\n", usb_idx);
            EnterCriticalSection(&g_bus_lock);
            g_bus[usb_idx] = NULL;
            LeaveCriticalSection(&g_bus_lock);
            wincan_close(bus);
            continue;
        }

        if (rc != WINCAN_OK) break;

        wincan_frame_to_net(&f, &nf, (uint64_t)GetTickCount64() * 1000ULL);

        n_send = 0;
        EnterCriticalSection(&g_clients_lock);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].active)           continue;
            if (!g_clients[i].subscribed[ch])   continue;
            if (!client_passes_filters(&g_clients[i], &f)) continue;
            to_send[n_send++] = g_clients[i].sock;
        }
        LeaveCriticalSection(&g_clients_lock);

        for (int i = 0; i < n_send; i++)
            wincan_proto_send(to_send[i], WINCAN_PKT_FRAME, (uint8_t)ch, &nf, sizeof(nf));
    }

    return 0;
}

/* =========================================================================
 * Per-client handler thread
 * ====================================================================== */

static DWORD WINAPI client_handler(LPVOID param)
{
    int idx = (int)(intptr_t)param;
    client_slot_t *slot = &g_clients[idx];

    wincan_pkt_hdr_t hdr;
    uint8_t payload[MAX_FILT * sizeof(wincan_net_filter_t)];

    while (slot->active) {
        int rc = wincan_proto_recv(slot->sock, &hdr, payload, sizeof(payload));
        if (rc != WINCAN_OK) break;

        switch ((wincan_pkt_type_t)hdr.type) {

        case WINCAN_PKT_OPEN: {
            wincan_pkt_open_ack_t ack;
            int ok = 0;
            if (IS_VCAN(hdr.channel)) {
                ok = 1;  /* vcan always available */
            } else if (IS_USB(hdr.channel)) {
                int usb_idx = USB_IDX(hdr.channel);
                EnterCriticalSection(&g_bus_lock);
                ok = (g_bus[usb_idx] != NULL);
                LeaveCriticalSection(&g_bus_lock);
            }
            if (ok) {
                EnterCriticalSection(&g_clients_lock);
                slot->subscribed[hdr.channel] = 1;
                LeaveCriticalSection(&g_clients_lock);
                ack.result = WINCAN_OK;
            } else {
                ack.result = WINCAN_ERR_NOTFOUND;
            }
            wincan_proto_send(slot->sock, WINCAN_PKT_OPEN_ACK,
                              hdr.channel, &ack, sizeof(ack));
            break;
        }

        case WINCAN_PKT_TX: {
            if (hdr.payload_len != sizeof(wincan_net_frame_t)) break;
            if (hdr.channel >= NUM_CHANNELS) break;
            wincan_frame_t f;
            wincan_frame_from_net((const wincan_net_frame_t *)payload, &f);
            if (IS_VCAN(hdr.channel)) {
                vcan_loopback(hdr.channel, &f);
            } else {
                int usb_idx = USB_IDX(hdr.channel);
                EnterCriticalSection(&g_bus_lock);
                wincan_bus_t *bus = g_bus[usb_idx];
                LeaveCriticalSection(&g_bus_lock);
                if (!bus) break;
                wincan_send(bus, &f, 500);
            }
            break;
        }

        case WINCAN_PKT_CLOSE: {
            EnterCriticalSection(&g_clients_lock);
            if (hdr.channel < NUM_CHANNELS) slot->subscribed[hdr.channel] = 0;
            LeaveCriticalSection(&g_clients_lock);
            break;
        }

        case WINCAN_PKT_SET_FILTER: {
            int n = (int)(hdr.payload_len / sizeof(wincan_net_filter_t));
            if (n > MAX_FILT) n = MAX_FILT;
            EnterCriticalSection(&g_clients_lock);
            if (n > 0)
                memcpy(slot->filters, payload,
                       (size_t)n * sizeof(wincan_net_filter_t));
            slot->filter_count = n;
            LeaveCriticalSection(&g_clients_lock);
            break;
        }

        case WINCAN_PKT_GET_STATUS: {
            if (hdr.channel >= NUM_CHANNELS) break;
            wincan_net_status_t nst;
            memset(&nst, 0, sizeof(nst));
            if (IS_USB(hdr.channel)) {
                int usb_idx = USB_IDX(hdr.channel);
                EnterCriticalSection(&g_bus_lock);
                wincan_bus_t *bus = g_bus[usb_idx];
                LeaveCriticalSection(&g_bus_lock);
                if (!bus) break;
                wincan_status_t st;
                if (wincan_get_status(bus, &st) == WINCAN_OK) {
                    nst.state        = (uint32_t)st.state;
                    nst.rx_errors    = st.rx_errors;
                    nst.tx_errors    = st.tx_errors;
                    nst.rx_buffered  = st.rx_buffered;
                    nst.rx_overflows = st.rx_overflows;
                }
            }
            /* vcan: nst zero-initialised — state=0 means ACTIVE, no errors */
            wincan_proto_send(slot->sock, WINCAN_PKT_STATUS_RSP,
                              hdr.channel, &nst, sizeof(nst));
            break;
        }

        default:
            break;
        }
    }

    EnterCriticalSection(&g_clients_lock);
    closesocket(slot->sock);
    slot->sock   = INVALID_SOCKET;
    slot->active = 0;
    LeaveCriticalSection(&g_clients_lock);

    return 0;
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void wincan_server_stop(void)
{
    g_server_running = 0;
}

void wincan_server_run(int bitrate_kbps, int channels_mask, int device_index)
{
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);

    g_bitrate_kbps  = bitrate_kbps;
    g_channels_mask = channels_mask;
    g_device_index  = device_index;

    InitializeCriticalSection(&g_clients_lock);
    InitializeCriticalSection(&g_bus_lock);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_clients[i].active = 0;
        g_clients[i].sock   = INVALID_SOCKET;
    }
    memset(g_bus, 0, sizeof(g_bus));

    /* vcan0 and vcan1 are always available — no initialisation needed */
    printf("wincan_server: vcan0 and vcan1 ready\n");

    /* Open USB channel(s) — protocol channels 2 (can0) and 3 (can1) */
    int opened = 0;
    for (int usb_idx = 0; usb_idx < 2; usb_idx++) {
        if (!(channels_mask & (1 << usb_idx))) continue;
        int ch = USB_CH_BASE + usb_idx;
        g_bus[usb_idx] = open_channel(ch);
        if (g_bus[usb_idx]) {
            printf("wincan_server: can%d connected at %d kbps\n",
                   usb_idx, bitrate_kbps);
            opened++;
        } else {
            fprintf(stderr, "wincan_server: can%d not available, will retry\n",
                    usb_idx);
        }
    }
    if (opened == 0 && channels_mask)
        fprintf(stderr, "wincan_server: no USB devices found at startup, waiting...\n");

    /* Start USB RX/reconnect threads — pass protocol channel number */
    g_server_running = 1;
    HANDLE rx_threads[2] = {NULL, NULL};
    for (int usb_idx = 0; usb_idx < 2; usb_idx++) {
        if (!(channels_mask & (1 << usb_idx))) continue;
        int ch = USB_CH_BASE + usb_idx;
        rx_threads[usb_idx] = CreateThread(NULL, 0, server_rx_thread,
                                           (LPVOID)(intptr_t)ch, 0, NULL);
    }

    /* Bind listen socket */
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        fprintf(stderr, "wincan_server: socket() failed: %d\n", WSAGetLastError());
        goto cleanup;
    }
    {
        int yes = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    }
    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(WINCAN_SERVER_PORT);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
            fprintf(stderr, "wincan_server: bind failed: %d\n", WSAGetLastError());
            closesocket(ls);
            goto cleanup;
        }
    }
    listen(ls, SOMAXCONN);
    printf("wincan_server: listening on localhost:%d\n", WINCAN_SERVER_PORT);

    /* Accept loop */
    while (g_server_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ls, &fds);
        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        if (select(0, &fds, NULL, NULL, &tv) <= 0) continue;

        SOCKET cs = accept(ls, NULL, NULL);
        if (cs == INVALID_SOCKET) continue;

        DWORD snd_to = 2000;
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO,
                   (const char *)&snd_to, sizeof(snd_to));

        EnterCriticalSection(&g_clients_lock);
        int found = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].active) { found = i; break; }
        }
        if (found >= 0) {
            g_clients[found].sock         = cs;
            g_clients[found].active       = 1;
            g_clients[found].filter_count = 0;
            memset(g_clients[found].subscribed, 0,
                   sizeof(g_clients[found].subscribed));
            g_clients[found].thread =
                CreateThread(NULL, 0, client_handler,
                             (LPVOID)(intptr_t)found, 0, NULL);
        } else {
            fprintf(stderr, "wincan_server: too many clients (max %d)\n",
                    MAX_CLIENTS);
            closesocket(cs);
        }
        LeaveCriticalSection(&g_clients_lock);
    }

    closesocket(ls);

cleanup:
    g_server_running = 0;

    for (int usb_idx = 0; usb_idx < 2; usb_idx++) {
        if (!rx_threads[usb_idx]) continue;
        WaitForSingleObject(rx_threads[usb_idx], 4000);
        CloseHandle(rx_threads[usb_idx]);
    }

    EnterCriticalSection(&g_clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) {
            closesocket(g_clients[i].sock);
            g_clients[i].active = 0;
            g_clients[i].sock   = INVALID_SOCKET;
        }
        if (g_clients[i].thread) {
            WaitForSingleObject(g_clients[i].thread, 500);
            CloseHandle(g_clients[i].thread);
            g_clients[i].thread = NULL;
        }
    }
    LeaveCriticalSection(&g_clients_lock);

    for (int usb_idx = 0; usb_idx < 2; usb_idx++) {
        wincan_close(g_bus[usb_idx]);
        g_bus[usb_idx] = NULL;
    }

    DeleteCriticalSection(&g_bus_lock);
    DeleteCriticalSection(&g_clients_lock);
    WSACleanup();
}

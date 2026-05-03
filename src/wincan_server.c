#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "wincan/wincan.h"
#include "wincan/wincan_proto.h"

/* =========================================================================
 * Client registry
 * ====================================================================== */

#define MAX_CLIENTS  8
#define MAX_FILT     64

typedef struct {
    SOCKET              sock;
    uint8_t             subscribed[2];          /* [ch] = 1 when subscribed  */
    wincan_net_filter_t filters[MAX_FILT];
    int                 filter_count;
    HANDLE              thread;
    volatile int        active;
} client_slot_t;

static client_slot_t    g_clients[MAX_CLIENTS];
static CRITICAL_SECTION g_clients_lock;
static volatile int     g_server_running = 0;

/* g_bus and g_bus_lock — RX threads write, client handlers read */
static wincan_bus_t    *g_bus[2];
static CRITICAL_SECTION g_bus_lock;

/* Reconnect config stored at startup */
static int g_bitrate_kbps;
static int g_channels_mask;
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
 * Open one USB channel (shared by startup and reconnect)
 * ====================================================================== */

static wincan_bus_t *open_channel(int ch)
{
    wincan_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.device_index   = g_device_index;
    cfg.channel        = ch;
    cfg.bitrate_kbps   = g_bitrate_kbps;
    cfg.rx_buffer_size = WINCAN_DEFAULT_RX_BUFFER;
    return wincan_open(&cfg);
}

/* =========================================================================
 * Per-channel RX thread: drains USB, broadcasts to subscribed clients,
 * handles device disconnect/reconnect.
 * ====================================================================== */

static DWORD WINAPI server_rx_thread(LPVOID param)
{
    int ch = (int)(intptr_t)param;
    wincan_frame_t f;
    wincan_net_frame_t nf;
    SOCKET to_send[MAX_CLIENTS];
    int n_send;

    while (g_server_running) {

        /* If bus is absent (never opened or was disconnected), try to open */
        EnterCriticalSection(&g_bus_lock);
        wincan_bus_t *bus = g_bus[ch];
        LeaveCriticalSection(&g_bus_lock);

        if (!bus) {
            Sleep(2000);
            if (!g_server_running) break;
            /* Skip open attempt (and its error messages) if device not present */
            if (wincan_device_count() == 0) continue;
            bus = open_channel(ch);
            if (bus) {
                EnterCriticalSection(&g_bus_lock);
                g_bus[ch] = bus;
                LeaveCriticalSection(&g_bus_lock);
                printf("wincan_server: can%d connected\n", ch);
            } else {
                /* Device present but open failed (still initialising after replug) */
                Sleep(3000);
            }
            continue;
        }

        int rc = wincan_recv(bus, &f, 100);

        if (rc == WINCAN_ERR_TIMEOUT) continue;

        if (rc == WINCAN_ERR_IO) {
            printf("wincan_server: can%d disconnected\n", ch);
            EnterCriticalSection(&g_bus_lock);
            g_bus[ch] = NULL;
            LeaveCriticalSection(&g_bus_lock);
            wincan_close(bus);
            continue; /* loop will retry open after 2s */
        }

        if (rc != WINCAN_OK) break;

        wincan_frame_to_net(&f, &nf, (uint64_t)GetTickCount64() * 1000ULL);

        /* Collect recipients while holding the lock briefly */
        n_send = 0;
        EnterCriticalSection(&g_clients_lock);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].active)           continue;
            if (!g_clients[i].subscribed[ch])   continue;
            if (!client_passes_filters(&g_clients[i], &f)) continue;
            to_send[n_send++] = g_clients[i].sock;
        }
        LeaveCriticalSection(&g_clients_lock);

        /* Send outside the lock; SO_SNDTIMEO bounds each call */
        for (int i = 0; i < n_send; i++) {
            wincan_proto_send(to_send[i], WINCAN_PKT_FRAME,
                              (uint8_t)ch, &nf, sizeof(nf));
        }
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
            EnterCriticalSection(&g_bus_lock);
            int bus_ok = (hdr.channel < 2 && g_bus[hdr.channel] != NULL);
            LeaveCriticalSection(&g_bus_lock);
            if (bus_ok) {
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
            if (hdr.channel >= 2) break;
            EnterCriticalSection(&g_bus_lock);
            wincan_bus_t *bus = g_bus[hdr.channel];
            LeaveCriticalSection(&g_bus_lock);
            if (!bus) break;
            wincan_frame_t f;
            wincan_frame_from_net((const wincan_net_frame_t *)payload, &f);
            wincan_send(bus, &f, 500);
            break;
        }

        case WINCAN_PKT_CLOSE: {
            EnterCriticalSection(&g_clients_lock);
            if (hdr.channel < 2) slot->subscribed[hdr.channel] = 0;
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
            if (hdr.channel >= 2) break;
            EnterCriticalSection(&g_bus_lock);
            wincan_bus_t *bus = g_bus[hdr.channel];
            LeaveCriticalSection(&g_bus_lock);
            if (!bus) break;
            wincan_status_t st;
            wincan_net_status_t nst;
            memset(&nst, 0, sizeof(nst));
            if (wincan_get_status(bus, &st) == WINCAN_OK) {
                nst.state        = (uint32_t)st.state;
                nst.rx_errors    = st.rx_errors;
                nst.tx_errors    = st.tx_errors;
                nst.rx_buffered  = st.rx_buffered;
                nst.rx_overflows = st.rx_overflows;
            }
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

    /* Open USB channel(s) */
    int opened = 0;
    for (int ch = 0; ch < 2; ch++) {
        if (!(channels_mask & (1 << ch))) continue;
        g_bus[ch] = open_channel(ch);
        if (g_bus[ch]) {
            printf("wincan_server: can%d connected at %d kbps\n", ch, bitrate_kbps);
            opened++;
        } else {
            /* Not fatal — RX thread will retry */
            fprintf(stderr, "wincan_server: can%d not available, will retry\n", ch);
        }
    }
    if (opened == 0)
        fprintf(stderr, "wincan_server: no devices found at startup, waiting...\n");

    /* Start per-channel RX/reconnect threads */
    g_server_running = 1;
    HANDLE rx_threads[2] = {NULL, NULL};
    for (int ch = 0; ch < 2; ch++) {
        if (!(channels_mask & (1 << ch))) continue;
        rx_threads[ch] = CreateThread(NULL, 0, server_rx_thread,
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

    for (int ch = 0; ch < 2; ch++) {
        if (!rx_threads[ch]) continue;
        WaitForSingleObject(rx_threads[ch], 4000);
        CloseHandle(rx_threads[ch]);
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

    for (int ch = 0; ch < 2; ch++) {
        wincan_close(g_bus[ch]);
        g_bus[ch] = NULL;
    }

    DeleteCriticalSection(&g_bus_lock);
    DeleteCriticalSection(&g_clients_lock);
    WSACleanup();
}

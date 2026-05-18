#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "wincan/wincan.h"
#include "wincan/wincan_proto.h"
#include "wincan_internal.h"

/* =========================================================================
 * Background receive thread
 *
 * Reads WINCAN_PKT_FRAME packets from the server and pushes them into the
 * ring buffer.  Exits when net_rx_running is cleared or the socket closes.
 * ====================================================================== */

static DWORD WINAPI net_rx_thread(LPVOID param)
{
    wincan_bus_t *bus = (wincan_bus_t *)param;

    /* Large enough for any packet payload we'll receive. */
    uint8_t payload[sizeof(wincan_net_frame_t)];
    wincan_pkt_hdr_t hdr;

    while (bus->net_rx_running) {
        int rc = wincan_proto_recv(bus->net_sock, &hdr, payload, sizeof(payload));
        if (rc != WINCAN_OK)
            break;

        if (hdr.type == WINCAN_PKT_FRAME &&
            hdr.payload_len == sizeof(wincan_net_frame_t)) {
            wincan_frame_t f;
            wincan_frame_from_net((const wincan_net_frame_t *)payload, &f);
            ring_push(&bus->ring, &f);
        }
        /* WINCAN_PKT_ERROR and unknown types: ignore silently */
    }

    bus->net_rx_running = 0;
    return 0;
}

/* =========================================================================
 * wincan_net_open
 * ====================================================================== */

wincan_bus_t *wincan_net_open(uint8_t channel, uint32_t bitrate_kbps,
                              uint32_t rx_buffer_size)
{
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "wincan_net: socket() failed: %d\n", WSAGetLastError());
        return NULL;
    }

    DWORD snd_to = 2000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&snd_to, sizeof(snd_to));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(WINCAN_SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAECONNREFUSED)
            fprintf(stderr, "wincan_net: server not running\n");
        else
            fprintf(stderr, "wincan_net: cannot connect to server (error %d)\n", err);
        closesocket(sock);
        return NULL;
    }

    /* Send OPEN */
    wincan_pkt_open_t open_pkt;
    open_pkt.bitrate_kbps = bitrate_kbps;
    if (wincan_proto_send(sock, WINCAN_PKT_OPEN, channel,
                          &open_pkt, sizeof(open_pkt)) != WINCAN_OK) {
        fprintf(stderr, "wincan_net: failed to send OPEN\n");
        closesocket(sock);
        return NULL;
    }

    /* Wait for OPEN_ACK */
    wincan_pkt_hdr_t hdr;
    wincan_pkt_open_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    if (wincan_proto_recv(sock, &hdr, &ack, sizeof(ack)) != WINCAN_OK ||
        hdr.type != WINCAN_PKT_OPEN_ACK) {
        fprintf(stderr, "wincan_net: no OPEN_ACK from server\n");
        closesocket(sock);
        return NULL;
    }
    if (ack.result != WINCAN_OK) {
        fprintf(stderr, "wincan_net: server rejected open on channel %d (error %d)\n",
                (int)channel, (int)ack.result);
        closesocket(sock);
        return NULL;
    }

    /* Allocate bus */
    wincan_bus_t *bus = calloc(1, sizeof(*bus));
    if (!bus) {
        closesocket(sock);
        return NULL;
    }
    bus->backend        = BACKEND_NET;
    bus->net_sock       = sock;
    bus->channel        = (int)channel;
    bus->net_rx_running = 1;
    InitializeCriticalSection(&bus->filter_lock);

    int cap = (rx_buffer_size > 0) ? (int)rx_buffer_size : WINCAN_DEFAULT_RX_BUFFER;
    if (ring_init(&bus->ring, cap) != WINCAN_OK) {
        DeleteCriticalSection(&bus->filter_lock);
        closesocket(sock);
        free(bus);
        return NULL;
    }

    /* Send initial empty filter list (server accepts all frames for this client) */
    wincan_proto_send(sock, WINCAN_PKT_SET_FILTER, channel, NULL, 0);

    /* Start background receive thread */
    bus->net_rx_thread = CreateThread(NULL, 0, net_rx_thread, bus, 0, NULL);
    if (!bus->net_rx_thread) {
        fprintf(stderr, "wincan_net: CreateThread failed: %lu\n", GetLastError());
        bus->net_rx_running = 0;
        ring_destroy(&bus->ring);
        DeleteCriticalSection(&bus->filter_lock);
        closesocket(sock);
        free(bus);
        return NULL;
    }

    return bus;
}

/* =========================================================================
 * wincan_net_send
 * ====================================================================== */

int wincan_net_send(wincan_bus_t *bus, const wincan_frame_t *frame,
                    uint32_t timeout_ms)
{
    (void)timeout_ms; /* SO_SNDTIMEO set at connect time */
    wincan_net_frame_t nf;
    wincan_frame_to_net(frame, &nf, 0);
    return wincan_proto_send(bus->net_sock, WINCAN_PKT_TX,
                             (uint8_t)bus->channel, &nf, sizeof(nf));
}

/* =========================================================================
 * wincan_net_set_filters
 * ====================================================================== */

int wincan_net_set_filters(wincan_bus_t *bus,
                           const wincan_filter_t *filters, int count)
{
    /* Update local copy */
    wincan_filter_t *copy = NULL;
    if (filters && count > 0) {
        copy = malloc(sizeof(wincan_filter_t) * count);
        if (!copy) return WINCAN_ERR_NOMEM;
        memcpy(copy, filters, sizeof(wincan_filter_t) * count);
    }
    EnterCriticalSection(&bus->filter_lock);
    free(bus->filters);
    bus->filters      = copy;
    bus->filter_count = copy ? count : 0;
    LeaveCriticalSection(&bus->filter_lock);

    /* Build wire filter array and send to server */
    if (count > 0 && filters) {
        wincan_net_filter_t *wire = malloc(sizeof(wincan_net_filter_t) * count);
        if (!wire) return WINCAN_ERR_NOMEM;
        for (int i = 0; i < count; i++) {
            wire[i].id   = filters[i].id;
            wire[i].mask = filters[i].mask;
            wire[i].ext  = filters[i].ext;
        }
        int rc = wincan_proto_send(bus->net_sock, WINCAN_PKT_SET_FILTER,
                                   (uint8_t)bus->channel,
                                   wire, (uint32_t)(sizeof(wincan_net_filter_t) * count));
        free(wire);
        return rc;
    }

    /* Empty filter list = accept all */
    return wincan_proto_send(bus->net_sock, WINCAN_PKT_SET_FILTER,
                             (uint8_t)bus->channel, NULL, 0);
}

/* =========================================================================
 * wincan_net_close
 * ====================================================================== */

void wincan_net_close(wincan_bus_t *bus)
{
    if (!bus) return;

    /* Signal recv thread to stop and unblock it by closing the socket */
    bus->net_rx_running = 0;

    /* Best-effort CLOSE notification to server */
    wincan_proto_send(bus->net_sock, WINCAN_PKT_CLOSE,
                      (uint8_t)bus->channel, NULL, 0);

    closesocket(bus->net_sock);
    bus->net_sock = INVALID_SOCKET;

    if (bus->net_rx_thread) {
        WaitForSingleObject(bus->net_rx_thread, 2000);
        CloseHandle(bus->net_rx_thread);
        bus->net_rx_thread = NULL;
    }

    ring_destroy(&bus->ring);

    EnterCriticalSection(&bus->filter_lock);
    free(bus->filters);
    bus->filters = NULL;
    LeaveCriticalSection(&bus->filter_lock);
    DeleteCriticalSection(&bus->filter_lock);

    free(bus);
}

#endif /* _WIN32 */

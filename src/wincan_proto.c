#include <string.h>
#include <winsock2.h>
#include "wincan/wincan.h"
#include "wincan/wincan_proto.h"

/* -------------------------------------------------------------------------
 * Internal: send exactly `len` bytes, retrying on short writes.
 * Returns 0 on success, -1 on error or connection close.
 * ---------------------------------------------------------------------- */
static int send_all(SOCKET sock, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int remaining = len;

    while (remaining > 0) {
        int n = send(sock, p, remaining, 0);
        if (n == SOCKET_ERROR || n == 0)
            return -1;
        p         += n;
        remaining -= n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Internal: receive exactly `len` bytes, retrying on short reads.
 * Returns 0 on success, -1 on error or connection close.
 * ---------------------------------------------------------------------- */
static int recv_all(SOCKET sock, void *buf, int len)
{
    char *p = (char *)buf;
    int remaining = len;

    while (remaining > 0) {
        int n = recv(sock, p, remaining, 0);
        if (n == SOCKET_ERROR || n == 0)
            return -1;
        p         += n;
        remaining -= n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * wincan_proto_send
 * ---------------------------------------------------------------------- */
int wincan_proto_send(SOCKET sock, uint8_t type, uint8_t channel,
                      const void *payload, uint32_t payload_len)
{
    wincan_pkt_hdr_t hdr;
    hdr.version     = WINCAN_PROTO_VERSION;
    hdr.type        = type;
    hdr.channel     = channel;
    hdr.reserved    = 0;
    hdr.payload_len = payload_len;

    if (send_all(sock, &hdr, (int)sizeof(hdr)) != 0)
        return WINCAN_ERR_IO;

    if (payload_len > 0 && payload != NULL) {
        if (send_all(sock, payload, (int)payload_len) != 0)
            return WINCAN_ERR_IO;
    }

    return WINCAN_OK;
}

/* -------------------------------------------------------------------------
 * wincan_proto_recv
 * ---------------------------------------------------------------------- */
int wincan_proto_recv(SOCKET sock, wincan_pkt_hdr_t *hdr_out,
                      void *payload_buf, uint32_t buf_size)
{
    if (recv_all(sock, hdr_out, (int)sizeof(wincan_pkt_hdr_t)) != 0)
        return WINCAN_ERR_IO;

    if (hdr_out->payload_len == 0)
        return WINCAN_OK;

    if (hdr_out->payload_len > buf_size)
        return WINCAN_ERR_IO;

    if (recv_all(sock, payload_buf, (int)hdr_out->payload_len) != 0)
        return WINCAN_ERR_IO;

    return WINCAN_OK;
}

/* -------------------------------------------------------------------------
 * Frame serialization
 * ---------------------------------------------------------------------- */
void wincan_frame_to_net(const wincan_frame_t *src, wincan_net_frame_t *dst,
                         uint64_t timestamp_us)
{
    dst->id   = src->id;
    dst->dlc  = src->dlc;
    dst->flags = 0;
    if (src->rtr) dst->flags |= WINCAN_FRAME_RTR;
    if (src->ext) dst->flags |= WINCAN_FRAME_EFF;
    memcpy(dst->data, src->data, 8);
    dst->timestamp_us = timestamp_us;
}

void wincan_frame_from_net(const wincan_net_frame_t *src, wincan_frame_t *dst)
{
    dst->id  = src->id;
    dst->dlc = src->dlc;
    dst->rtr = (src->flags & WINCAN_FRAME_RTR) ? 1 : 0;
    dst->ext = (src->flags & WINCAN_FRAME_EFF) ? 1 : 0;
    memcpy(dst->data, src->data, 8);
}

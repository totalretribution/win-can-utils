#ifndef WINCAN_PROTO_H
#define WINCAN_PROTO_H

#ifdef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <winsock2.h>
#include "wincan/wincan.h"

#define WINCAN_PROTO_VERSION  1
#define WINCAN_SERVER_PORT    29526

/* -------------------------------------------------------------------------
 * Packet types
 * ---------------------------------------------------------------------- */
typedef enum {
    WINCAN_PKT_FRAME      = 0x01,  /* server→client: received CAN frame     */
    WINCAN_PKT_TX         = 0x02,  /* client→server: transmit this frame    */
    WINCAN_PKT_OPEN       = 0x03,  /* client→server: subscribe to a channel */
    WINCAN_PKT_OPEN_ACK   = 0x04,  /* server→client: result of open         */
    WINCAN_PKT_CLOSE      = 0x05,  /* client→server: unsubscribe            */
    WINCAN_PKT_SET_FILTER = 0x06,  /* client→server: update RX filters      */
    WINCAN_PKT_GET_STATUS = 0x07,  /* client→server: request bus status     */
    WINCAN_PKT_STATUS_RSP = 0x08,  /* server→client: bus status response    */
    WINCAN_PKT_ERROR      = 0xFF,  /* server→client: error notification     */
} wincan_pkt_type_t;

/* -------------------------------------------------------------------------
 * Wire structures — all fields little-endian (native on x86 Windows)
 * ---------------------------------------------------------------------- */
#pragma pack(push, 1)

typedef struct {
    uint8_t  version;       /* WINCAN_PROTO_VERSION                          */
    uint8_t  type;          /* wincan_pkt_type_t                             */
    uint8_t  channel;       /* 0=vcan0, 1=vcan1, 2=can0, 3=can1              */
    uint8_t  reserved;
    uint32_t payload_len;   /* bytes following this header                   */
} wincan_pkt_hdr_t;

#define WINCAN_FRAME_RTR  0x01
#define WINCAN_FRAME_EFF  0x02

typedef struct {
    uint32_t id;            /* CAN ID (11 or 29-bit)                         */
    uint8_t  dlc;           /* 0–8                                           */
    uint8_t  flags;         /* WINCAN_FRAME_RTR | WINCAN_FRAME_EFF           */
    uint8_t  data[8];
    uint64_t timestamp_us;  /* server USB-arrival time; 0 if N/A             */
} wincan_net_frame_t;

/* WINCAN_PKT_OPEN payload */
typedef struct {
    uint32_t bitrate_kbps;
} wincan_pkt_open_t;

/* WINCAN_PKT_OPEN_ACK payload */
typedef struct {
    int32_t  result;        /* WINCAN_OK or error code                       */
} wincan_pkt_open_ack_t;

/* WINCAN_PKT_SET_FILTER payload: array of wincan_net_filter_t entries      */
typedef struct {
    uint32_t id;
    uint32_t mask;
    int32_t  ext;           /* 1=EFF only, 0=SFF only, -1=both               */
} wincan_net_filter_t;

/* WINCAN_PKT_STATUS_RSP payload */
typedef struct {
    uint32_t state;         /* wincan_state_t                                */
    uint32_t rx_errors;
    uint32_t tx_errors;
    uint32_t rx_buffered;
    uint32_t rx_overflows;
} wincan_net_status_t;

/* WINCAN_PKT_ERROR payload */
typedef struct {
    int32_t  code;          /* WINCAN_ERR_* code                             */
} wincan_pkt_error_t;

#pragma pack(pop)

/* -------------------------------------------------------------------------
 * Transport helpers
 *
 * wincan_proto_send — builds and sends header + payload atomically.
 *   payload may be NULL when payload_len == 0.
 *   Returns WINCAN_OK or WINCAN_ERR_IO.
 *
 * wincan_proto_recv — reads one complete packet.
 *   Fills *hdr_out with the parsed header.
 *   Reads up to buf_size payload bytes into payload_buf (may be NULL when
 *   no payload is expected).  Returns WINCAN_ERR_IO if payload_len exceeds
 *   buf_size or the socket is closed/errored.
 *   Returns WINCAN_OK on success.
 * ---------------------------------------------------------------------- */
int wincan_proto_send(SOCKET sock, uint8_t type, uint8_t channel,
                      const void *payload, uint32_t payload_len);

int wincan_proto_recv(SOCKET sock, wincan_pkt_hdr_t *hdr_out,
                      void *payload_buf, uint32_t buf_size);

/* -------------------------------------------------------------------------
 * Frame serialization helpers
 * ---------------------------------------------------------------------- */
void wincan_frame_to_net(const wincan_frame_t *src, wincan_net_frame_t *dst,
                         uint64_t timestamp_us);

void wincan_frame_from_net(const wincan_net_frame_t *src, wincan_frame_t *dst);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* WINCAN_PROTO_H */

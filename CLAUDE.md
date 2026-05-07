# WinCAN Server — Architecture

## Overview

A TCP server (`wincan_server`) that owns the WinUSB CAN device and lets multiple
clients connect over localhost using the existing `wincan_bus_t` API transparently.
Clients pass `--server` flag to tools to use the net backend instead of USB direct.

```
┌─────────────┐     TCP localhost:29526     ┌──────────────────────────┐
│  Client App │ ◄─────────────────────────► │  wincan_server            │
│  (candump,  │                             │  - owns WinUSB device     │
│   cansend,  │                             │  - rx thread → broadcast  │
│   cangen)   │                             │  - rx from clients → tx   │
└─────────────┘                             └──────────────────────────┘
      ▲                                                  ▲
      │ wincan_config_ex_t { use_server=1 }              │ wincan_bus_t (existing)
```

## Wire Protocol

Fixed port: **29526** (no configuration). Bind/connect to `127.0.0.1:29526` only.

### Packet Header (8 bytes, little-endian)

```c
// include/wincan/wincan_proto.h

#define WINCAN_PROTO_VERSION  1
#define WINCAN_SERVER_PORT    29526

typedef enum {
    WINCAN_PKT_FRAME      = 0x01,  // server→client: received CAN frame
    WINCAN_PKT_TX         = 0x02,  // client→server: transmit this frame
    WINCAN_PKT_OPEN       = 0x03,  // client→server: subscribe to a channel
    WINCAN_PKT_OPEN_ACK   = 0x04,  // server→client: result of open
    WINCAN_PKT_CLOSE      = 0x05,  // client→server: unsubscribe
    WINCAN_PKT_SET_FILTER = 0x06,  // client→server: update RX filters
    WINCAN_PKT_GET_STATUS = 0x07,  // client→server: request bus status
    WINCAN_PKT_STATUS_RSP = 0x08,  // server→client: bus status response
    WINCAN_PKT_ERROR      = 0xFF,  // server→client: error notification
} wincan_pkt_type_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;       // WINCAN_PROTO_VERSION
    uint8_t  type;          // wincan_pkt_type_t
    uint8_t  channel;       // 0=vcan0, 1=vcan1, 2=can0, 3=can1
    uint8_t  reserved;
    uint32_t payload_len;   // bytes following this header
} wincan_pkt_hdr_t;
```

### Frame Payload (22 bytes)

Used for both `WINCAN_PKT_FRAME` (server→client) and `WINCAN_PKT_TX` (client→server).
For TX frames, client sets `timestamp_us = 0`; server ignores it.

```c
typedef struct __attribute__((packed)) {
    uint32_t id;            // CAN ID (11 or 29-bit)
    uint8_t  dlc;           // 0–8
    uint8_t  flags;         // WINCAN_FRAME_RTR | WINCAN_FRAME_EFF
    uint8_t  data[8];
    uint64_t timestamp_us;  // server USB-arrival time, 0 if N/A
} wincan_net_frame_t;

#define WINCAN_FRAME_RTR  0x01
#define WINCAN_FRAME_EFF  0x02
```

All multi-byte fields are little-endian (native on Windows/x86, no byte-swapping needed).

## Client API

```c
// include/wincan/wincan_client.h

typedef struct {
    uint8_t  channel;
    uint32_t bitrate_kbps;
    uint32_t rx_buffer_size;
    int      use_server;    // 0 = USB direct, 1 = connect to 127.0.0.1:29526
} wincan_config_ex_t;

wincan_bus_t *wincan_open_ex(const wincan_config_ex_t *cfg);
// wincan_send / wincan_recv / wincan_close / wincan_set_filters work unchanged
```

### Internal backend dispatch (src/wincan_internal.h)

```c
typedef enum { BACKEND_USB, BACKEND_NET } wincan_backend_t;

struct wincan_bus {
    wincan_backend_t backend;
    union {
        struct { /* existing USB fields */ } usb;
        struct {
            SOCKET  sock;
            uint8_t channel;
        } net;
    };
};
```

`wincan_send()` and `wincan_recv()` gain a one-line backend dispatch at the top.
`wincan_recv()` on net backend uses `select()` + `SO_RCVTIMEO` to match `timeout_ms` semantics.

## Server Design

### Thread Model

| Thread         | Count               | Job                                                        |
|----------------|---------------------|------------------------------------------------------------|
| Main           | 1                   | Open device, spawn RX threads, run accept loop             |
| RX             | 1 per USB channel   | `wincan_recv()` loop → serialize → broadcast to clients    |
| Client handler | 1 per connection    | Read packets → USB send or vcan loopback or control        |

vcan channels have no RX thread — the client handler performs the loopback inline.

### Client Registry

```c
#define MAX_CLIENTS 8

typedef struct {
    SOCKET          sock;
    uint8_t         subscribed[4];      // [channel] = 1 if subscribed (0-3)
    wincan_filter_t filters[MAX_FILT];
    int             filter_count;
    HANDLE          thread;
    volatile int    active;
} wincan_client_slot_t;

wincan_client_slot_t g_clients[MAX_CLIENTS];
CRITICAL_SECTION     g_clients_lock;
```

### Broadcast rules
- RX thread builds one packet, iterates `g_clients`, sends to every active+subscribed slot
- Per-client filters applied server-side before sending (skip non-matching frames)
- `SO_SNDTIMEO` enforced on each client socket so a slow/dead client cannot stall the loop

### Server CLI

```
wincan_server [-b bitrate] [-c can0|can1|both|none] [-d device_index]
```

`-c` controls USB channels only. No host or port options — always binds `127.0.0.1:29526`.

### vcan channels

`vcan0` (channel 2) and `vcan1` (channel 3) are always enabled — no flag needed.
Any frame transmitted on a vcan channel is broadcast back to all subscribers of that
channel (including the sender), matching SocketCAN `vcan` behaviour. No USB device
required.

Tools accept `vcan0`/`vcan1` as channel names and automatically use server mode:
```
candump vcan0
cangen  vcan0 -g 200
cansend vcan0 123#DEADBEEF
```

## Tool Changes

Each tool gains a single boolean flag. No argument:

```
candump can0 --server
cansend can0 123#DEADBEEF --server
cangen can0 --server
```

`tools/common.c` scans `argv` for `"--server"` and sets `cfg.use_server = 1`.
Tools call `wincan_open_ex()` instead of `wincan_open()` when the flag is present.
All other tool logic is unchanged.

## File Layout

```
src/
  wincan.c              existing — USB backend, gains backend dispatch in send/recv
  wincan_net.c          new — net backend: connect, send, recv, close
  wincan_proto.c        new — send_pkt() / recv_pkt() helpers, serialize/deserialize
  wincan_server.c       new — accept loop, RX threads, client handlers

include/wincan/
  wincan.h              unchanged
  wincan_client.h       new — wincan_config_ex_t, wincan_open_ex()
  wincan_proto.h        new — wire types shared by server and client

tools/
  common.c / common.h   new — --server flag detection, shared across tools
  candump.c             updated
  cansend.c             updated
  cangen.c              updated
  wincan_server.c       new — main() entry point → wincan_server_run()
```

## CMake

```cmake
option(WINCAN_BUILD_SERVER "Build wincan_server daemon" ON)
option(WINCAN_BUILD_NET    "Build network client backend" ON)

if(WINCAN_BUILD_NET)
    target_sources(wincanlib PRIVATE src/wincan_net.c src/wincan_proto.c)
    target_link_libraries(wincanlib PRIVATE ws2_32)
endif()

if(WINCAN_BUILD_SERVER)
    add_executable(wincan_server tools/wincan_server.c src/wincan_server.c src/wincan_proto.c)
    target_link_libraries(wincan_server PRIVATE wincanlib ws2_32)
endif()
```

USB-only builds (`-DWINCAN_BUILD_NET=OFF`) have zero new dependencies.


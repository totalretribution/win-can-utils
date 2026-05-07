> This project was built with the assistance of [Claude](https://claude.ai) (Anthropic AI). The code has been reviewed and tested by the author.

# wincan

Windows C library and CLI tools for the [candleLight](https://github.com/marckleinebudde/candleLight_fw/tree/multichannel) USB CAN adapter (gs_usb protocol).

No driver installation required — the firmware implements WCID USB descriptors so Windows automatically uses WinUSB.

---

## Server

`wincan_server` owns the USB device and lets multiple clients connect over TCP on `127.0.0.1:29526`. Tools default to server mode; pass `--usb` to access the device directly (single process only).

```sh
wincan_server                    # USB can0 + can1 at 250 kbps, plus vcan0/vcan1
wincan_server -b 500             # 500 kbps
wincan_server -c can0            # USB can0 only
wincan_server -c none            # virtual channels only, no USB required
```

**Virtual channels** (`vcan0`, `vcan1`) are always available regardless of USB hardware. Frames transmitted on a vcan channel are looped back to all subscribers — useful for testing without hardware.

```sh
# Terminal 1
candump vcan0

# Terminal 2
cangen vcan0 -g 100
```

---

## Library

`wincan` is a static C library (`libwincanlib.a`) for sending and receiving CAN frames.

```c
#include <wincan/wincan.h>

wincan_config_t cfg = {
    .channel      = 0,
    .bitrate_kbps = 500,
};
wincan_bus_t *bus = wincan_open(&cfg);

wincan_frame_t tx = { .id = 0x123, .dlc = 8,
                      .data = {1,2,3,4,5,6,7,8} };
wincan_send(bus, &tx, 1000);

wincan_frame_t rx;
if (wincan_recv(bus, &rx, 2000) == WINCAN_OK)
    printf("%03X [%u]\n", rx.id, rx.dlc);

wincan_close(bus);
```

To connect via the server instead of USB directly:

```c
#include <wincan/wincan_client.h>

wincan_config_ex_t cfg = {
    .channel      = 2,          /* 0=vcan0, 1=vcan1, 2=can0, 3=can1 */
    .bitrate_kbps = 250,
    .use_server   = 1,
};
wincan_bus_t *bus = wincan_open_ex(&cfg);
```

### Use in another CMake project

```cmake
include(FetchContent)
FetchContent_Declare(wincan
    GIT_REPOSITORY https://github.com/totalretribution/wincan.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(wincan)

target_link_libraries(myapp PRIVATE wincan::wincan)
```

### API summary

| Function | Description |
|----------|-------------|
| `wincan_device_count()` | Number of adapters connected |
| `wincan_open(cfg)` | Open a USB channel directly |
| `wincan_open_ex(cfg)` | Open USB or server channel |
| `wincan_send(bus, frame, timeout_ms)` | Transmit a frame |
| `wincan_recv(bus, frame, timeout_ms)` | Receive a frame |
| `wincan_set_filters(bus, filters, n)` | Set receive filters |
| `wincan_get_status(bus, status)` | Read error counters and bus state |
| `wincan_close(bus)` | Reset channel and free resources |
| `wincan_strerror(err)` | Human-readable error string |

Pass `timeout_ms = 0` to `wincan_recv` for non-blocking, `UINT32_MAX` to block indefinitely.

Enable a background receive ring buffer by setting `cfg.rx_buffer_size = WINCAN_DEFAULT_RX_BUFFER`.

### Channel numbering

| Channel | Name  | Available |
|---------|-------|-----------|
| 0       | vcan0 | always (virtual loopback) |
| 1       | vcan1 | always (virtual loopback) |
| 2       | can0  | USB device required |
| 3       | can1  | USB device required |

---

## CLI Tools

| Executable | Description |
|------------|-------------|
| `wincan_server` | Server daemon — owns the USB device, serves multiple clients |
| `candump` | Receive and print CAN frames |
| `cangen` | Transmit random CAN frames |
| `cansend` | Transmit a single specific CAN frame |

All tools default to server mode. Pass `--usb` to bypass the server and access the USB device directly (single process only). `vcan` channels always require server mode.

---

## Requirements

- Windows 10 or later
- [MSYS2](https://www.msys2.org/) with the UCRT64 toolchain:
  ```
  pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-make
  ```

---

## Build

```sh
# Library only
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Library + CLI tools
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DWINCAN_BUILD_TOOLS=ON
cmake --build build

# Library + examples
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DWINCAN_BUILD_EXAMPLES=ON
cmake --build build
```

Executables are written to `build/`.

---

## Usage

### wincan_server

```
wincan_server [-b bitrate] [-c can0|can1|both|none] [-d index]
```

| Option | Default | Description |
|--------|---------|-------------|
| `-b bitrate` | `250` | Bitrate in kbps for USB channels |
| `-c channel` | `both` | USB channels to open: `can0`, `can1`, `both`, or `none` |
| `-d index` | `0` | Device index when multiple adapters are connected |

`vcan0` and `vcan1` are always enabled. `-c none` runs the server with virtual channels only.

---

### candump

Receive and print frames.

```
candump [channel...] [-b bitrate] [-d index] [--usb]
```

Channels: `vcan0`, `vcan1`, `can0`, `can1`, `both` (default: `both` = can0+can1)

```sh
candump                    # listen on can0 and can1
candump can0 -b 500        # listen on can0 at 500 kbps
candump vcan0              # listen on virtual channel
candump vcan0 can0         # listen on both vcan0 and can0
```

Output format:
```
can0   123   [8]  01 02 03 04 05 06 07 08
can1   18DAF110#  [8]  DE AD BE EF 00 00 00 00
vcan0  456   [4]  AA BB CC DD
```

---

### cangen

Transmit random CAN frames continuously.

```
cangen [channel] [-n count] [-g gap_ms] [-b bitrate] [-d index] [-v] [--usb]
```

| Option | Default | Description |
|--------|---------|-------------|
| `channel` | `can0` | `vcan0`, `vcan1`, `can0`, or `can1` |
| `-n count` | infinite | Number of frames to send |
| `-g gap_ms` | `100` | Delay between frames (ms) |
| `-b bitrate` | `250` | Bitrate in kbps |
| `-v` | off | Print each frame as sent |

```sh
cangen can0                       # random frames on can0 every 100 ms
cangen vcan0 -g 50 -v             # random frames on vcan0 every 50 ms, verbose
cangen can1 -n 50 -g 200 -b 500   # 50 frames on can1 at 500 kbps
```

---

### cansend

Transmit a single CAN frame.

```
cansend <channel> <frame> [-b bitrate] [-d index] [--usb]
```

Channel: `vcan0`, `vcan1`, `can0`, or `can1`

Frame format mirrors SocketCAN:

| Format | Description |
|--------|-------------|
| `123#DEADBEEF` | SFF (11-bit ID), 4 data bytes |
| `18DAF110#1122334455667788` | EFF (29-bit ID, auto-detected when ID > 0x7FF) |
| `123#DE.AD.BE.EF` | Dots between bytes are optional |
| `123#` | Empty frame (DLC=0) |
| `123#R` | RTR frame |
| `123#R8` | RTR frame with explicit DLC |

```sh
cansend can0 123#DEADBEEF
cansend vcan0 123#DEADBEEF
cansend can1 18DAF110#1122334455667788
cansend can0 123#R
```

---

## Bitrate

All tools accept `-b <kbps>`. Supported values: `125`, `250` (default), `500`, `1000`.

Timing presets are calculated for STM32-based candleLight hardware at a 48 MHz CAN peripheral clock (16 TQ/bit, varying `brp`). Custom timing can be set via `wincan_config_t.timing` when `bitrate_kbps = 0`.

---

## Firmware

Build and flash the [multichannel branch](https://github.com/marckleinebudde/candleLight_fw/tree/multichannel) of candleLight_fw. The firmware supports two independent CAN channels (`can0`, `can1`) over a single USB connection.

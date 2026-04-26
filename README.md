> This project was built with the assistance of [Claude](https://claude.ai) (Anthropic AI). The code has been reviewed and tested by the author.

# wincan

Windows C library and CLI tools for the [candleLight](https://github.com/marckleinebudde/candleLight_fw/tree/multichannel) USB CAN adapter (gs_usb protocol).

No driver installation required — the firmware implements WCID USB descriptors so Windows automatically uses WinUSB.

---

## Library

`wincan` is a static C library (`libwincanlib.a`) for sending and receiving CAN frames from candleLight adapters on Windows.

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

Then include with `#include <wincan/wincan.h>`.

### API summary

| Function | Description |
|----------|-------------|
| `wincan_device_count()` | Number of adapters connected |
| `wincan_open(cfg)` | Open a channel, returns a bus handle |
| `wincan_send(bus, frame, timeout_ms)` | Transmit a frame |
| `wincan_recv(bus, frame, timeout_ms)` | Receive a frame |
| `wincan_set_filters(bus, filters, n)` | Set receive filters |
| `wincan_get_status(bus, status)` | Read error counters and bus state |
| `wincan_close(bus)` | Reset channel and free resources |
| `wincan_strerror(err)` | Human-readable error string |

Pass `timeout_ms = 0` to `wincan_recv` for non-blocking, `UINT32_MAX` to block indefinitely.

Enable a background receive ring buffer by setting `cfg.rx_buffer_size = WINCAN_DEFAULT_RX_BUFFER`.
When enabled, a thread fills the ring continuously so `wincan_recv` pops frames without blocking on USB.

---

## CLI Tools

| Executable | Description |
|------------|-------------|
| `candump` | Receive and print CAN frames |
| `cangen` | Transmit random CAN frames |
| `cansend` | Transmit a single specific CAN frame |
| `canboth` | Dump and gen simultaneously on independent channels |

All tools accept `-d <index>` to select a specific adapter when multiple are connected.

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

### candump

Receive and print frames from one or both channels.

```
candump [can0 | can1 | both] [-b bitrate] [-d index]
```

Default: `both`, 250 kbps

```sh
candump both           # listen on can0 and can1 at 250 kbps
candump can0 -b 500    # listen on can0 at 500 kbps
```

Output format:
```
can0  123   [8]  01 02 03 04 05 06 07 08
can1  18DAF110#  [8]  DE AD BE EF 00 00 00 00
```

---

### cangen

Transmit random CAN frames continuously.

```
cangen [can0 | can1] [-n count] [-g gap_ms] [-b bitrate] [-d index]
```

| Option | Default | Description |
|--------|---------|-------------|
| `can0\|can1` | `can0` | Channel to transmit on |
| `-n count` | infinite | Number of frames to send |
| `-g gap_ms` | `100` | Delay between frames (ms) |
| `-b bitrate` | `250` | Bitrate in kbps |

```sh
cangen can0                       # send random frames on can0 every 100 ms
cangen can1 -n 50 -g 200 -b 500   # send 50 frames on can1 at 500 kbps
```

---

### cansend

Transmit a single CAN frame.

```
cansend <can0 | can1> <frame> [-b bitrate] [-d index]
```

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
cansend can1 18DAF110#1122334455667788
cansend can0 123#R
```

---

### canboth

Run dump and gen simultaneously on independent channels.

```
canboth dump [can0 | can1 | both] [-b bitrate] [-d index]
canboth gen  [can0 | can1] [-n count] [-g gap_ms] [-b bitrate] [-d index]
canboth both --dump [can0 | can1 | both] --gen [can0 | can1] [-n count] [-g gap_ms] [-b bitrate] [-d index]
```

```sh
canboth dump both                          # dump only
canboth gen can1 -g 50                     # gen only on can1
canboth both --dump can0 --gen can1        # dump can0 while generating on can1
canboth both --dump both --gen can0 -n 100 # dump both, gen 100 frames on can0
```

> **Note:** `candump` and `cangen` cannot run simultaneously as separate processes because
> WinUSB only allows one process to hold the device open at a time. Use `canboth` when you
> need simultaneous dump and gen.

---

## Bitrate

All tools accept `-b <kbps>`. Supported values: `125`, `250` (default), `500`, `1000`.

Timing presets are calculated for STM32-based candleLight hardware at a 48 MHz CAN peripheral clock (16 TQ/bit, varying `brp`). Custom timing can be set via `wincan_config_t.timing` when `bitrate_kbps = 0`.

---

## Firmware

Build and flash the [multichannel branch](https://github.com/marckleinebudde/candleLight_fw/tree/multichannel) of candleLight_fw. The firmware supports two independent CAN channels (`can0`, `can1`) over a single USB connection.

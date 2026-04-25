# wincan

Windows CAN bus tools for the [candleLight](https://github.com/marckleinebudde/candleLight_fw/tree/multichannel) USB CAN adapter.
Mimics the Linux SocketCAN `candump`, `cangen`, and `cansend` utilities.

No driver installation required — the firmware implements WCID USB descriptors so Windows
automatically uses WinUSB.

---

## Tools

| Executable | Description |
|------------|-------------|
| `candump.exe` | Receive and print CAN frames |
| `cangen.exe` | Transmit random CAN frames |
| `cansend.exe` | Transmit a single specific CAN frame |
| `wincan.exe` | Combined tool — dump and gen simultaneously |

---

## Requirements

- Windows 10 or later
- [MSYS2](https://www.msys2.org/) with the UCRT64 toolchain:
  ```
  pacman -S mingw-w64-ucrt-x86_64-gcc make
  ```

---

## Build

```sh
make
```

---

## Usage

### candump

Receive and print frames from one or both channels.

```
candump [can0 | can1 | both] [-b bitrate]
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
cangen [can0 | can1] [-n count] [-g gap_ms] [-b bitrate]
```

| Option | Default | Description |
|--------|---------|-------------|
| `can0\|can1` | `can0` | Channel to transmit on |
| `-n count` | infinite | Number of frames to send |
| `-g gap_ms` | `100` | Delay between frames (ms) |
| `-b bitrate` | `250` | Bitrate in kbps |

```sh
cangen can0                       # send random frames on can0 every 100 ms at 250 kbps
cangen can1 -n 50 -g 200 -b 500   # send 50 frames on can1 at 500 kbps
```

---

### cansend

Transmit a single CAN frame.

```
cansend <can0 | can1> <frame> [-b bitrate]
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

### wincan (combined)

Run dump and gen simultaneously on independent channels using a single device handle.

```
wincan dump [can0 | can1 | both] [-b bitrate]
wincan gen  [can0 | can1] [-n count] [-g gap_ms] [-b bitrate]
wincan both --dump [can0 | can1 | both] --gen [can0 | can1] [-n count] [-g gap_ms] [-b bitrate]
```

```sh
wincan dump both                             # dump only
wincan gen can1 -g 50                        # gen only on can1
wincan both --dump can0 --gen can1           # dump can0 while generating on can1
wincan both --dump both --gen can0 -n 100    # dump both, gen 100 frames on can0
```

> **Note:** `candump.exe` and `cangen.exe` cannot run at the same time because WinUSB
> only allows one process to hold the device open. Use `wincan both` when you need
> simultaneous dump and gen.

---

## Bitrate

All tools accept a `-b <kbps>` flag. Supported values:

| Flag | Rate |
|------|------|
| `-b 125` | 125 kbps |
| `-b 250` | 250 kbps (default) |
| `-b 500` | 500 kbps |
| `-b 1000` | 1 Mbps |

```sh
candump both -b 500
cangen can0 -b 1000 -g 50
cansend can1 123#DEADBEEF -b 500
wincan both --dump can0 --gen can1 -b 500
```

Timing presets are calculated for STM32-based candleLight hardware at a 48 MHz CAN
peripheral clock (16 TQ/bit, varying `brp`).

---

## Firmware

Build and flash the [multichannel branch](https://github.com/marckleinebudde/candleLight_fw/tree/multichannel)
of candleLight_fw. The firmware supports two independent CAN channels (`can0`, `can1`) over a
single USB connection.

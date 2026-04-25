#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include "wincan.h"
#include "gs_usb.h"

/* Device interface GUID used by WinUSB when installed via WCID.
   candleLight firmware advertises "WINUSB" as compatible ID so Windows
   assigns this GUID automatically. */
static const GUID WINUSB_GUID =
    { 0xDEE824EF, 0x729B, 0x4A0E,
      { 0x9C, 0x14, 0xB7, 0x11, 0x7D, 0x33, 0xA8, 0x17 } };

/* Return the device path of the Nth (0-based) candleLight device found.
   Returns NULL if fewer than (device_index+1) devices are present. */
static char *find_device_path(int device_index)
{
    HDEVINFO dev_info;
    SP_DEVICE_INTERFACE_DATA iface_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA detail;
    DWORD required;
    char *path = NULL;
    int found = 0;
    int idx;

    dev_info = SetupDiGetClassDevs(&WINUSB_GUID, NULL, NULL,
                                   DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE)
        return NULL;

    iface_data.cbSize = sizeof(iface_data);
    for (idx = 0; ; idx++) {
        if (!SetupDiEnumDeviceInterfaces(dev_info, NULL, &WINUSB_GUID,
                                         idx, &iface_data))
            break;

        SetupDiGetDeviceInterfaceDetail(dev_info, &iface_data,
                                        NULL, 0, &required, NULL);
        detail = malloc(required);
        if (!detail)
            break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (!SetupDiGetDeviceInterfaceDetail(dev_info, &iface_data,
                                              detail, required, NULL, NULL)) {
            free(detail);
            continue;
        }

        if (strstr(detail->DevicePath, "vid_1d50") &&
            strstr(detail->DevicePath, "pid_606f")) {
            if (found == device_index) {
                path = _strdup(detail->DevicePath);
                free(detail);
                break;
            }
            found++;
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return path;
}

/* Return the number of candleLight devices currently connected. */
int wincan_device_count(void)
{
    HDEVINFO dev_info;
    SP_DEVICE_INTERFACE_DATA iface_data;
    PSP_DEVICE_INTERFACE_DETAIL_DATA detail;
    DWORD required;
    int count = 0;
    int idx;

    dev_info = SetupDiGetClassDevs(&WINUSB_GUID, NULL, NULL,
                                   DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) return 0;

    iface_data.cbSize = sizeof(iface_data);
    for (idx = 0; ; idx++) {
        if (!SetupDiEnumDeviceInterfaces(dev_info, NULL, &WINUSB_GUID,
                                         idx, &iface_data))
            break;
        SetupDiGetDeviceInterfaceDetail(dev_info, &iface_data,
                                        NULL, 0, &required, NULL);
        detail = malloc(required);
        if (!detail) break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        if (SetupDiGetDeviceInterfaceDetail(dev_info, &iface_data,
                                             detail, required, NULL, NULL)) {
            if (strstr(detail->DevicePath, "vid_1d50") &&
                strstr(detail->DevicePath, "pid_606f"))
                count++;
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(dev_info);
    return count;
}

WINCAN_DEV *wincan_open(int device_index)
{
    char *path = find_device_path(device_index);
    if (!path) {
        int total = wincan_device_count();
        if (total == 0)
            fprintf(stderr, "candleLight device (VID=1D50 PID=606F) not found.\n"
                            "Ensure the device is plugged in and WinUSB is installed.\n");
        else
            fprintf(stderr, "candleLight device index %d not found "
                            "(%d device(s) connected, indices 0-%d).\n",
                    device_index, total, total - 1);
        return NULL;
    }

    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
    free(path);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateFile failed: %lu\n", GetLastError());
        return NULL;
    }

    WINUSB_INTERFACE_HANDLE usb;
    if (!WinUsb_Initialize(h, &usb)) {
        fprintf(stderr, "WinUsb_Initialize failed: %lu\n", GetLastError());
        CloseHandle(h);
        return NULL;
    }

    WINCAN_DEV *d = malloc(sizeof(*d));
    d->dev = h;
    d->usb = usb;
    return d;
}

void wincan_close(WINCAN_DEV *d)
{
    if (!d) return;
    WinUsb_Free(d->usb);
    CloseHandle(d->dev);
    free(d);
}

static int ctrl_out(WINCAN_DEV *d, u8 request, u16 value,
                    void *data, u16 len)
{
    WINUSB_SETUP_PACKET pkt = {
        .RequestType = 0x41, /* vendor | interface | host->device */
        .Request     = request,
        .Value       = value,
        .Index       = 0,
        .Length      = len,
    };
    ULONG transferred;
    return WinUsb_ControlTransfer(d->usb, pkt, data, len,
                                  &transferred, NULL) ? 0 : -1;
}

int wincan_channel_init(WINCAN_DEV *d, int channel, int bitrate_kbps)
{
    struct gs_device_bittiming presets[] = {
        BITTIMING_125K,
        BITTIMING_250K,
        BITTIMING_500K,
        BITTIMING_1000K,
    };
    int rates[] = { 125, 250, 500, 1000 };
    struct gs_device_bittiming *bt = NULL;

    for (int i = 0; i < 4; i++) {
        if (rates[i] == bitrate_kbps) { bt = &presets[i]; break; }
    }
    if (!bt) {
        fprintf(stderr, "Unsupported bitrate %d kbps. "
                        "Valid values: 125, 250, 500, 1000\n", bitrate_kbps);
        return -1;
    }

    if (ctrl_out(d, GS_USB_BREQ_BITTIMING, (u16)channel, bt, sizeof(*bt)) != 0) {
        fprintf(stderr, "BITTIMING failed for channel %d: %lu\n",
                channel, GetLastError());
        return -1;
    }

    struct gs_device_mode mode = { .mode = GS_CAN_MODE_START, .feature = 0 };
    if (ctrl_out(d, GS_USB_BREQ_MODE, (u16)channel, &mode, sizeof(mode)) != 0) {
        fprintf(stderr, "MODE START failed for channel %d: %lu\n",
                channel, GetLastError());
        return -1;
    }
    return 0;
}

void wincan_channel_reset(WINCAN_DEV *d, int channel)
{
    struct gs_device_mode mode = { .mode = GS_CAN_MODE_RESET, .feature = 0 };
    ctrl_out(d, GS_USB_BREQ_MODE, (u16)channel, &mode, sizeof(mode));
}

int wincan_send(WINCAN_DEV *d, const struct gs_host_frame *f)
{
    ULONG transferred;
    if (!WinUsb_WritePipe(d->usb, WINCAN_EP_OUT,
                          (PUCHAR)f, sizeof(*f),
                          &transferred, NULL))
        return -1;
    return 0;
}

int wincan_recv(WINCAN_DEV *d, struct gs_host_frame *f, DWORD timeout_ms)
{
    if (timeout_ms > 0) {
        ULONG val = timeout_ms;
        WinUsb_SetPipePolicy(d->usb, WINCAN_EP_IN,
                             PIPE_TRANSFER_TIMEOUT, sizeof(val), &val);
    }

    ULONG transferred = 0;
    if (!WinUsb_ReadPipe(d->usb, WINCAN_EP_IN,
                         (PUCHAR)f, sizeof(*f),
                         &transferred, NULL))
        return 0;

    return (transferred == sizeof(*f)) ? 1 : 0;
}

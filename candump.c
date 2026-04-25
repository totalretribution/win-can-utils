#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <windows.h>
#include "wincan.h"
#include "gs_usb.h"

static volatile int g_running = 1;
static WINCAN_DEV *g_dev = NULL;
static int g_ch0 = 0, g_ch1 = 0;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    g_running = 0;
    return TRUE;
}

static void print_frame(const struct gs_host_frame *f)
{
    int eff = (f->can_id & CAN_EFF_FLAG) != 0;
    int rtr = (f->can_id & CAN_RTR_FLAG) != 0;
    u32 id  = f->can_id & (eff ? CAN_EFF_MASK : CAN_SFF_MASK);

    if (eff)
        printf("can%u  %08X#  [%u] ", f->channel, id, f->can_dlc);
    else
        printf("can%u  %03X   [%u] ", f->channel, id, f->can_dlc);

    if (rtr) {
        printf(" remote request\n");
        return;
    }

    u8 dlc = f->can_dlc > 8 ? 8 : f->can_dlc;
    for (u8 i = 0; i < dlc; i++)
        printf(" %02X", f->data[i]);
    printf("\n");
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [can0 | can1 | both] [-b bitrate]\n"
                    "  -b bitrate   125, 250, 500, or 1000 kbps (default: 250)\n"
                    "Default channel: both\n", prog);
}

int main(int argc, char *argv[])
{
    int dump0 = 1, dump1 = 1;
    int bitrate = 250;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i+1 < argc) {
            bitrate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "can0") == 0) { dump0=1; dump1=0; }
        else if  (strcmp(argv[i], "can1") == 0) { dump0=0; dump1=1; }
        else if  (strcmp(argv[i], "both") == 0) { dump0=1; dump1=1; }
        else { usage(argv[0]); return 1; }
    }

    g_dev = wincan_open();
    if (!g_dev) return 1;

    if (dump0 && wincan_channel_init(g_dev, 0, bitrate) != 0) {
        wincan_close(g_dev); return 1;
    }
    if (dump1 && wincan_channel_init(g_dev, 1, bitrate) != 0) {
        if (dump0) wincan_channel_reset(g_dev, 0);
        wincan_close(g_dev); return 1;
    }
    g_ch0 = dump0;
    g_ch1 = dump1;

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    printf("Listening on %s%s%s... (Ctrl-C to stop)\n",
           dump0 ? "can0" : "",
           (dump0 && dump1) ? " + " : "",
           dump1 ? "can1" : "");

    struct gs_host_frame f;
    while (g_running) {
        if (!wincan_recv(g_dev, &f, 100))
            continue;

        /* Skip TX echo frames */
        if (f.echo_id != GS_CAN_EVENT_RX_FRAME)
            continue;

        /* Filter to requested channel(s) */
        if (f.channel == 0 && !dump0) continue;
        if (f.channel == 1 && !dump1) continue;

        print_frame(&f);
    }

    if (g_ch0) wincan_channel_reset(g_dev, 0);
    if (g_ch1) wincan_channel_reset(g_dev, 1);
    wincan_close(g_dev);
    return 0;
}

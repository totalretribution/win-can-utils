#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include "wincan.h"
#include "gs_usb.h"

static volatile int g_running = 1;
static WINCAN_DEV *g_dev = NULL;
static int g_channel = 0;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    g_running = 0;
    return TRUE;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [can0 | can1] [-n count] [-g gap_ms] [-b bitrate]\n"
        "  can0/can1    channel to transmit on (default: can0)\n"
        "  -n count     number of frames (default: infinite)\n"
        "  -g gap_ms    delay between frames in ms (default: 100)\n"
        "  -b bitrate   125, 250, 500, or 1000 kbps (default: 250)\n",
        prog);
}

int main(int argc, char *argv[])
{
    int channel  = 0;
    long count   = -1;  /* -1 = infinite */
    DWORD gap_ms = 100;
    int bitrate  = 250;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("cangen %s\n", VERSION); return 0;
        } else if (strcmp(argv[i], "can0") == 0) {
            channel = 0;
        } else if (strcmp(argv[i], "can1") == 0) {
            channel = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) {
            count = atol(argv[++i]);
        } else if (strcmp(argv[i], "-g") == 0 && i+1 < argc) {
            gap_ms = (DWORD)atol(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) {
            bitrate = atoi(argv[++i]);
        } else {
            usage(argv[0]); return 1;
        }
    }

    g_dev = wincan_open();
    if (!g_dev) return 1;

    if (wincan_channel_init(g_dev, channel, bitrate) != 0) {
        wincan_close(g_dev); return 1;
    }
    g_channel = channel;

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    srand((unsigned)time(NULL));

    printf("Generating frames on can%d (gap=%lums%s)... Ctrl-C to stop\n",
           channel, (unsigned long)gap_ms,
           count >= 0 ? "" : ", infinite");

    u32 echo_seq = 1;
    long sent = 0;

    while (g_running && (count < 0 || sent < count)) {
        struct gs_host_frame f;
        memset(&f, 0, sizeof(f));

        f.echo_id = echo_seq++;
        f.can_id  = (u32)(rand() & (int)CAN_SFF_MASK);
        f.can_dlc = 8;
        f.channel = (u8)channel;
        for (int i = 0; i < 8; i++)
            f.data[i] = (u8)(rand() & 0xFF);

        if (wincan_send(g_dev, &f) != 0)
            fprintf(stderr, "Send error: %lu\n", GetLastError());
        else
            sent++;

        if (gap_ms > 0)
            Sleep(gap_ms);
    }

    printf("Sent %ld frame(s).\n", sent);
    wincan_channel_reset(g_dev, channel);
    wincan_close(g_dev);
    return 0;
}

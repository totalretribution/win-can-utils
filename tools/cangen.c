#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <wincan/wincan.h>
#include "common.h"

static volatile int g_running = 1;
static wincan_bus_t *g_bus = NULL;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    g_running = 0;
    return TRUE;
}

static void print_frame(int channel, const wincan_frame_t *f)
{
    if (f->ext)
        printf("can%d  %08X#  [%u]", channel, f->id, f->dlc);
    else
        printf("can%d  %03X   [%u]", channel, f->id, f->dlc);

    if (f->rtr) { printf("  remote request\n"); return; }

    for (int i = 0; i < f->dlc; i++)
        printf(" %02X", f->data[i]);
    printf("\n");
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [can0 | can1] [-n count] [-g gap_ms] [-b bitrate] [-d index] [-v] [--server]\n"
        "  can0/can1    channel to transmit on (default: can0)\n"
        "  -n count     number of frames (default: infinite)\n"
        "  -g gap_ms    delay between frames in ms (default: 100)\n"
        "  -b bitrate   125, 250, 500, or 1000 kbps (default: 250)\n"
        "  -d index     device index for multiple adapters (default: 0)\n"
        "  -v           print each frame as it is sent\n"
        "  --usb        connect directly to USB device (default: use server)\n",
        prog);
}

int main(int argc, char *argv[])
{
    int channel = 0;
    long count  = -1;
    DWORD gap_ms = 100;
    int bitrate = 250, device = 0, use_server = 1, verbose = 0;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--version") == 0) { printf("cangen " VERSION "\n"); return 0; }
        else if (strcmp(argv[i], "can0") == 0)      channel    = 0;
        else if (strcmp(argv[i], "can1") == 0)      channel    = 1;
        else if (strcmp(argv[i], "--usb") == 0)     use_server = 0;
        else if (strcmp(argv[i], "-v") == 0)        verbose    = 1;
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) count   = atol(argv[++i]);
        else if (strcmp(argv[i], "-g") == 0 && i+1 < argc) gap_ms  = (DWORD)atol(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) device  = atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }

    wincan_config_ex_t cfg = {0};
    cfg.channel      = channel;
    cfg.bitrate_kbps = bitrate;
    cfg.use_server   = use_server;
    g_bus = wincan_open_ex(&cfg);
    if (!g_bus) return 1;

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    srand((unsigned)time(NULL));

    printf("Generating frames on can%d (gap=%lums%s)%s... Ctrl-C to stop\n",
           channel, (unsigned long)gap_ms,
           count >= 0 ? "" : ", infinite",
           use_server ? " [server]" : "");

    long sent = 0;
    while (g_running && (count < 0 || sent < count)) {
        wincan_frame_t f = {0};
        f.id  = (uint32_t)(rand() & 0x7FF);
        f.dlc = 8;
        for (int i = 0; i < 8; i++)
            f.data[i] = (uint8_t)(rand() & 0xFF);

        int rc = wincan_send(g_bus, &f, 500);
        if (rc == WINCAN_ERR_IO) {
            fprintf(stderr, "cangen: device disconnected\n");
            break;
        }
        if (rc != WINCAN_OK)
            fprintf(stderr, "Send error: %s\n", wincan_strerror(rc));
        else {
            sent++;
            if (verbose) print_frame(channel, &f);
        }

        if (gap_ms > 0) Sleep(gap_ms);
    }

    printf("Sent %ld frame(s).\n", sent);
    wincan_close(g_bus);
    return 0;
}

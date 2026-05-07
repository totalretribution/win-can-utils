#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wincan/wincan.h>
#include "common.h"

static volatile int g_running = 1;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    g_running = 0;
    return TRUE;
}

/* Protocol: 0=vcan0, 1=vcan1, 2=can0, 3=can1 */
static const char *chan_name(int ch)
{
    switch (ch) {
        case 0: return "vcan0";
        case 1: return "vcan1";
        case 2: return "can0";
        case 3: return "can1";
        default: return "?";
    }
}

static void print_frame(int channel, const wincan_frame_t *f)
{
    const char *name = chan_name(channel);
    if (f->ext)
        printf("%-5s  %08X#  [%u]", name, f->id, f->dlc);
    else
        printf("%-5s  %03X   [%u]", name, f->id, f->dlc);

    if (f->rtr) { printf("  remote request\n"); return; }

    for (int i = 0; i < f->dlc; i++)
        printf(" %02X", f->data[i]);
    printf("\n");
}

typedef struct { wincan_bus_t *bus; int channel; } DumpArgs;

static DWORD WINAPI dump_thread(LPVOID param)
{
    DumpArgs *a = (DumpArgs *)param;
    wincan_frame_t f;
    while (g_running) {
        int rc = wincan_recv(a->bus, &f, 100);
        if (rc == WINCAN_OK)
            print_frame(a->channel, &f);
        else if (rc == WINCAN_ERR_IO) {
            fprintf(stderr, "candump: %s disconnected\n", chan_name(a->channel));
            g_running = 0;
            break;
        }
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [channel...] [-b bitrate] [-d index] [--usb]\n"
        "  channel    vcan0, vcan1, can0, can1, both (default: both)\n"
        "             multiple channels may be specified\n"
        "  -b bitrate 125, 250, 500, or 1000 kbps (default: 250)\n"
        "  -d index   device index for multiple adapters (default: 0)\n"
        "  --usb      connect directly to USB device (default: use server)\n"
        "  vcan channels always require server mode\n", prog);
}

int main(int argc, char *argv[])
{
    int dump_ch[4]  = {0, 0, 0, 0};  /* indexed by protocol channel */
    int any         = 0;
    int bitrate     = 250;
    int device      = 0;
    int use_server  = 1;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--version") == 0) { printf("candump " VERSION "\n"); return 0; }
        else if (strcmp(argv[i], "vcan0") == 0) { dump_ch[0] = 1; any = 1; }
        else if (strcmp(argv[i], "vcan1") == 0) { dump_ch[1] = 1; any = 1; }
        else if (strcmp(argv[i], "can0") == 0)  { dump_ch[2] = 1; any = 1; }
        else if (strcmp(argv[i], "can1") == 0)  { dump_ch[3] = 1; any = 1; }
        else if (strcmp(argv[i], "both") == 0)  { dump_ch[2] = dump_ch[3] = 1; any = 1; }
        else if (strcmp(argv[i], "--usb") == 0) { use_server = 0; }
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) device  = atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }

    if (!any)
        dump_ch[2] = dump_ch[3] = 1;  /* default: can0 + can1 */

    if ((dump_ch[0] || dump_ch[1]) && !use_server) {
        fprintf(stderr, "candump: vcan channels require server mode (drop --usb)\n");
        return 1;
    }

    wincan_bus_t *bus[4]  = {NULL, NULL, NULL, NULL};
    int channels[4]       = {-1, -1, -1, -1};
    int n = 0;

    for (int ch = 0; ch < 4; ch++) {
        if (!dump_ch[ch]) continue;
        wincan_config_ex_t cfg = {0};
        cfg.channel      = ch;
        cfg.bitrate_kbps = bitrate;
        cfg.use_server   = (ch < 2) ? 1 : use_server;
        wincan_bus_t *b = wincan_open_ex(&cfg);
        if (!b) {
            if (ch < 2)
                fprintf(stderr, "candump: %s not available "
                        "(is server running?)\n", chan_name(ch));
            else
                fprintf(stderr, "candump: %s not available\n", chan_name(ch));
            continue;
        }
        bus[n]      = b;
        channels[n] = ch;
        n++;
    }

    if (n == 0) {
        fprintf(stderr, "candump: no channels available\n");
        return 1;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    char display[64] = {0};
    for (int i = 0; i < n; i++) {
        if (i) strcat(display, " + ");
        strcat(display, chan_name(channels[i]));
    }
    printf("Listening on %s [%s]... (Ctrl-C to stop)\n",
           display, use_server ? "server" : "usb");

    HANDLE threads[4];
    DumpArgs args[4];
    for (int i = 0; i < n; i++) {
        args[i].bus     = bus[i];
        args[i].channel = channels[i];
        threads[i] = CreateThread(NULL, 0, dump_thread, &args[i], 0, NULL);
    }

    WaitForMultipleObjects(n, threads, TRUE, INFINITE);
    for (int i = 0; i < n; i++) CloseHandle(threads[i]);
    for (int i = 0; i < n; i++) wincan_close(bus[i]);
    return 0;
}

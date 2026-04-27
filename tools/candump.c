#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wincan/wincan.h>

static volatile int g_running = 1;

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

typedef struct { wincan_bus_t *bus; int channel; } DumpArgs;

static DWORD WINAPI dump_thread(LPVOID param)
{
    DumpArgs *a = (DumpArgs *)param;
    wincan_frame_t f;
    while (g_running) {
        if (wincan_recv(a->bus, &f, 100) == WINCAN_OK)
            print_frame(a->channel, &f);
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [can0 | can1 | both] [-b bitrate] [-d index]\n"
        "  -b bitrate   125, 250, 500, or 1000 kbps (default: 250)\n"
        "  -d index     device index for multiple adapters (default: 0)\n"
        "Default channel: both\n", prog);
}

int main(int argc, char *argv[])
{
    int dump0 = 1, dump1 = 1, bitrate = 250, device = 0;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--version") == 0) { printf("candump " VERSION "\n"); return 0; }
        else if (strcmp(argv[i], "can0") == 0)      { dump0 = 1; dump1 = 0; }
        else if (strcmp(argv[i], "can1") == 0)      { dump0 = 0; dump1 = 1; }
        else if (strcmp(argv[i], "both") == 0)      { dump0 = 1; dump1 = 1; }
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) device  = atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }

    wincan_bus_t *bus[2] = {NULL, NULL};
    int channels[2] = {-1, -1};
    int n = 0;

    if (dump0) {
        wincan_config_t cfg = {0};
        cfg.device_index = device; cfg.channel = 0; cfg.bitrate_kbps = bitrate;
        bus[n] = wincan_open(&cfg);
        if (!bus[n]) return 1;
        channels[n++] = 0;
    }
    if (dump1) {
        wincan_config_t cfg = {0};
        cfg.device_index = device; cfg.channel = 1; cfg.bitrate_kbps = bitrate;
        bus[n] = wincan_open(&cfg);
        if (!bus[n]) {
            if (n == 0) return 1; /* can0 also failed, nothing to do */
            fprintf(stderr, "candump: can1 not available, listening on can0 only\n");
        } else {
            channels[n++] = 1;
        }
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    printf("Listening on %s%s%s... (Ctrl-C to stop)\n",
           dump0 ? "can0" : "",
           (dump0 && dump1) ? " + " : "",
           dump1 ? "can1" : "");

    HANDLE threads[2];
    DumpArgs args[2];
    for (int i = 0; i < n; i++) {
        args[i].bus = bus[i];
        args[i].channel = channels[i];
        threads[i] = CreateThread(NULL, 0, dump_thread, &args[i], 0, NULL);
    }

    WaitForMultipleObjects(n, threads, TRUE, INFINITE);
    for (int i = 0; i < n; i++) CloseHandle(threads[i]);
    for (int i = 0; i < n; i++) wincan_close(bus[i]);
    return 0;
}

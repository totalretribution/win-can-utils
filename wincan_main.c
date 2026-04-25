#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include "wincan.h"
#include "gs_usb.h"

/* ------------------------------------------------------------------ */
/*  Shared state                                                        */
/* ------------------------------------------------------------------ */

static volatile int  g_running  = 1;
static WINCAN_DEV   *g_dev      = NULL;

/* channels initialised so the Ctrl-C handler can clean up */
static int g_ch[2] = {0, 0};

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    g_running = 0;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Dump thread                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int ch0, ch1;           /* which channels to print */
} DumpArgs;

static void print_frame(const struct gs_host_frame *f)
{
    int eff = (f->can_id & CAN_EFF_FLAG) != 0;
    int rtr = (f->can_id & CAN_RTR_FLAG) != 0;
    u32 id  = f->can_id & (eff ? CAN_EFF_MASK : CAN_SFF_MASK);

    if (eff)
        printf("can%u  %08X#  [%u]", f->channel, id, f->can_dlc);
    else
        printf("can%u  %03X   [%u]", f->channel, id, f->can_dlc);

    if (rtr) { printf("  remote\n"); return; }

    u8 dlc = f->can_dlc > 8 ? 8 : f->can_dlc;
    for (u8 i = 0; i < dlc; i++)
        printf(" %02X", f->data[i]);
    printf("\n");
}

static DWORD WINAPI dump_thread(LPVOID param)
{
    DumpArgs *a = (DumpArgs *)param;
    struct gs_host_frame f;

    while (g_running) {
        if (!wincan_recv(g_dev, &f, 100))
            continue;
        if (f.echo_id != GS_CAN_EVENT_RX_FRAME)
            continue;
        if (f.channel == 0 && !a->ch0) continue;
        if (f.channel == 1 && !a->ch1) continue;
        print_frame(&f);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Gen thread                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int   channel;
    long  count;    /* -1 = infinite */
    DWORD gap_ms;
} GenArgs;

static DWORD WINAPI gen_thread(LPVOID param)
{
    GenArgs *a = (GenArgs *)param;
    u32 echo_seq = 1;
    long sent = 0;

    while (g_running && (a->count < 0 || sent < a->count)) {
        struct gs_host_frame f;
        memset(&f, 0, sizeof(f));
        f.echo_id = echo_seq++;
        f.can_id  = (u32)(rand() & (int)CAN_SFF_MASK);
        f.can_dlc = 8;
        f.channel = (u8)a->channel;
        for (int i = 0; i < 8; i++)
            f.data[i] = (u8)(rand() & 0xFF);

        if (wincan_send(g_dev, &f) != 0)
            fprintf(stderr, "gen: send error %lu\n", GetLastError());
        else
            sent++;

        if (a->gap_ms > 0)
            Sleep(a->gap_ms);
    }
    printf("gen: sent %ld frame(s).\n", sent);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s dump [can0|can1|both] [-b bitrate] [-d index]\n"
        "  %s gen  [can0|can1] [-n count] [-g ms] [-b bitrate] [-d index]\n"
        "  %s both --dump [can0|can1|both] --gen [can0|can1] [-n count] [-g ms] [-b bitrate] [-d index]\n"
        "\n"
        "  -b bitrate   125, 250, 500, or 1000 kbps (default: 250)\n"
        "  -d index     USB device index when multiple adapters are connected (default: 0)\n"
        "Defaults: --dump both  --gen can0  count=infinite  gap=100ms\n",
        prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("wincan %s\n", VERSION); return 0;
    }

    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];
    int is_dump = (strcmp(cmd, "dump") == 0);
    int is_gen  = (strcmp(cmd, "gen")  == 0);
    int is_both = (strcmp(cmd, "both") == 0);

    if (!is_dump && !is_gen && !is_both) { usage(argv[0]); return 1; }

    /* defaults */
    DumpArgs da = { .ch0 = 1, .ch1 = 1 };
    GenArgs  ga = { .channel = 0, .count = -1, .gap_ms = 100 };
    int bitrate = 250;
    int device  = 0;

    /* parse remaining args */
    for (int i = 2; i < argc; i++) {
        if (is_both && strcmp(argv[i], "--dump") == 0 && i+1 < argc) {
            /* --dump can0|can1|both */
            const char *ch = argv[++i];
            if      (strcmp(ch, "can0") == 0) { da.ch0=1; da.ch1=0; }
            else if (strcmp(ch, "can1") == 0) { da.ch0=0; da.ch1=1; }
            else if (strcmp(ch, "both") == 0) { da.ch0=1; da.ch1=1; }
            else { usage(argv[0]); return 1; }
        } else if (is_both && strcmp(argv[i], "--gen") == 0 && i+1 < argc) {
            /* --gen can0|can1 */
            const char *ch = argv[++i];
            if      (strcmp(ch, "can0") == 0) ga.channel = 0;
            else if (strcmp(ch, "can1") == 0) ga.channel = 1;
            else { usage(argv[0]); return 1; }
        } else if (strcmp(argv[i], "can0") == 0) {
            if (is_gen)  ga.channel = 0;
            if (is_dump) { da.ch0=1; da.ch1=0; }
        } else if (strcmp(argv[i], "can1") == 0) {
            if (is_gen)  ga.channel = 1;
            if (is_dump) { da.ch0=0; da.ch1=1; }
        } else if (strcmp(argv[i], "both") == 0 && is_dump) {
            da.ch0 = 1; da.ch1 = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) {
            ga.count = atol(argv[++i]);
        } else if (strcmp(argv[i], "-g") == 0 && i+1 < argc) {
            ga.gap_ms = (DWORD)atol(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) {
            bitrate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) {
            device = atoi(argv[++i]);
        } else {
            usage(argv[0]); return 1;
        }
    }

    /* open device */
    g_dev = wincan_open(device);
    if (!g_dev) return 1;

    /* init channels */
    if ((is_dump || is_both) && da.ch0) {
        if (wincan_channel_init(g_dev, 0, bitrate) != 0) { wincan_close(g_dev); return 1; }
        g_ch[0] = 1;
    }
    if ((is_dump || is_both) && da.ch1) {
        if (wincan_channel_init(g_dev, 1, bitrate) != 0) {
            if (g_ch[0]) wincan_channel_reset(g_dev, 0);
            wincan_close(g_dev); return 1;
        }
        g_ch[1] = 1;
    }
    if (is_gen) {
        if (wincan_channel_init(g_dev, ga.channel, bitrate) != 0) {
            wincan_close(g_dev); return 1;
        }
        g_ch[ga.channel] = 1;
    }
    /* for 'both', gen channel may not be initialised yet */
    if (is_both && !g_ch[ga.channel]) {
        if (wincan_channel_init(g_dev, ga.channel, bitrate) != 0) {
            if (g_ch[0]) wincan_channel_reset(g_dev, 0);
            if (g_ch[1]) wincan_channel_reset(g_dev, 1);
            wincan_close(g_dev); return 1;
        }
        g_ch[ga.channel] = 1;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    srand((unsigned)time(NULL));

    HANDLE threads[2] = {NULL, NULL};
    int nthreads = 0;

    if (is_dump || is_both) {
        printf("Dumping %s%s%s... (Ctrl-C to stop)\n",
               da.ch0 ? "can0" : "",
               (da.ch0 && da.ch1) ? "+" : "",
               da.ch1 ? "can1" : "");
        threads[nthreads++] = CreateThread(NULL, 0, dump_thread, &da, 0, NULL);
    }
    if (is_gen || is_both) {
        printf("Generating on can%d (gap=%lums%s)...\n",
               ga.channel, (unsigned long)ga.gap_ms,
               ga.count >= 0 ? "" : " infinite");
        threads[nthreads++] = CreateThread(NULL, 0, gen_thread, &ga, 0, NULL);
    }

    WaitForMultipleObjects(nthreads, threads, TRUE, INFINITE);
    for (int i = 0; i < nthreads; i++)
        CloseHandle(threads[i]);

    if (g_ch[0]) wincan_channel_reset(g_dev, 0);
    if (g_ch[1]) wincan_channel_reset(g_dev, 1);
    wincan_close(g_dev);
    return 0;
}

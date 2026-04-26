#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <wincan/wincan.h>

static volatile int g_running = 1;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    g_running = 0;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Dump thread                                                         */
/* ------------------------------------------------------------------ */

typedef struct { wincan_bus_t *bus; int channel; } DumpArgs;

static void print_frame(int channel, const wincan_frame_t *f)
{
    if (f->ext)
        printf("can%d  %08X#  [%u]", channel, f->id, f->dlc);
    else
        printf("can%d  %03X   [%u]", channel, f->id, f->dlc);

    if (f->rtr) { printf("  remote\n"); return; }

    for (int i = 0; i < f->dlc; i++) printf(" %02X", f->data[i]);
    printf("\n");
}

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

/* ------------------------------------------------------------------ */
/*  Gen thread                                                          */
/* ------------------------------------------------------------------ */

typedef struct { wincan_bus_t *bus; long count; DWORD gap_ms; } GenArgs;

static DWORD WINAPI gen_thread(LPVOID param)
{
    GenArgs *a = (GenArgs *)param;
    long sent = 0;

    while (g_running && (a->count < 0 || sent < a->count)) {
        wincan_frame_t f = {0};
        f.id  = (uint32_t)(rand() & 0x7FF);
        f.dlc = 8;
        for (int i = 0; i < 8; i++) f.data[i] = (uint8_t)(rand() & 0xFF);

        int rc = wincan_send(a->bus, &f, 500);
        if (rc != WINCAN_OK)
            fprintf(stderr, "gen: %s\n", wincan_strerror(rc));
        else
            sent++;

        if (a->gap_ms > 0) Sleep(a->gap_ms);
    }
    printf("gen: sent %ld frame(s).\n", sent);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s dump [can0|can1|both] [-b bitrate] [-d index]\n"
        "  %s gen  [can0|can1] [-n count] [-g ms] [-b bitrate] [-d index]\n"
        "  %s both --dump [can0|can1|both] --gen [can0|can1] "
              "[-n count] [-g ms] [-b bitrate] [-d index]\n"
        "\n"
        "  -b bitrate   125, 250, 500, or 1000 kbps (default: 250)\n"
        "  -d index     device index for multiple adapters (default: 0)\n"
        "Defaults: --dump both  --gen can0  count=infinite  gap=100ms\n",
        prog, prog, prog);
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("canboth " VERSION "\n"); return 0;
    }
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];
    int is_dump = strcmp(cmd, "dump") == 0;
    int is_gen  = strcmp(cmd, "gen")  == 0;
    int is_both = strcmp(cmd, "both") == 0;
    if (!is_dump && !is_gen && !is_both) { usage(argv[0]); return 1; }

    int dump_ch0 = 1, dump_ch1 = 1;
    int gen_ch   = 0;
    long gen_count = -1;
    DWORD gen_gap  = 100;
    int bitrate = 250, device = 0;

    for (int i = 2; i < argc; i++) {
        if (is_both && strcmp(argv[i], "--dump") == 0 && i+1 < argc) {
            const char *ch = argv[++i];
            if      (strcmp(ch, "can0") == 0) { dump_ch0=1; dump_ch1=0; }
            else if (strcmp(ch, "can1") == 0) { dump_ch0=0; dump_ch1=1; }
            else if (strcmp(ch, "both") == 0) { dump_ch0=1; dump_ch1=1; }
            else { usage(argv[0]); return 1; }
        } else if (is_both && strcmp(argv[i], "--gen") == 0 && i+1 < argc) {
            const char *ch = argv[++i];
            if      (strcmp(ch, "can0") == 0) gen_ch = 0;
            else if (strcmp(ch, "can1") == 0) gen_ch = 1;
            else { usage(argv[0]); return 1; }
        } else if (strcmp(argv[i], "can0") == 0) {
            if (is_gen)  gen_ch = 0;
            if (is_dump) { dump_ch0=1; dump_ch1=0; }
        } else if (strcmp(argv[i], "can1") == 0) {
            if (is_gen)  gen_ch = 1;
            if (is_dump) { dump_ch0=0; dump_ch1=1; }
        } else if (strcmp(argv[i], "both") == 0 && is_dump) {
            dump_ch0 = 1; dump_ch1 = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) {
            gen_count = atol(argv[++i]);
        } else if (strcmp(argv[i], "-g") == 0 && i+1 < argc) {
            gen_gap = (DWORD)atol(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) {
            bitrate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) {
            device = atoi(argv[++i]);
        } else {
            usage(argv[0]); return 1;
        }
    }

    /* Open buses */
    wincan_bus_t *dump_bus[2] = {NULL, NULL};
    int dump_channels[2] = {-1, -1};
    int n_dump = 0;
    wincan_bus_t *gen_bus = NULL;

    if (is_dump || is_both) {
        if (dump_ch0) {
            wincan_config_t cfg = {0};
            cfg.device_index = device; cfg.channel = 0; cfg.bitrate_kbps = bitrate;
            dump_bus[n_dump] = wincan_open(&cfg);
            if (!dump_bus[n_dump]) goto fail;
            dump_channels[n_dump++] = 0;
        }
        if (dump_ch1) {
            wincan_config_t cfg = {0};
            cfg.device_index = device; cfg.channel = 1; cfg.bitrate_kbps = bitrate;
            dump_bus[n_dump] = wincan_open(&cfg);
            if (!dump_bus[n_dump]) goto fail;
            dump_channels[n_dump++] = 1;
        }
    }
    if (is_gen || is_both) {
        wincan_config_t cfg = {0};
        cfg.device_index = device; cfg.channel = gen_ch; cfg.bitrate_kbps = bitrate;
        gen_bus = wincan_open(&cfg);
        if (!gen_bus) goto fail;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    srand((unsigned)time(NULL));

    HANDLE threads[3];
    int nthreads = 0;
    DumpArgs dargs[2];
    GenArgs  gargs;

    if (is_dump || is_both) {
        printf("Dumping %s%s%s... (Ctrl-C to stop)\n",
               dump_ch0 ? "can0" : "",
               (dump_ch0 && dump_ch1) ? "+" : "",
               dump_ch1 ? "can1" : "");
        for (int i = 0; i < n_dump; i++) {
            dargs[i].bus = dump_bus[i];
            dargs[i].channel = dump_channels[i];
            threads[nthreads++] = CreateThread(NULL, 0, dump_thread, &dargs[i], 0, NULL);
        }
    }
    if (is_gen || is_both) {
        printf("Generating on can%d (gap=%lums%s)...\n",
               gen_ch, (unsigned long)gen_gap, gen_count >= 0 ? "" : " infinite");
        gargs.bus = gen_bus; gargs.count = gen_count; gargs.gap_ms = gen_gap;
        threads[nthreads++] = CreateThread(NULL, 0, gen_thread, &gargs, 0, NULL);
    }

    WaitForMultipleObjects(nthreads, threads, TRUE, INFINITE);
    for (int i = 0; i < nthreads; i++) CloseHandle(threads[i]);
    for (int i = 0; i < n_dump; i++) wincan_close(dump_bus[i]);
    wincan_close(gen_bus);
    return 0;

fail:
    for (int i = 0; i < n_dump; i++) wincan_close(dump_bus[i]);
    wincan_close(gen_bus);
    return 1;
}

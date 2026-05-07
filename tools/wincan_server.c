#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wincan/wincan.h>

/* Forward declaration — defined in src/wincan_server.c */
void wincan_server_run(int bitrate_kbps, int channels_mask, int device_index);
void wincan_server_stop(void);

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    printf("\nwincan_server: shutting down...\n");
    wincan_server_stop();
    return TRUE;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-b bitrate] [-c can0|can1|both] [-d index]\n"
        "  -b bitrate   125, 250, 500, or 1000 kbps (default: 250)\n"
        "  -c channel   USB channel: can0, can1, or both (default: both)\n"
        "  -d index     device index for multiple adapters (default: 0)\n"
        "\n"
        "vcan0 and vcan1 are always available (no USB required).\n"
        "Listens on 127.0.0.1:29526.  Clients use --server flag.\n",
        prog);
}

int main(int argc, char *argv[])
{
    int bitrate       = 250;
    int channels_mask = 0x03; /* both USB channels */
    int device        = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("wincan_server " VERSION "\n");
            return 0;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            bitrate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            const char *ch = argv[++i];
            if      (strcmp(ch, "can0") == 0) channels_mask = 0x01;
            else if (strcmp(ch, "can1") == 0) channels_mask = 0x02;
            else if (strcmp(ch, "both") == 0) channels_mask = 0x03;
            else if (strcmp(ch, "none") == 0) channels_mask = 0x00;
            else { usage(argv[0]); return 1; }
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            device = atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    wincan_server_run(bitrate, channels_mask, device);
    return 0;
}

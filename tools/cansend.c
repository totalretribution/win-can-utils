#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include <wincan/wincan.h>
#include "common.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <channel> <frame> [-b bitrate] [-d index] [--server]\n"
        "\n"
        "  channel    can0 or can1\n"
        "  frame      <can_id>#<data>   standard or extended CAN frame\n"
        "             <can_id>#R        RTR frame\n"
        "             <can_id>#R<dlc>   RTR frame with explicit DLC (0-8)\n"
        "  -b bitrate 125, 250, 500, or 1000 kbps (default: 250)\n"
        "  -d index   device index for multiple adapters (default: 0)\n"
        "  --usb      connect directly to USB device (default: use server)\n"
        "\n"
        "  can_id     up to 3 hex digits for 11-bit (SFF)\n"
        "             up to 8 hex digits for 29-bit (EFF, id > 0x7FF)\n"
        "  data       up to 8 hex bytes, dots optional\n"
        "\n"
        "Examples:\n"
        "  %s can0 123#DEADBEEF\n"
        "  %s can0 123#DE.AD.BE.EF\n"
        "  %s can1 18DAF110#1122334455667788\n"
        "  %s can0 123#R\n"
        "  %s can0 123#R8\n",
        prog, prog, prog, prog, prog, prog);
}

static int parse_data(const char *s, uint8_t *out, int maxlen)
{
    int n = 0;
    while (*s) {
        if (*s == '.') { s++; continue; }
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) {
            fprintf(stderr, "Bad data byte near: %s\n", s);
            return -1;
        }
        if (n >= maxlen) {
            fprintf(stderr, "Too many bytes (max %d)\n", maxlen);
            return -1;
        }
        char buf[3] = { s[0], s[1], '\0' };
        out[n++] = (uint8_t)strtoul(buf, NULL, 16);
        s += 2;
    }
    return n;
}

static int parse_frame(const char *str, wincan_frame_t *f)
{
    memset(f, 0, sizeof(*f));

    const char *hash = strchr(str, '#');
    if (!hash) { fprintf(stderr, "Frame must contain '#': %s\n", str); return -1; }

    size_t id_len = (size_t)(hash - str);
    if (id_len == 0 || id_len > 8) {
        fprintf(stderr, "CAN ID must be 1-8 hex digits\n");
        return -1;
    }

    char id_buf[9] = {0};
    memcpy(id_buf, str, id_len);
    for (size_t i = 0; i < id_len; i++) {
        if (!isxdigit((unsigned char)id_buf[i])) {
            fprintf(stderr, "Non-hex character in CAN ID: %s\n", id_buf);
            return -1;
        }
    }

    uint32_t raw_id = (uint32_t)strtoul(id_buf, NULL, 16);
    if (raw_id > 0x1FFFFFFF) {
        fprintf(stderr, "CAN ID 0x%X out of range (max 0x1FFFFFFF)\n", raw_id);
        return -1;
    }
    f->id  = raw_id;
    f->ext = (raw_id > 0x7FF) ? 1 : 0;

    const char *payload = hash + 1;
    if (payload[0] == 'R' || payload[0] == 'r') {
        f->rtr = 1;
        if (payload[1] != '\0') {
            if (!isdigit((unsigned char)payload[1]) || payload[2] != '\0') {
                fprintf(stderr, "RTR DLC must be a single digit 0-8\n");
                return -1;
            }
            int dlc = payload[1] - '0';
            if (dlc > 8) { fprintf(stderr, "RTR DLC must be 0-8\n"); return -1; }
            f->dlc = (uint8_t)dlc;
        }
    } else {
        int n = parse_data(payload, f->data, 8);
        if (n < 0) return -1;
        f->dlc = (uint8_t)n;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("cansend " VERSION "\n"); return 0;
    }

    if (argc < 3) { usage(argv[0]); return 1; }

    int channel;
    if      (strcmp(argv[1], "can0") == 0) channel = 0;
    else if (strcmp(argv[1], "can1") == 0) channel = 1;
    else { usage(argv[0]); return 1; }

    int bitrate = 250, device = 0, use_server = 1;
    for (int i = 3; i < argc; i++) {
        if      (strcmp(argv[i], "-b") == 0 && i+1 < argc) bitrate    = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) device     = atoi(argv[++i]);
        else if (strcmp(argv[i], "--usb") == 0)             use_server = 0;
        else { usage(argv[0]); return 1; }
    }

    wincan_frame_t f;
    if (parse_frame(argv[2], &f) != 0) return 1;

    wincan_config_ex_t cfg = {0};
    cfg.channel      = channel;
    cfg.bitrate_kbps = bitrate;
    cfg.use_server   = use_server;
    wincan_bus_t *bus = wincan_open_ex(&cfg);
    if (!bus) return 1;

    int rc = wincan_send(bus, &f, 500);
    if (rc != WINCAN_OK)
        fprintf(stderr, "Send failed: %s\n", wincan_strerror(rc));

    wincan_close(bus);
    return rc != WINCAN_OK ? 1 : 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include "wincan.h"
#include "gs_usb.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <channel> <frame> [-b bitrate] [-d index]\n"
        "\n"
        "  channel    can0 or can1\n"
        "  frame      <can_id>#<data>   standard or extended CAN frame\n"
        "             <can_id>#R        RTR frame\n"
        "             <can_id>#R<dlc>   RTR frame with explicit DLC (0-8)\n"
        "  -b bitrate 125, 250, 500, or 1000 kbps (default: 250)\n"
        "  -d index   USB device index when multiple adapters are connected (default: 0)\n"
        "\n"
        "  can_id     up to 3 hex digits for 11-bit (SFF)\n"
        "             up to 8 hex digits for 29-bit (EFF, id > 0x7FF)\n"
        "  data       up to 8 hex bytes, dots between bytes are optional\n"
        "\n"
        "Examples:\n"
        "  %s can0 123#DEADBEEF\n"
        "  %s can0 123#DE.AD.BE.EF\n"
        "  %s can1 18DAF110#1122334455667788\n"
        "  %s can0 123#R\n"
        "  %s can0 123#R8\n"
        "  %s can0 123#DEADBEEF -b 500\n",
        prog, prog, prog, prog, prog, prog, prog);
}

/* Parse a hex string (with optional '.' separators) into bytes.
   Returns number of bytes written, or -1 on error. */
static int parse_data(const char *s, u8 *out, int maxlen)
{
    int n = 0;
    while (*s) {
        /* skip dot separators */
        if (*s == '.') { s++; continue; }

        if (!isxdigit((unsigned char)s[0]) ||
            !isxdigit((unsigned char)s[1])) {
            fprintf(stderr, "Bad data byte near: %s\n", s);
            return -1;
        }
        if (n >= maxlen) {
            fprintf(stderr, "Too many data bytes (max %d)\n", maxlen);
            return -1;
        }
        char buf[3] = { s[0], s[1], '\0' };
        out[n++] = (u8)strtoul(buf, NULL, 16);
        s += 2;
    }
    return n;
}

/* Parse "CANID#..." into a gs_host_frame.
   Returns 0 on success, -1 on error. */
static int parse_frame(const char *str, int channel, struct gs_host_frame *f)
{
    memset(f, 0, sizeof(*f));
    f->echo_id = 1;
    f->channel = (u8)channel;

    /* Split at '#' */
    const char *hash = strchr(str, '#');
    if (!hash) {
        fprintf(stderr, "Frame must contain '#': %s\n", str);
        return -1;
    }

    /* Parse CAN ID */
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
    u32 raw_id = (u32)strtoul(id_buf, NULL, 16);
    if (raw_id > 0x1FFFFFFF) {
        fprintf(stderr, "CAN ID 0x%X out of range (max 0x1FFFFFFF)\n", raw_id);
        return -1;
    }

    if (raw_id > CAN_SFF_MASK) {
        f->can_id = raw_id | CAN_EFF_FLAG;
    } else {
        f->can_id = raw_id;
    }

    /* Parse payload after '#' */
    const char *payload = hash + 1;

    if (payload[0] == 'R' || payload[0] == 'r') {
        /* RTR frame */
        f->can_id |= CAN_RTR_FLAG;
        if (payload[1] != '\0') {
            /* optional DLC digit */
            if (!isdigit((unsigned char)payload[1]) || payload[2] != '\0') {
                fprintf(stderr, "RTR DLC must be a single digit 0-8\n");
                return -1;
            }
            int dlc = payload[1] - '0';
            if (dlc > 8) { fprintf(stderr, "RTR DLC must be 0-8\n"); return -1; }
            f->can_dlc = (u8)dlc;
        }
        /* DLC stays 0 if not specified — that is fine */
    } else {
        /* Data frame */
        int n = parse_data(payload, f->data, 8);
        if (n < 0) return -1;
        f->can_dlc = (u8)n;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("cansend %s\n", VERSION); return 0;
    }

    if (argc < 3) { usage(argv[0]); return 1; }

    int channel;
    if      (strcmp(argv[1], "can0") == 0) channel = 0;
    else if (strcmp(argv[1], "can1") == 0) channel = 1;
    else { usage(argv[0]); return 1; }

    /* argv[2] is the frame; optional flags follow */
    int bitrate = 250;
    int device  = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i+1 < argc)
            bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc)
            device = atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }

    struct gs_host_frame f;
    if (parse_frame(argv[2], channel, &f) != 0)
        return 1;

    WINCAN_DEV *dev = wincan_open(device);
    if (!dev) return 1;

    if (wincan_channel_init(dev, channel, bitrate) != 0) {
        wincan_close(dev); return 1;
    }

    int rc = wincan_send(dev, &f);
    if (rc != 0)
        fprintf(stderr, "Send failed: %lu\n", GetLastError());

    wincan_channel_reset(dev, channel);
    wincan_close(dev);
    return rc != 0 ? 1 : 0;
}

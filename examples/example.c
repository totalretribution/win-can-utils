#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wincan/wincan.h>

static void print_frame(const wincan_frame_t *f)
{
    if (f->ext)
        printf("%08X#  [%u]", f->id, f->dlc);
    else
        printf("%03X   [%u]", f->id, f->dlc);

    if (f->rtr) { printf("  RTR\n"); return; }
    for (int i = 0; i < f->dlc; i++) printf(" %02X", f->data[i]);
    printf("\n");
}

/* Example 1 — Basic send and receive (synchronous, no ring buffer) */
static void example_basic(void)
{
    printf("=== Example 1: Basic send/receive ===\n");

    wincan_config_t cfg = {
        .channel        = 0,
        .bitrate_kbps   = 500,
        .rx_buffer_size = 0,
    };

    wincan_bus_t *bus = wincan_open(&cfg);
    if (!bus) { fprintf(stderr, "Open failed\n"); return; }

    wincan_frame_t tx = {
        .id  = 0x123,
        .dlc = 8,
        .data = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 },
    };
    int rc = wincan_send(bus, &tx, 1000);
    if (rc != WINCAN_OK)
        fprintf(stderr, "Send failed: %s\n", wincan_strerror(rc));
    else
        { printf("Sent:     "); print_frame(&tx); }

    wincan_frame_t rx;
    rc = wincan_recv(bus, &rx, 2000);
    if (rc == WINCAN_OK)
        { printf("Received: "); print_frame(&rx); }
    else
        printf("No frame received: %s\n", wincan_strerror(rc));

    wincan_close(bus);
}

/* Example 2 — Ring buffer with background RX thread + status query */
static void example_buffered(void)
{
    printf("\n=== Example 2: Buffered receive + status ===\n");

    wincan_config_t cfg = {
        .channel        = 0,
        .bitrate_kbps   = 250,
        .rx_buffer_size = WINCAN_DEFAULT_RX_BUFFER,
    };

    wincan_bus_t *bus = wincan_open(&cfg);
    if (!bus) { fprintf(stderr, "Open failed\n"); return; }

    wincan_status_t st;
    if (wincan_get_status(bus, &st) == WINCAN_OK) {
        const char *state_str[] = {
            "error-active", "error-warning", "error-passive", "bus-off"
        };
        printf("State:      %s\n", state_str[st.state]);
        printf("RX errors:  %u\n", st.rx_errors);
        printf("TX errors:  %u\n", st.tx_errors);
        printf("Buffered:   %u frames\n", st.rx_buffered);
        printf("Overflows:  %u\n", st.rx_overflows);
    }

    printf("Draining ring buffer (non-blocking)...\n");
    wincan_frame_t f;
    for (int i = 0; i < 10; i++) {
        if (wincan_recv(bus, &f, 0) != WINCAN_OK) break;
        printf("  [%d] ", i); print_frame(&f);
    }

    wincan_close(bus);
}

/* Example 3 — Receive filters */
static void example_filters(void)
{
    printf("\n=== Example 3: Receive filters ===\n");

    wincan_config_t cfg = {
        .channel        = 1,
        .bitrate_kbps   = 500,
        .rx_buffer_size = 128,
    };

    wincan_bus_t *bus = wincan_open(&cfg);
    if (!bus) { fprintf(stderr, "Open failed\n"); return; }

    wincan_filter_t filters[] = {
        { .id = 0x100, .mask = 0x700, .ext = 0 },
    };
    wincan_set_filters(bus, filters, 1);

    printf("Listening for SFF IDs 0x100-0x1FF on can1 (5 frames or timeout)...\n");
    wincan_frame_t f;
    for (int i = 0; i < 5; i++) {
        if (wincan_recv(bus, &f, 3000) != WINCAN_OK) { printf("Timeout\n"); break; }
        printf("  [%d] ", i); print_frame(&f);
    }

    wincan_set_filters(bus, NULL, 0);
    wincan_close(bus);
}

/* Example 4 — Custom bit timing */
static void example_custom_timing(void)
{
    printf("\n=== Example 4: Custom bit timing ===\n");

    wincan_config_t cfg = {
        .channel      = 0,
        .bitrate_kbps = 0,
        .timing = {
            .prop_seg   = 1,
            .phase_seg1 = 11,
            .phase_seg2 = 3,
            .sjw        = 1,
            .brp        = 12,
        },
    };

    wincan_bus_t *bus = wincan_open(&cfg);
    if (!bus) { fprintf(stderr, "Open failed\n"); return; }

    printf("Opened with custom timing (250 kbps equivalent)\n");

    wincan_frame_t tx = { .id = 0x7FF, .dlc = 1, .data = { 0xAB } };
    int rc = wincan_send(bus, &tx, 500);
    printf("Send 0x7FF#AB: %s\n", wincan_strerror(rc));

    wincan_close(bus);
}

/* Example 5 — Extended (29-bit) frame and RTR */
static void example_eff_rtr(void)
{
    printf("\n=== Example 5: EFF and RTR frames ===\n");

    wincan_config_t cfg = { .channel = 0, .bitrate_kbps = 500 };
    wincan_bus_t *bus = wincan_open(&cfg);
    if (!bus) { fprintf(stderr, "Open failed\n"); return; }

    wincan_frame_t eff = {
        .id  = 0x18DAF110,
        .ext = 1,
        .dlc = 8,
        .data = { 0x02, 0x10, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00 },
    };
    int rc = wincan_send(bus, &eff, 500);
    printf("Send EFF 0x18DAF110: %s\n", wincan_strerror(rc));

    wincan_frame_t rtr = { .id = 0x123, .rtr = 1, .dlc = 8 };
    rc = wincan_send(bus, &rtr, 500);
    printf("Send RTR 0x123:      %s\n", wincan_strerror(rc));

    wincan_close(bus);
}

/* Example 6 — Two independent channel handles on the same device
 *
 * Demonstrates the pattern for two classes each owning one channel.
 * Both call wincan_open independently; the library shares the underlying
 * USB handle automatically so no coordination between classes is needed. */
static void example_dual_channel(void)
{
    printf("\n=== Example 6: Dual channel (can0 + can1 simultaneously) ===\n");

    /* --- Class A owns can0 --- */
    wincan_config_t cfg0 = {
        .channel        = 0,
        .bitrate_kbps   = 250,
        .rx_buffer_size = WINCAN_DEFAULT_RX_BUFFER,
    };
    wincan_bus_t *bus0 = wincan_open(&cfg0);
    if (!bus0) { fprintf(stderr, "Failed to open can0\n"); return; }
    printf("can0 opened\n");

    /* --- Class B owns can1 --- */
    wincan_config_t cfg1 = {
        .channel        = 1,
        .bitrate_kbps   = 250,
        .rx_buffer_size = WINCAN_DEFAULT_RX_BUFFER,
    };
    wincan_bus_t *bus1 = wincan_open(&cfg1);
    if (!bus1) { fprintf(stderr, "Failed to open can1\n"); wincan_close(bus0); return; }
    printf("can1 opened\n");

    /* Each class sends on its own channel */
    wincan_frame_t tx0 = { .id = 0x100, .dlc = 1, .data = { 0xAA } };
    wincan_frame_t tx1 = { .id = 0x200, .dlc = 1, .data = { 0xBB } };

    int rc0 = wincan_send(bus0, &tx0, 500);
    int rc1 = wincan_send(bus1, &tx1, 500);
    printf("can0 send 0x100: %s\n", wincan_strerror(rc0));
    printf("can1 send 0x200: %s\n", wincan_strerror(rc1));

    /* Each class receives independently — frames are filtered by channel */
    wincan_frame_t rx;
    printf("can0 recv: ");
    if (wincan_recv(bus0, &rx, 1000) == WINCAN_OK)
        print_frame(&rx);
    else
        printf("no frame\n");

    printf("can1 recv: ");
    if (wincan_recv(bus1, &rx, 1000) == WINCAN_OK)
        print_frame(&rx);
    else
        printf("no frame\n");

    /* Each class closes independently — USB handle released on last close */
    wincan_close(bus0);
    printf("can0 closed\n");
    wincan_close(bus1);
    printf("can1 closed\n");
}

int main(void)
{
    example_basic();
    example_buffered();
    example_filters();
    example_custom_timing();
    example_eff_rtr();
    example_dual_channel();
    return 0;
}

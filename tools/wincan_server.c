#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winsvc.h>
#include <wincan/wincan.h>

/* Forward declaration — defined in src/wincan_server.c */
void wincan_server_run(int bitrate_kbps, int channels_mask, int device_index);
void wincan_server_stop(void);

/* =========================================================================
 * Service identity
 * ====================================================================== */

#define SVC_NAME         L"WinCANServer"
#define SVC_DISPLAY_NAME L"WinCAN Server"
#define SVC_DESCRIPTION  L"Shares WinUSB CAN adapters over localhost TCP. " \
                          L"Provides vcan0/vcan1 virtual loopback channels."

/* =========================================================================
 * Globals shared between ServiceMain and ServiceCtrlHandler
 * ====================================================================== */

static SERVICE_STATUS        g_svc_status;
static SERVICE_STATUS_HANDLE g_svc_status_handle;

static void svc_set_status(DWORD state, DWORD exit_code, DWORD wait_hint)
{
    g_svc_status.dwCurrentState  = state;
    g_svc_status.dwWin32ExitCode = exit_code;
    g_svc_status.dwWaitHint      = wait_hint;
    SetServiceStatus(g_svc_status_handle, &g_svc_status);
}

/* =========================================================================
 * Service control handler — called by SCM on stop/pause/etc.
 * ====================================================================== */

static VOID WINAPI svc_ctrl_handler(DWORD ctrl)
{
    if (ctrl == SERVICE_CONTROL_STOP) {
        svc_set_status(SERVICE_STOP_PENDING, NO_ERROR, 4000);
        wincan_server_stop();
    }
}

/* =========================================================================
 * ServiceMain — entry point called by SCM
 *
 * argc/argv are the arguments from the service binary path in the registry,
 * i.e. the args passed to --install (--run-service -b N -c X -d N).
 * ====================================================================== */

static VOID WINAPI svc_main(DWORD argc, LPWSTR *argv)
{
    g_svc_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwControlsAccepted        = SERVICE_ACCEPT_STOP;
    g_svc_status.dwServiceSpecificExitCode = 0;
    g_svc_status.dwCheckPoint             = 0;

    g_svc_status_handle = RegisterServiceCtrlHandlerW(SVC_NAME, svc_ctrl_handler);
    if (!g_svc_status_handle) return;

    svc_set_status(SERVICE_START_PENDING, NO_ERROR, 3000);

    /* Parse config args passed via the service binary path */
    int bitrate       = 250;
    int channels_mask = 0x03;
    int device        = 0;

    for (DWORD i = 1; i < argc; i++) {
        if      (wcscmp(argv[i], L"--run-service") == 0) { /* skip */ }
        else if (wcscmp(argv[i], L"-b") == 0 && i+1 < argc) bitrate       = _wtoi(argv[++i]);
        else if (wcscmp(argv[i], L"-d") == 0 && i+1 < argc) device        = _wtoi(argv[++i]);
        else if (wcscmp(argv[i], L"-c") == 0 && i+1 < argc) {
            const wchar_t *ch = argv[++i];
            if      (wcscmp(ch, L"can0") == 0) channels_mask = 0x01;
            else if (wcscmp(ch, L"can1") == 0) channels_mask = 0x02;
            else if (wcscmp(ch, L"both") == 0) channels_mask = 0x03;
            else if (wcscmp(ch, L"none") == 0) channels_mask = 0x00;
        }
    }

    svc_set_status(SERVICE_RUNNING, NO_ERROR, 0);

    wincan_server_run(bitrate, channels_mask, device);

    svc_set_status(SERVICE_STOPPED, NO_ERROR, 0);
}

/* =========================================================================
 * Service management helpers (require admin)
 * ====================================================================== */

static int svc_install(int bitrate, int channels_mask, int device)
{
    /* Build binary path: "C:\path\to\wincan_server.exe" --run-service -b N -c X -d N */
    wchar_t exe_path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe_path, MAX_PATH)) {
        fprintf(stderr, "GetModuleFileNameW failed: %lu\n", GetLastError());
        return 1;
    }

    /* Map channels_mask back to -c argument */
    const wchar_t *ch_arg;
    switch (channels_mask) {
        case 0x01: ch_arg = L"can0"; break;
        case 0x02: ch_arg = L"can1"; break;
        case 0x00: ch_arg = L"none"; break;
        default:   ch_arg = L"both"; break;
    }

    wchar_t bin_path[MAX_PATH + 64];
    _snwprintf(bin_path, sizeof(bin_path) / sizeof(wchar_t),
               L"\"%s\" --run-service -b %d -c %s -d %d",
               exe_path, bitrate, ch_arg, device);

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed: %lu (run as administrator)\n",
                GetLastError());
        return 1;
    }

    SC_HANDLE svc = CreateServiceW(
        scm,
        SVC_NAME,
        SVC_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        bin_path,
        NULL, NULL, NULL,
        NULL,   /* LocalSystem */
        NULL
    );

    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS)
            fprintf(stderr, "Service already installed. Run --uninstall first.\n");
        else
            fprintf(stderr, "CreateService failed: %lu\n", err);
        CloseServiceHandle(scm);
        return 1;
    }

    /* Set description */
    SERVICE_DESCRIPTIONW desc = { (LPWSTR)SVC_DESCRIPTION };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    printf("Service installed. Start with: wincan_server --start\n");
    printf("Or it will start automatically on next boot.\n");

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int svc_uninstall(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed: %lu (run as administrator)\n",
                GetLastError());
        return 1;
    }

    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME,
                                 SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        fprintf(stderr, "Service not found.\n");
        CloseServiceHandle(scm);
        return 1;
    }

    /* Stop if running */
    SERVICE_STATUS st;
    QueryServiceStatus(svc, &st);
    if (st.dwCurrentState != SERVICE_STOPPED) {
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        /* Wait up to 5 s */
        for (int i = 0; i < 50; i++) {
            QueryServiceStatus(svc, &st);
            if (st.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(100);
        }
    }

    if (!DeleteService(svc)) {
        fprintf(stderr, "DeleteService failed: %lu\n", GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }

    printf("Service uninstalled.\n");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int svc_start(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed: %lu (run as administrator)\n",
                GetLastError());
        return 1;
    }

    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        fprintf(stderr, "Service not found. Run --install first.\n");
        CloseServiceHandle(scm);
        return 1;
    }

    if (!StartServiceW(svc, 0, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING)
            fprintf(stderr, "Service is already running.\n");
        else
            fprintf(stderr, "StartService failed: %lu\n", err);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }

    printf("Service started.\n");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int svc_stop(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed: %lu (run as administrator)\n",
                GetLastError());
        return 1;
    }

    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        fprintf(stderr, "Service not found.\n");
        CloseServiceHandle(scm);
        return 1;
    }

    SERVICE_STATUS st;
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_NOT_ACTIVE)
            fprintf(stderr, "Service is not running.\n");
        else
            fprintf(stderr, "ControlService failed: %lu\n", err);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }

    printf("Service stopped.\n");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

/* =========================================================================
 * Standalone ctrl-C handler
 * ====================================================================== */

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    printf("\nwincan_server: shutting down...\n");
    wincan_server_stop();
    return TRUE;
}

/* =========================================================================
 * Usage
 * ====================================================================== */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Standalone mode (default):\n"
        "  -b bitrate   125, 250, 500, or 1000 kbps (default: 250)\n"
        "  -c channel   USB channel: can0, can1, both, or none (default: both)\n"
        "  -d index     device index for multiple adapters (default: 0)\n"
        "\n"
        "Windows Service (requires administrator):\n"
        "  --install [-b bitrate] [-c channel] [-d index]\n"
        "               Install as auto-start Windows service\n"
        "  --uninstall  Remove the service\n"
        "  --start      Start the installed service\n"
        "  --stop       Stop the running service\n"
        "\n"
        "vcan0 and vcan1 are always available (no USB required).\n"
        "Listens on 127.0.0.1:29526.  Clients use --server flag.\n",
        prog);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(int argc, char *argv[])
{
    /* --run-service: launched by SCM, hand off to dispatcher */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--run-service") == 0) {
            SERVICE_TABLE_ENTRYW svc_table[] = {
                { (LPWSTR)SVC_NAME, svc_main },
                { NULL, NULL }
            };
            StartServiceCtrlDispatcherW(svc_table);
            return 0;
        }
    }

    int bitrate       = 250;
    int channels_mask = 0x03;
    int device        = 0;
    int do_install    = 0;
    int do_uninstall  = 0;
    int do_start      = 0;
    int do_stop       = 0;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--version")   == 0) { printf("wincan_server " VERSION "\n"); return 0; }
        else if (strcmp(argv[i], "--install")   == 0) do_install   = 1;
        else if (strcmp(argv[i], "--uninstall") == 0) do_uninstall = 1;
        else if (strcmp(argv[i], "--start")     == 0) do_start     = 1;
        else if (strcmp(argv[i], "--stop")      == 0) do_stop      = 1;
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) device  = atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i+1 < argc) {
            const char *ch = argv[++i];
            if      (strcmp(ch, "can0") == 0) channels_mask = 0x01;
            else if (strcmp(ch, "can1") == 0) channels_mask = 0x02;
            else if (strcmp(ch, "both") == 0) channels_mask = 0x03;
            else if (strcmp(ch, "none") == 0) channels_mask = 0x00;
            else { usage(argv[0]); return 1; }
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (do_uninstall) return svc_uninstall();
    if (do_install)   return svc_install(bitrate, channels_mask, device);
    if (do_start)     return svc_start();
    if (do_stop)      return svc_stop();

    /* Standalone mode */
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    wincan_server_run(bitrate, channels_mask, device);
    return 0;
}

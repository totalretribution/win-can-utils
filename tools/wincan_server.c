#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winsvc.h>
#include <winreg.h>
#include <wincan/wincan.h>

/* Forward declarations — defined in src/wincan_server.c */
void wincan_server_run(int bitrate_kbps, int channels_mask, int device_index);
void wincan_server_stop(void);
void wincan_server_set_logger(void (*fn)(int level, const char *msg));

#define LOG_INFO  0
#define LOG_WARN  1
#define LOG_ERROR 2

/* =========================================================================
 * Service identity
 * ====================================================================== */

#define SVC_NAME         L"WinCANServer"
#define SVC_DISPLAY_NAME L"WinCAN Server"
#define SVC_DESCRIPTION  L"Shares WinUSB CAN adapters over localhost TCP. " \
                          L"Provides vcan0/vcan1 virtual loopback channels."

/* Event source registry path */
#define EVT_SOURCE_KEY \
    L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\WinCANServer"

/* =========================================================================
 * Event Log
 * ====================================================================== */

static HANDLE g_event_log = NULL;

static void service_logger(int level, const char *msg)
{
    if (!g_event_log) return;

    WORD type;
    switch (level) {
        case LOG_WARN:  type = EVENTLOG_WARNING_TYPE;     break;
        case LOG_ERROR: type = EVENTLOG_ERROR_TYPE;       break;
        default:        type = EVENTLOG_INFORMATION_TYPE; break;
    }

    wchar_t wmsg[512];
    MultiByteToWideChar(CP_UTF8, 0, msg, -1, wmsg,
                        sizeof(wmsg) / sizeof(wchar_t));
    LPCWSTR strings[1] = { wmsg };
    ReportEventW(g_event_log, type, 0, 1, NULL, 1, 0, strings, NULL);
}

static void register_event_source_key(const wchar_t *exe_path)
{
    HKEY hk;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, EVT_SOURCE_KEY,
                        0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL)
            != ERROR_SUCCESS)
        return;

    RegSetValueExW(hk, L"EventMessageFile", 0, REG_EXPAND_SZ,
                   (const BYTE *)exe_path,
                   (DWORD)((wcslen(exe_path) + 1) * sizeof(wchar_t)));
    DWORD types = EVENTLOG_INFORMATION_TYPE |
                  EVENTLOG_WARNING_TYPE     |
                  EVENTLOG_ERROR_TYPE;
    RegSetValueExW(hk, L"TypesSupported", 0, REG_DWORD,
                   (const BYTE *)&types, sizeof(types));
    RegCloseKey(hk);
}

static void unregister_event_source_key(void)
{
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, EVT_SOURCE_KEY);
}

/* =========================================================================
 * Service status helpers
 * ====================================================================== */

static SERVICE_STATUS        g_svc_status;
static SERVICE_STATUS_HANDLE g_svc_status_handle;

static void svc_set_status(DWORD state, DWORD exit_code, DWORD wait_hint)
{
    static DWORD checkpoint = 1;
    g_svc_status.dwCurrentState  = state;
    g_svc_status.dwWin32ExitCode = exit_code;
    g_svc_status.dwWaitHint      = wait_hint;
    /* Increment checkpoint for pending states so SCM knows we're alive */
    g_svc_status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_svc_status_handle, &g_svc_status);
}

/* =========================================================================
 * Service control handler
 * ====================================================================== */

static VOID WINAPI svc_ctrl_handler(DWORD ctrl)
{
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
        svc_set_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        wincan_server_stop();
        break;
    case SERVICE_CONTROL_INTERROGATE:
        /* Respond with current status so SCM knows we're alive */
        SetServiceStatus(g_svc_status_handle, &g_svc_status);
        break;
    default:
        break;
    }
}

/* =========================================================================
 * ServiceMain — entry point called by SCM
 * ====================================================================== */

static VOID WINAPI svc_main(DWORD argc, LPWSTR *argv)
{
    g_svc_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwControlsAccepted        = SERVICE_ACCEPT_STOP;
    g_svc_status.dwServiceSpecificExitCode = 0;
    g_svc_status.dwCheckPoint             = 0;
    g_svc_status.dwWaitHint               = 0;

    g_svc_status_handle = RegisterServiceCtrlHandlerW(SVC_NAME, svc_ctrl_handler);
    if (!g_svc_status_handle) return;

    svc_set_status(SERVICE_START_PENDING, NO_ERROR, 3000);

    /* Open Event Log and wire up logger */
    g_event_log = RegisterEventSourceW(NULL, SVC_NAME);
    wincan_server_set_logger(service_logger);

    /* Parse config args from service binary path */
    int bitrate       = 250;
    int channels_mask = 0x03;
    int device        = 0;

    for (DWORD i = 1; i < argc; i++) {
        if      (wcscmp(argv[i], L"--run-service") == 0) { /* skip */ }
        else if (wcscmp(argv[i], L"-b") == 0 && i+1 < argc)
            bitrate = _wtoi(argv[++i]);
        else if (wcscmp(argv[i], L"-d") == 0 && i+1 < argc)
            device = _wtoi(argv[++i]);
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

    svc_set_status(SERVICE_STOP_PENDING, NO_ERROR, 1000);

    if (g_event_log) {
        DeregisterEventSource(g_event_log);
        g_event_log = NULL;
    }

    svc_set_status(SERVICE_STOPPED, NO_ERROR, 0);
}

/* =========================================================================
 * Service management (require admin)
 * ====================================================================== */

static int svc_install(int bitrate, int channels_mask, int device)
{
    wchar_t exe_path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe_path, MAX_PATH)) {
        fprintf(stderr, "GetModuleFileNameW failed: %lu\n", GetLastError());
        return 1;
    }

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

    /* Description */
    SERVICE_DESCRIPTIONW desc = { (LPWSTR)SVC_DESCRIPTION };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    /* Failure actions: restart after 1s, 5s, 30s */
    SC_ACTION actions[3] = {
        { SC_ACTION_RESTART,  1000  },
        { SC_ACTION_RESTART,  5000  },
        { SC_ACTION_RESTART,  30000 },
    };
    SERVICE_FAILURE_ACTIONSW fa;
    memset(&fa, 0, sizeof(fa));
    fa.dwResetPeriod = 86400;  /* reset failure count after 1 day */
    fa.cActions      = 3;
    fa.lpsaActions   = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    /* Register Event Log source */
    register_event_source_key(exe_path);

    printf("Service installed successfully.\n");
    printf("Start now with:  wincan_server --start\n");
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
        for (int i = 0; i < 50; i++) {
            QueryServiceStatus(svc, &st);
            if (st.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(100);
        }
    }

    int rc = 0;
    if (!DeleteService(svc)) {
        fprintf(stderr, "DeleteService failed: %lu\n", GetLastError());
        rc = 1;
    } else {
        unregister_event_source_key();
        printf("Service uninstalled.\n");
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return rc;
}

static int svc_start(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed: %lu (run as administrator)\n",
                GetLastError());
        return 1;
    }

    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME,
                                 SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        fprintf(stderr, "Service not found. Run --install first.\n");
        CloseServiceHandle(scm);
        return 1;
    }

    int rc = 0;
    if (!StartServiceW(svc, 0, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING)
            fprintf(stderr, "Service is already running.\n");
        else
            fprintf(stderr, "StartService failed: %lu\n", err);
        rc = 1;
    } else {
        printf("Service started.\n");
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return rc;
}

static int svc_stop(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed: %lu (run as administrator)\n",
                GetLastError());
        return 1;
    }

    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME,
                                 SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        fprintf(stderr, "Service not found.\n");
        CloseServiceHandle(scm);
        return 1;
    }

    int rc = 0;
    SERVICE_STATUS st;
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_NOT_ACTIVE)
            fprintf(stderr, "Service is not running.\n");
        else
            fprintf(stderr, "ControlService failed: %lu\n", err);
        rc = 1;
    } else {
        printf("Service stopped.\n");
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return rc;
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
        "  --uninstall  Stop and remove the service\n"
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
    /* --run-service: launched by SCM, hand off to dispatcher immediately */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--run-service") == 0) {
            SERVICE_TABLE_ENTRYW table[] = {
                { (LPWSTR)SVC_NAME, svc_main },
                { NULL, NULL }
            };
            StartServiceCtrlDispatcherW(table);
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

/*
 * Kobo BLE Remote — NickelHook plugin
 *
 * Listens on a Unix socket for page turn commands from the standalone
 * ble_peripheral binary. Calls ReadingView::nextPage()/prevPage()
 * directly (orientation-independent) and keeps BT alive via
 * BluetoothHeartbeat + PowerManager.
 *
 * Architecture:
 *   Apple Watch → BLE GATT write → ble_peripheral → Unix socket → this plugin → Nickel
 *
 * Process management:
 *   - Reaper thread blocks on waitpid(), respawns ble_peripheral on death
 *   - PowerManager::resumed() signal triggers BT health check on wake
 *   - turnBluetoothOn hook spawns ble_peripheral when user enables BT
 */

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

#include <NickelHook.h>

#include <QMetaObject>
#include <QObject>
#include <QWidget>
#include <QEvent>

#include "resume_handler.h"


/* ================================================================
 * Nickel symbol pointers (resolved by NickelHook via nh_dlsym)
 * ================================================================ */

static void *(*MainWindowController_sharedInstance)();
static QWidget *(*MainWindowController_currentView)(void *);

static void *(*BluetoothHeartbeat_ctor)(void *, long long);
static void (*BluetoothHeartbeat_beat)(void *);

static QObject *(*PowerManager_sharedInstance)();
static int (*PowerManager_filter)(QObject *, QObject *, QEvent *);
static QEvent::Type (*TimeEvent_eventType)();

static QObject *(*N3PowerWorkflowManager_sharedInstance)();

/* Original turnBluetoothOn — stored by nh_hook */
static void (*orig_turnBluetoothOn)(void *);

/* ================================================================
 * Constants
 * ================================================================ */

#define CMD_NEXT_PAGE     0x01
#define CMD_PREV_PAGE     0x02
#define CMD_HEALTH_CHECK  0xFF
#define EXIT_HEALTH_FAIL  42

#define SOCKET_PATH    "/tmp/kobo-ble-remote.sock"
#define BLE_PERIPHERAL "/usr/local/Kobo/ble_peripheral"

/* BluetoothHeartbeat instance (placement new, like kobo-btpt) */
static char g_bt_heartbeat_buf[1024];
static void *g_bt_heartbeat = NULL;

/* ble_peripheral subprocess PID */
static volatile pid_t g_ble_pid = -1;

/* Socket server */
static int g_sock_fd = -1;
static volatile int g_running = 1;
static volatile int g_ble_client_fd = -1;  /* fd for writing back to ble_peripheral */

/* ================================================================
 * Nickel integration: page turning + power management
 * ================================================================ */

static void invokeMainWindowController(const char *method)
{
    void *mwc = MainWindowController_sharedInstance();
    if (!mwc) {
        nh_log("invokeMainWindowController: no MWC");
        return;
    }
    QWidget *cv = MainWindowController_currentView(mwc);
    if (!cv) {
        nh_log("invokeMainWindowController: no current view");
        return;
    }
    if (cv->objectName() == "ReadingView") {
        nh_log("invokeMainWindowController: %s", method);
        QMetaObject::invokeMethod(cv, method, Qt::QueuedConnection);
    } else {
        nh_log("invokeMainWindowController: not ReadingView (%s)",
               cv->objectName().toStdString().c_str());
    }
}

static void beat_bluetooth_heartbeat()
{
    if (!g_bt_heartbeat) {
        g_bt_heartbeat = g_bt_heartbeat_buf;
        new(g_bt_heartbeat) QObject();
        BluetoothHeartbeat_ctor(g_bt_heartbeat, 0);
    }
    BluetoothHeartbeat_beat(g_bt_heartbeat);
}

static void update_power_manager()
{
    QObject *pm = PowerManager_sharedInstance();
    if (!pm) {
        nh_log("update_power_manager: no PowerManager");
        return;
    }
    QEvent timeEvent(TimeEvent_eventType());
    PowerManager_filter(pm, pm, &timeEvent);
}

static void on_ble_activity()
{
    beat_bluetooth_heartbeat();
    update_power_manager();
}

/* ================================================================
 * ble_peripheral subprocess management
 * ================================================================ */

static void spawn_ble_peripheral()
{
    pid_t pid = fork();
    if (pid < 0) {
        nh_log("fork failed: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        /* Redirect stdout/stderr to log file, then exec */
        int fd = open("/tmp/ble_peripheral.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        setenv("LD_LIBRARY_PATH", "/usr/lib", 1);
        execl(BLE_PERIPHERAL, BLE_PERIPHERAL, (char *)NULL);
        _exit(127);
    }

    g_ble_pid = pid;
    nh_log("spawned ble_peripheral (pid=%d)", pid);
}

/* ================================================================
 * ResumeHandler — connected to PowerManager::resumed()
 * ================================================================ */

void ResumeHandler::onResume()
{
    nh_log("resume detected (PowerManager::resumed)");
    int fd = g_ble_client_fd;
    if (fd >= 0) {
        unsigned char cmd = CMD_HEALTH_CHECK;
        ssize_t n = write(fd, &cmd, 1);
        if (n == 1)
            nh_log("sent health check to ble_peripheral");
        else
            nh_log("health check write failed");
    } else {
        nh_log("no ble_peripheral connection for health check");
    }
}

/* ================================================================
 * Reaper thread — blocks on waitpid(), respawns on child death
 * ================================================================ */

static void *reaper_thread(void *)
{
    /* Initial delay — wait for BT stack to be ready */
    sleep(10);

    nh_log("reaper: spawning ble_peripheral");
    spawn_ble_peripheral();

    int backoff = 5;

    while (g_running) {
        int status;
        pid_t pid = waitpid(g_ble_pid, &status, 0);  /* blocks */
        if (pid <= 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) {
                /* No child — wait a bit and check again */
                sleep(1);
                continue;
            }
            break;
        }

        if (pid == g_ble_pid) {
            int health_exit = 0;
            if (WIFEXITED(status)) {
                int code = WEXITSTATUS(status);
                nh_log("ble_peripheral exited (code=%d)", code);
                health_exit = (code == EXIT_HEALTH_FAIL);
            } else if (WIFSIGNALED(status)) {
                nh_log("ble_peripheral killed (signal=%d)", WTERMSIG(status));
            }
            g_ble_pid = -1;

            if (health_exit) {
                nh_log("health check exit — respawning immediately");
                backoff = 5;
            } else {
                nh_log("respawning in %d seconds", backoff);
                sleep(backoff);
                if (backoff < 30)
                    backoff = backoff * 2;
            }
            spawn_ble_peripheral();
        }
    }

    return NULL;
}

/* ================================================================
 * Hook: turnBluetoothOn — spawn ble_peripheral when user enables BT
 * ================================================================ */

extern "C" __attribute__((visibility("default")))
void nh_hook_turnBluetoothOn(void *self)
{
    nh_log("turnBluetoothOn hook called");

    /* Call the original implementation */
    if (orig_turnBluetoothOn)
        orig_turnBluetoothOn(self);

    /* Spawn ble_peripheral after a delay (BT stack needs time) */
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, [](void *) -> void * {
        sleep(5);
        spawn_ble_peripheral();
        return NULL;
    }, NULL);
    pthread_attr_destroy(&attr);
}

/* ================================================================
 * Unix socket server — receives commands from ble_peripheral
 * ================================================================ */

static void handle_command(unsigned char cmd)
{
    on_ble_activity();

    if (cmd == CMD_NEXT_PAGE) {
        invokeMainWindowController("nextPage");
    } else if (cmd == CMD_PREV_PAGE) {
        invokeMainWindowController("prevPage");
    } else {
        nh_log("unknown command: 0x%02x", cmd);
    }
}

static void *socket_server_thread(void *)
{
    nh_log("socket server started");

    /* Remove stale socket */
    unlink(SOCKET_PATH);

    g_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_sock_fd < 0) {
        nh_log("socket: %s", strerror(errno));
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(g_sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        nh_log("bind: %s", strerror(errno));
        close(g_sock_fd);
        g_sock_fd = -1;
        return NULL;
    }

    chmod(SOCKET_PATH, 0777);

    if (listen(g_sock_fd, 1) < 0) {
        nh_log("listen: %s", strerror(errno));
        close(g_sock_fd);
        g_sock_fd = -1;
        return NULL;
    }

    nh_log("listening on %s", SOCKET_PATH);

    while (g_running) {
        int client_fd = accept(g_sock_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            nh_log("accept: %s", strerror(errno));
            break;
        }

        nh_log("ble_peripheral connected");
        g_ble_client_fd = client_fd;

        unsigned char buf[64];
        while (g_running) {
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                if (n < 0)
                    nh_log("read: %s", strerror(errno));
                else
                    nh_log("ble_peripheral disconnected");
                break;
            }
            for (ssize_t i = 0; i < n; i++) {
                handle_command(buf[i]);
            }
        }

        g_ble_client_fd = -1;
        close(client_fd);
    }

    close(g_sock_fd);
    g_sock_fd = -1;
    unlink(SOCKET_PATH);
    return NULL;
}

/* ================================================================
 * NickelHook init
 * ================================================================ */

static int ble_remote_init()
{
    nh_log("init: starting");

    /* Connect to N3PowerWorkflowManager::resumedFromSleep() for resume detection.
     * This fires after the UI is back up (not during kernel resume like
     * PowerManager::resumed()), so the BT stack is ready by then. */
    ResumeHandler *rh = new ResumeHandler();

    QObject *pwm = N3PowerWorkflowManager_sharedInstance();
    if (pwm) {
        bool ok = QObject::connect(pwm, SIGNAL(resumedFromSleep()), rh, SLOT(onResume()));
        nh_log("N3PowerWorkflowManager::resumedFromSleep() connect: %s", ok ? "OK" : "FAILED");
    } else {
        nh_log("WARNING: no N3PowerWorkflowManager for resume detection");
    }

    /* Start socket server thread */
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, socket_server_thread, NULL);

    /* Start reaper thread (blocks on waitpid, respawns on death) */
    pthread_t rp;
    pthread_create(&rp, &attr, reaper_thread, NULL);
    pthread_attr_destroy(&attr);

    return 0;
}

/* ================================================================
 * NickelHook registration
 * ================================================================ */

static struct nh_info ble_remote_info = {
    .name            = "KoboBleRemote",
    .desc            = "BLE page turning for Apple Watch",
    .uninstall_flag  = "/mnt/onboard/.kobo-ble-remote-uninstall",
    .uninstall_xflag = NULL,
    .failsafe_delay  = 10,
};

static struct nh_hook ble_remote_hook[] = {
    {
        .sym     = "_ZN12iMX6Netronix15turnBluetoothOnEv",
        .sym_new = "nh_hook_turnBluetoothOn",
        .lib     = "libnickel.so.1.0.0",
        .out     = nh_symoutptr(orig_turnBluetoothOn),
        .desc    = "iMX6Netronix::turnBluetoothOn()",
        .optional = true,
    },
    {0},
};

static struct nh_dlsym ble_remote_dlsym[] = {
    {
        .name = "_ZN20MainWindowController14sharedInstanceEv",
        .out  = nh_symoutptr(MainWindowController_sharedInstance),
        .desc = "MainWindowController::sharedInstance()",
    },
    {
        .name = "_ZNK20MainWindowController11currentViewEv",
        .out  = nh_symoutptr(MainWindowController_currentView),
        .desc = "MainWindowController::currentView()",
    },
    {
        .name = "_ZN18BluetoothHeartbeatC1Ex",
        .out  = nh_symoutptr(BluetoothHeartbeat_ctor),
        .desc = "BluetoothHeartbeat::BluetoothHeartbeat(long long)",
    },
    {
        .name = "_ZN18BluetoothHeartbeat4beatEv",
        .out  = nh_symoutptr(BluetoothHeartbeat_beat),
        .desc = "BluetoothHeartbeat::beat()",
    },
    {
        .name = "_ZN12PowerManager14sharedInstanceEv",
        .out  = nh_symoutptr(PowerManager_sharedInstance),
        .desc = "PowerManager::sharedInstance()",
    },
    {
        .name = "_ZN12PowerManager6filterEP7QObjectP6QEvent",
        .out  = nh_symoutptr(PowerManager_filter),
        .desc = "PowerManager::filter(QObject*, QEvent*)",
    },
    {
        .name = "_ZN9TimeEvent9eventTypeEv",
        .out  = nh_symoutptr(TimeEvent_eventType),
        .desc = "TimeEvent::eventType()",
    },
    {
        .name = "_ZN22N3PowerWorkflowManager14sharedInstanceEv",
        .out  = nh_symoutptr(N3PowerWorkflowManager_sharedInstance),
        .desc = "N3PowerWorkflowManager::sharedInstance()",
    },
    {0},
};

NickelHook(
    .init  = ble_remote_init,
    .info  = &ble_remote_info,
    .hook  = ble_remote_hook,
    .dlsym = ble_remote_dlsym,
)

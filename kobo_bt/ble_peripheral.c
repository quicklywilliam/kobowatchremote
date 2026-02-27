/*
 * Kobo Color BLE Peripheral - Uses MediaTek BT middleware via dlopen
 *
 * Registers a GATT server with a page-turn service, then starts BLE
 * legacy advertising (ADV_IND) via start_advertising_set so that
 * Apple Watch (which only scans legacy PDUs) can discover and connect.
 *
 * On write to the characteristic:
 *   0x01 = next page, 0x02 = previous page
 * These are sent over a Unix socket to the NickelHook plugin which
 * calls ReadingView::nextPage()/prevPage() directly.
 *
 * The main thread blocks on the socket waiting for commands from the
 * plugin. On device resume, the plugin sends CMD_HEALTH_CHECK (0xFF);
 * ble_peripheral tests the BT stack and exits if stale (reaper respawns).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

/* Type definitions matching MediaTek BT MW */
typedef unsigned char  UINT8;
typedef unsigned short UINT16;
typedef unsigned int   UINT32;
typedef char           CHAR;
typedef int            INT32;
typedef void           VOID;

#define BT_GATT_MAX_UUID_LEN  37
#define BT_GATT_MAX_ATTR_LEN  600
#define MAX_BDADDR_LEN        18

/* GATTS callback event types */
typedef enum {
    BT_GATTS_REGISTER_SERVER = 0,
    BT_GATTS_CONNECT,
    BT_GATTS_DISCONNECT,
    BT_GATTS_GET_RSSI_DONE,
    BT_GATTS_EVENT_MAX
} BT_GATTS_EVENT_T;

typedef enum {
    BT_GATTS_START_SRVC = 0,
    BT_GATTS_STOP_SRVC,
    BT_GATTS_DEL_SRVC,
    BT_GATTS_SRVC_OP_MAX
} BT_GATTS_SRVC_OP_TYPE_T;

/* GATTS callback result structures */
typedef struct {
    CHAR uuid[BT_GATT_MAX_UUID_LEN];
    UINT8 inst_id;
} BT_GATT_ID_T;

typedef struct {
    BT_GATT_ID_T id;
    UINT8 is_primary;
} BT_GATTS_SRVC_ID_T;

typedef struct {
    INT32 server_if;
    CHAR  app_uuid[BT_GATT_MAX_UUID_LEN];
} BT_GATTS_REG_SERVER_RST_T;

typedef struct {
    INT32 conn_id;
    INT32 server_if;
    CHAR  btaddr[MAX_BDADDR_LEN];
} BT_GATTS_CONNECT_RST_T;

typedef struct {
    INT32 server_if;
    BT_GATTS_SRVC_ID_T srvc_id;
    INT32 srvc_handle;
} BT_GATTS_ADD_SRVC_RST_T;

typedef struct {
    INT32 server_if;
    INT32 srvc_handle;
    INT32 incl_srvc_handle;
} BT_GATTS_ADD_INCL_RST_T;

typedef struct {
    INT32 server_if;
    CHAR  uuid[BT_GATT_MAX_UUID_LEN];
    INT32 srvc_handle;
    INT32 char_handle;
} BT_GATTS_ADD_CHAR_RST_T;

typedef struct {
    INT32 server_if;
    CHAR  uuid[BT_GATT_MAX_UUID_LEN];
    INT32 srvc_handle;
    INT32 descr_handle;
} BT_GATTS_ADD_DESCR_RST_T;

typedef struct {
    INT32 server_if;
    INT32 srvc_handle;
} BT_GATTS_SRVC_RST_T;

typedef struct {
    INT32 conn_id;
    INT32 trans_id;
    CHAR  btaddr[MAX_BDADDR_LEN];
    INT32 attr_handle;
    INT32 offset;
    UINT8 is_long;
} BT_GATTS_REQ_READ_RST_T;

typedef struct {
    INT32 conn_id;
    INT32 trans_id;
    CHAR  btaddr[MAX_BDADDR_LEN];
    INT32 attr_handle;
    INT32 offset;
    INT32 length;
    UINT8 need_rsp;
    UINT8 is_prep;
    UINT8 value[BT_GATT_MAX_ATTR_LEN];
} BT_GATTS_REQ_WRITE_RST_T;

/* GATTS callback function pointer types */
typedef VOID (*BtAppGATTSEventCbk)(BT_GATTS_EVENT_T event, void* pv_tag);
typedef VOID (*BtAppGATTSRegServerCbk)(BT_GATTS_REG_SERVER_RST_T *rst, void* pv_tag);
typedef VOID (*BtAppGATTSAddSrvcCbk)(BT_GATTS_ADD_SRVC_RST_T *rst, void* pv_tag);
typedef VOID (*BtAppGATTSAddInclCbk)(BT_GATTS_ADD_INCL_RST_T *rst, void* pv_tag);
typedef VOID (*BtAppGATTSAddCharCbk)(BT_GATTS_ADD_CHAR_RST_T *rst, void* pv_tag);
typedef VOID (*BtAppGATTSAddDescCbk)(BT_GATTS_ADD_DESCR_RST_T *rst, void* pv_tag);
typedef VOID (*BtAppGATTSOpSrvcCbk)(BT_GATTS_SRVC_OP_TYPE_T op, BT_GATTS_SRVC_RST_T *rst, void* pv_tag);
typedef VOID (*BtAppGATTSReqReadCbk)(BT_GATTS_REQ_READ_RST_T *rst, void* pv_tag);
typedef VOID (*BtAppGATTSReqWriteCbk)(BT_GATTS_REQ_WRITE_RST_T *rst, void* pv_tag);
typedef VOID (*BtAppGATTSIndSentCbk)(INT32 conn_id, INT32 status, void* pv_tag);

typedef struct {
    BtAppGATTSEventCbk     bt_gatts_event_cb;
    BtAppGATTSRegServerCbk bt_gatts_reg_server_cb;
    BtAppGATTSAddSrvcCbk   bt_gatts_add_srvc_cb;
    BtAppGATTSAddInclCbk   bt_gatts_add_incl_cb;
    BtAppGATTSAddCharCbk   bt_gatts_add_char_cb;
    BtAppGATTSAddDescCbk   bt_gatts_add_desc_cb;
    BtAppGATTSOpSrvcCbk    bt_gatts_op_srvc_cb;
    BtAppGATTSReqReadCbk   bt_gatts_req_read_cb;
    BtAppGATTSReqWriteCbk  bt_gatts_req_write_cb;
    BtAppGATTSIndSentCbk   bt_gatts_ind_sent_cb;
} MTKRPCAPI_BT_APP_GATTS_CB_FUNC_T;

/* GATTC callback types - all 16 slots must be populated */
typedef enum {
    BT_GATTC_REGISTER_APP = 0,
    BT_GATTC_SCAN_RESULT,
    BT_GATTC_CONNECT,
    BT_GATTC_DISCONNECT,
    BT_GATTC_EVENT_MAX
} BT_GATTC_EVENT_T;

typedef struct {
    INT32 client_if;
    CHAR  app_uuid[BT_GATT_MAX_UUID_LEN];
} BT_GATTC_REG_CLIENT_RST_T;

typedef VOID (*BtAppGATTCEventCbk)(BT_GATTC_EVENT_T event, void* pv_tag);
typedef VOID (*BtAppGATTCRegClientCbk)(BT_GATTC_REG_CLIENT_RST_T *rst, void* pv_tag);
typedef VOID (*BtAppGATTCGenericCbk)(void *rst, void* pv_tag);

typedef struct {
    BtAppGATTCEventCbk       bt_gattc_event_cb;
    BtAppGATTCRegClientCbk   bt_gattc_reg_client_cb;
    BtAppGATTCGenericCbk     bt_gattc_scan_cb;
    BtAppGATTCGenericCbk     bt_gattc_get_gatt_db_cb;
    BtAppGATTCGenericCbk     bt_gattc_get_reg_noti_cb;
    BtAppGATTCGenericCbk     bt_gattc_notify_cb;
    BtAppGATTCGenericCbk     bt_gattc_read_char_cb;
    BtAppGATTCGenericCbk     bt_gattc_write_char_cb;
    BtAppGATTCGenericCbk     bt_gattc_read_desc_cb;
    BtAppGATTCGenericCbk     bt_gattc_write_desc_cb;
    BtAppGATTCGenericCbk     bt_gattc_scan_filt_param_cb;
    BtAppGATTCGenericCbk     bt_gattc_scan_filt_status_cb;
    BtAppGATTCGenericCbk     bt_gattc_scan_filt_cfg_cb;
    BtAppGATTCGenericCbk     bt_gattc_adv_cb;
    BtAppGATTCGenericCbk     bt_gattc_mtu_config_cb;
    BtAppGATTCGenericCbk     bt_gattc_phy_updated_cb;
} MTKRPCAPI_BT_APP_GATTC_CB_FUNC_T;

/* Function pointer types for dlsym */
typedef INT32 (*fn_service_init)(void);
typedef INT32 (*fn_rpc_init)(void);
typedef INT32 (*fn_rpc_start)(void);
typedef INT32 (*fn_gatts_base_init)(MTKRPCAPI_BT_APP_GATTS_CB_FUNC_T *func, void* pv_tag);
typedef INT32 (*fn_gatts_register_server)(CHAR *app_uuid);
typedef INT32 (*fn_gatts_unregister_server)(INT32 server_if);
typedef INT32 (*fn_gatts_add_service)(INT32 server_if, CHAR *service_uuid, UINT8 is_primary, INT32 number);
typedef INT32 (*fn_gatts_add_char)(INT32 server_if, INT32 service_handle, CHAR *uuid, INT32 properties, INT32 permissions);
typedef INT32 (*fn_gatts_add_desc)(INT32 server_if, INT32 service_handle, CHAR *uuid, INT32 permissions);
typedef INT32 (*fn_gatts_start_service)(INT32 server_if, INT32 service_handle, INT32 transport);
typedef INT32 (*fn_gatts_stop_service)(INT32 server_if, INT32 service_handle);
typedef INT32 (*fn_gatts_send_response)(INT32 conn_id, INT32 trans_id, INT32 status, INT32 handle, CHAR *p_value, INT32 value_len, INT32 auth_req);
typedef INT32 (*fn_gattc_base_init)(MTKRPCAPI_BT_APP_GATTC_CB_FUNC_T *func, void* pv_tag);
typedef INT32 (*fn_gattc_register_app)(CHAR *app_uuid);
typedef INT32 (*fn_gattc_multi_adv_enable)(INT32 client_if, INT32 min_interval, INT32 max_interval, INT32 adv_type, INT32 chnl_map, INT32 tx_power, INT32 timeout_s);
typedef INT32 (*fn_gattc_multi_adv_setdata)(INT32 client_if, UINT8 set_scan_rsp, UINT8 include_name, UINT8 include_txpower, INT32 appearance, INT32 manufacturer_len, CHAR* manufacturer_data, INT32 service_data_len, CHAR* service_data, INT32 service_uuid_len, CHAR* service_uuid);
typedef INT32 (*fn_gattc_multi_adv_disable)(INT32 client_if);

/* BLE 5.0 Advertising Set API structs */
struct __attribute__((packed)) AdvSetParams {
    UINT16 advertising_event_properties;
    UINT32 min_interval;
    UINT32 max_interval;
    UINT8  channel_map;
    UINT8  tx_power;
    UINT8  primary_advertising_phy;
    UINT8  secondary_advertising_phy;
};

struct AdvSetData {
    INT32 len;
    UINT8 data[1024];
};

struct __attribute__((packed)) PeriodicAdvParams {
    UINT8  enable;
    UINT8  pad;
    UINT16 min_interval;
    UINT16 max_interval;
    UINT16 properties;
};

typedef INT32 (*fn_gattc_start_adv_set)(INT32 client_if,
    struct AdvSetParams *adv_params,
    struct AdvSetData *adv_data,
    struct AdvSetData *scan_rsp_data,
    struct PeriodicAdvParams *periodic_params,
    struct AdvSetData *periodic_data,
    UINT16 duration,
    UINT8 max_ext_adv_events);
typedef INT32 (*fn_gattc_stop_adv_set)(INT32 client_if);

/* Globals */
static volatile int g_running = 1;
static volatile int g_health_fail = 0;
static volatile INT32 g_server_if = -1;
static volatile INT32 g_client_if = -1;
static volatile INT32 g_srvc_handle = -1;
static volatile INT32 g_char_handle = -1;
static volatile INT32 g_descr_handle = -1;
static volatile INT32 g_conn_id = -1;
static volatile INT32 g_srvc_started = -1; /* set to 0+ by op_srvc_cb */

/* Wait for a volatile INT32 to change from -1, polling every 50ms.
 * Returns 1 on success, 0 on timeout. */
static int wait_for(volatile INT32 *var, int timeout_ms) {
    struct timespec ts = { 0, 50000000 }; /* 50ms */
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 50) {
        if (*var >= 0) return 1;
        nanosleep(&ts, NULL);
    }
    return *var >= 0;
}

/* Page turn commands */
#define CMD_NEXT_PAGE  0x01
#define CMD_PREV_PAGE  0x02

/* Health check command (sent by plugin on resume) */
#define CMD_HEALTH_CHECK 0xFF
#define EXIT_HEALTH_FAIL 42

static UINT8 g_last_cmd = 0;

/* Our custom service UUID */
#define SERVICE_UUID    "0b278e49-7f56-4788-a1bb-4624e0d64b46"
#define CHAR_PAGE_UUID  "5257acb0-be4d-4cf1-af8f-cbdb67bf998a"
#define CCCD_UUID       "00002902-0000-1000-8000-00805f9b34fb"

/* BLE GATT properties and permissions */
#define GATT_PROP_READ       0x02
#define GATT_PROP_WRITE      0x08
#define GATT_PROP_NOTIFY     0x10
#define GATT_PERM_READ       0x01
#define GATT_PERM_WRITE      0x10

/* Function pointers stored globally for use in callbacks */
static fn_gatts_send_response g_send_response = NULL;
static fn_gatts_stop_service g_stop_service = NULL;
static fn_gattc_start_adv_set g_start_adv_set = NULL;
static fn_gattc_stop_adv_set g_stop_adv_set = NULL;
static fn_gattc_multi_adv_enable g_multi_adv_enable = NULL;
static fn_gattc_multi_adv_setdata g_multi_adv_setdata = NULL;
static fn_gattc_multi_adv_disable g_multi_adv_disable = NULL;

/* Library handle for reinit */
static void *g_lib = NULL;

/* Unix socket to NickelHook plugin */
#define SOCKET_PATH "/tmp/kobo-ble-remote.sock"
static int g_sock_fd = -1;

/* --- Socket IPC to NickelHook plugin --- */

static void send_command(UINT8 cmd) {
    if (g_sock_fd < 0) return;
    ssize_t n = write(g_sock_fd, &cmd, 1);
    if (n != 1) {
        printf("[SOCK] write failed: %s\n", n < 0 ? strerror(errno) : "short write");
        close(g_sock_fd);
        g_sock_fd = -1;
    }
}

/* --- Advertising helpers --- */

static void build_adv_data(struct AdvSetData *ad) {
    memset(ad, 0, sizeof(*ad));
    UINT8 *p = ad->data;
    *p++ = 0x02; *p++ = 0x01; *p++ = 0x06;
    *p++ = 0x11; *p++ = 0x07;
    *p++ = 0x46; *p++ = 0x4b; *p++ = 0xd6; *p++ = 0xe0;
    *p++ = 0x24; *p++ = 0x46; *p++ = 0xbb; *p++ = 0xa1;
    *p++ = 0x88; *p++ = 0x47; *p++ = 0x56; *p++ = 0x7f;
    *p++ = 0x49; *p++ = 0x8e; *p++ = 0x27; *p++ = 0x0b;
    ad->len = (INT32)(p - ad->data);
}

static void build_scan_rsp(struct AdvSetData *sr) {
    memset(sr, 0, sizeof(*sr));
    UINT8 *p = sr->data;
    *p++ = 18; *p++ = 0x09;
    memcpy(p, "Kobo Libra Colour", 17);
    p += 17;
    sr->len = (INT32)(p - sr->data);
}

static INT32 start_legacy_advertising(void) {
    if (g_client_if < 0) return -1;
    INT32 ret;

    if (g_multi_adv_enable) {
        printf("Priming with multi_adv (client_if=%d)...\n", g_client_if);
        ret = g_multi_adv_enable(g_client_if, 96, 160, 0, 7, 7, 0);
        printf("multi_adv_enable: ret=%d\n", ret);
        if (ret == 0) {
            usleep(200000);
            g_multi_adv_setdata(g_client_if,
                0, 1, 0, 0, 0, NULL, 0, NULL,
                strlen(SERVICE_UUID), SERVICE_UUID);
            g_multi_adv_setdata(g_client_if,
                1, 1, 0, 0, 0, NULL, 0, NULL,
                strlen(SERVICE_UUID), SERVICE_UUID);
            usleep(200000);
            g_multi_adv_disable(g_client_if);
            usleep(200000);
        }
    }

    if (!g_start_adv_set) return -1;

    struct AdvSetParams ap;
    memset(&ap, 0, sizeof(ap));
    ap.advertising_event_properties = 0x0013;
    ap.min_interval = 96;
    ap.max_interval = 160;
    ap.channel_map = 7;
    ap.tx_power = 7;
    ap.primary_advertising_phy = 1;
    ap.secondary_advertising_phy = 1;

    struct AdvSetData ad, sr, pd;
    build_adv_data(&ad);
    build_scan_rsp(&sr);
    memset(&pd, 0, sizeof(pd));

    struct PeriodicAdvParams pp;
    memset(&pp, 0, sizeof(pp));

    printf("Starting legacy advertising (client_if=%d)...\n", g_client_if);
    ret = g_start_adv_set(g_client_if, &ap, &ad, &sr, &pp, &pd, 0, 0);
    printf("start_advertising_set: ret=%d\n", ret);
    return ret;
}

/* --- GATTS Callbacks --- */

static void gatts_event_cb(BT_GATTS_EVENT_T event, void* pv_tag) {
    (void)pv_tag;
    printf("[GATTS] Event: %d\n", event);
    if (event == BT_GATTS_CONNECT) {
        printf("[GATTS] *** Device connected! ***\n");
    } else if (event == BT_GATTS_DISCONNECT) {
        printf("[GATTS] *** Device disconnected ***\n");
        g_conn_id = -1;
        printf("[GATTS] Restarting advertising...\n");
        start_legacy_advertising();
    }
}

static void gatts_reg_server_cb(BT_GATTS_REG_SERVER_RST_T *rst, void* pv_tag) {
    (void)pv_tag;
    printf("[GATTS] Server registered: server_if=%d uuid=%s\n", rst->server_if, rst->app_uuid);
    g_server_if = rst->server_if;
}

static void gatts_add_srvc_cb(BT_GATTS_ADD_SRVC_RST_T *rst, void* pv_tag) {
    (void)pv_tag;
    printf("[GATTS] Service added: srvc_handle=%d\n", rst->srvc_handle);
    g_srvc_handle = rst->srvc_handle;
}

static void gatts_add_incl_cb(BT_GATTS_ADD_INCL_RST_T *rst, void* pv_tag) {
    (void)rst; (void)pv_tag;
}

static void gatts_add_char_cb(BT_GATTS_ADD_CHAR_RST_T *rst, void* pv_tag) {
    (void)pv_tag;
    printf("[GATTS] Char added: uuid=%s char_handle=%d\n", rst->uuid, rst->char_handle);
    g_char_handle = rst->char_handle;
}

static void gatts_add_desc_cb(BT_GATTS_ADD_DESCR_RST_T *rst, void* pv_tag) {
    (void)pv_tag;
    printf("[GATTS] Descriptor added: uuid=%s descr_handle=%d\n", rst->uuid, rst->descr_handle);
    g_descr_handle = rst->descr_handle;
}

static void gatts_op_srvc_cb(BT_GATTS_SRVC_OP_TYPE_T op, BT_GATTS_SRVC_RST_T *rst, void* pv_tag) {
    (void)pv_tag;
    const char *ops[] = {"START", "STOP", "DELETE"};
    printf("[GATTS] Service op: %s srvc_handle=%d\n",
           op < 3 ? ops[op] : "?", rst->srvc_handle);
    if (op == 0) /* START */
        g_srvc_started = rst->srvc_handle;
}

static void gatts_req_read_cb(BT_GATTS_REQ_READ_RST_T *rst, void* pv_tag) {
    (void)pv_tag;
    printf("[GATTS] Read request from %s\n", rst->btaddr);
    g_conn_id = rst->conn_id;

    if (g_send_response) {
        CHAR value[1] = { (CHAR)g_last_cmd };
        g_send_response(rst->conn_id, rst->trans_id, 0, rst->attr_handle,
                        value, 1, 0);
    }
}

static void gatts_req_write_cb(BT_GATTS_REQ_WRITE_RST_T *rst, void* pv_tag) {
    (void)pv_tag;
    printf("[GATTS] Write request: len=%d from %s\n", rst->length, rst->btaddr);
    g_conn_id = rst->conn_id;

    if (rst->length >= 1) {
        UINT8 cmd = rst->value[0];
        g_last_cmd = cmd;
        if (cmd == CMD_NEXT_PAGE || cmd == CMD_PREV_PAGE) {
            printf("[PAGE] Sending %s command to plugin\n",
                   cmd == CMD_NEXT_PAGE ? "NEXT" : "PREV");
            send_command(cmd);
        }
    }

    if (rst->need_rsp && g_send_response) {
        g_send_response(rst->conn_id, rst->trans_id, 0, rst->attr_handle,
                        NULL, 0, 0);
    }
}

static void gatts_ind_sent_cb(INT32 conn_id, INT32 status, void* pv_tag) {
    (void)conn_id; (void)status; (void)pv_tag;
}

/* --- GATTC Callbacks (all 16 slots must be filled) --- */

static void gattc_event_cb(BT_GATTC_EVENT_T event, void* pv_tag) {
    (void)event; (void)pv_tag;
}

static void gattc_reg_client_cb(BT_GATTC_REG_CLIENT_RST_T *rst, void* pv_tag) {
    (void)pv_tag;
    printf("[GATTC] Client registered: client_if=%d\n", rst->client_if);
    g_client_if = rst->client_if;
}

static void gattc_generic_cb(void *rst, void* pv_tag) {
    (void)rst; (void)pv_tag;
}

/* --- Signal handlers --- */

static void sighandler(int sig) {
    (void)sig;
    g_running = 0;
}

static void crash_handler(int sig) {
    const char *name = "UNKNOWN";
    if (sig == SIGSEGV) name = "SIGSEGV";
    else if (sig == SIGBUS) name = "SIGBUS";
    else if (sig == SIGABRT) name = "SIGABRT";
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "\n*** CRASH: signal %d (%s) ***\n", sig, name);
    (void)write(STDERR_FILENO, buf, len);
    _exit(128 + sig);
}

/* --- BT stack init/reinit --- */

#define LOAD_SYM_RET(handle, type, name, retval) \
    type name = (type)dlsym(handle, #name); \
    if (!name) { fprintf(stderr, "dlsym(%s): %s\n", #name, dlerror()); return retval; }

static int bt_init(void) {
    INT32 ret;

    /* Reset state */
    g_server_if = -1;
    g_client_if = -1;
    g_srvc_handle = -1;
    g_char_handle = -1;
    g_descr_handle = -1;
    g_conn_id = -1;
    g_srvc_started = -1;
    g_health_fail = 0;
    g_send_response = NULL;
    g_stop_service = NULL;
    g_start_adv_set = NULL;
    g_stop_adv_set = NULL;
    g_multi_adv_enable = NULL;
    g_multi_adv_setdata = NULL;
    g_multi_adv_disable = NULL;

    /* Load the client library */
    g_lib = dlopen("libmtk_bt_service_client.so", RTLD_NOW);
    if (!g_lib) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return -1;
    }

    /* Load all function pointers */
    LOAD_SYM_RET(g_lib, fn_service_init, a_mtk_bt_service_init, -1);
    LOAD_SYM_RET(g_lib, fn_rpc_init, c_rpc_init_mtk_bt_service_client, -1);
    LOAD_SYM_RET(g_lib, fn_rpc_start, c_rpc_start_mtk_bt_service_client, -1);
    LOAD_SYM_RET(g_lib, fn_gattc_base_init, a_mtkapi_bt_gattc_base_init, -1);
    LOAD_SYM_RET(g_lib, fn_gatts_base_init, a_mtkapi_bt_gatts_base_init, -1);
    LOAD_SYM_RET(g_lib, fn_gatts_register_server, a_mtkapi_bt_gatts_register_server, -1);
    LOAD_SYM_RET(g_lib, fn_gatts_add_service, a_mtkapi_bt_gatts_add_service, -1);
    LOAD_SYM_RET(g_lib, fn_gatts_add_char, a_mtkapi_bt_gatts_add_char, -1);
    LOAD_SYM_RET(g_lib, fn_gatts_add_desc, a_mtkapi_bt_gatts_add_desc, -1);
    LOAD_SYM_RET(g_lib, fn_gatts_start_service, a_mtkapi_bt_gatts_start_service, -1);
    LOAD_SYM_RET(g_lib, fn_gatts_stop_service, a_mtkapi_bt_gatts_stop_service, -1);
    LOAD_SYM_RET(g_lib, fn_gatts_send_response, a_mtkapi_bt_gatts_send_response, -1);
    LOAD_SYM_RET(g_lib, fn_gattc_register_app, a_mtkapi_bt_gattc_register_app, -1);
    LOAD_SYM_RET(g_lib, fn_gattc_multi_adv_enable, a_mtkapi_bt_gattc_multi_adv_enable, -1);
    LOAD_SYM_RET(g_lib, fn_gattc_multi_adv_setdata, a_mtkapi_bt_gattc_multi_adv_setdata, -1);
    LOAD_SYM_RET(g_lib, fn_gattc_multi_adv_disable, a_mtkapi_bt_gattc_multi_adv_disable, -1);
    LOAD_SYM_RET(g_lib, fn_gattc_start_adv_set, a_mtkapi_bt_gattc_start_advertising_set, -1);
    LOAD_SYM_RET(g_lib, fn_gattc_stop_adv_set, a_mtkapi_bt_gattc_stop_advertising_set, -1);

    g_send_response = a_mtkapi_bt_gatts_send_response;
    g_stop_service = a_mtkapi_bt_gatts_stop_service;
    g_start_adv_set = a_mtkapi_bt_gattc_start_advertising_set;
    g_stop_adv_set = a_mtkapi_bt_gattc_stop_advertising_set;
    g_multi_adv_enable = a_mtkapi_bt_gattc_multi_adv_enable;
    g_multi_adv_setdata = a_mtkapi_bt_gattc_multi_adv_setdata;
    g_multi_adv_disable = a_mtkapi_bt_gattc_multi_adv_disable;

    /* Initialize RPC connection to btservice */
    ret = a_mtk_bt_service_init();
    printf("service_init: ret=%d\n", ret);

    c_rpc_init_mtk_bt_service_client();
    c_rpc_start_mtk_bt_service_client();

    /* Initialize GATTC callbacks (all 16 slots) */
    MTKRPCAPI_BT_APP_GATTC_CB_FUNC_T gattc_cbs;
    memset(&gattc_cbs, 0, sizeof(gattc_cbs));
    gattc_cbs.bt_gattc_event_cb = gattc_event_cb;
    gattc_cbs.bt_gattc_reg_client_cb = gattc_reg_client_cb;
    gattc_cbs.bt_gattc_scan_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_get_gatt_db_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_get_reg_noti_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_notify_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_read_char_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_write_char_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_read_desc_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_write_desc_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_scan_filt_param_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_scan_filt_status_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_scan_filt_cfg_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_adv_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_mtu_config_cb = gattc_generic_cb;
    gattc_cbs.bt_gattc_phy_updated_cb = gattc_generic_cb;

    ret = a_mtkapi_bt_gattc_base_init(&gattc_cbs, NULL);
    printf("gattc_base_init: ret=%d\n", ret);

    /* Initialize GATTS callbacks */
    MTKRPCAPI_BT_APP_GATTS_CB_FUNC_T gatts_cbs = {
        .bt_gatts_event_cb      = gatts_event_cb,
        .bt_gatts_reg_server_cb = gatts_reg_server_cb,
        .bt_gatts_add_srvc_cb   = gatts_add_srvc_cb,
        .bt_gatts_add_incl_cb   = gatts_add_incl_cb,
        .bt_gatts_add_char_cb   = gatts_add_char_cb,
        .bt_gatts_add_desc_cb   = gatts_add_desc_cb,
        .bt_gatts_op_srvc_cb    = gatts_op_srvc_cb,
        .bt_gatts_req_read_cb   = gatts_req_read_cb,
        .bt_gatts_req_write_cb  = gatts_req_write_cb,
        .bt_gatts_ind_sent_cb   = gatts_ind_sent_cb,
    };

    ret = a_mtkapi_bt_gatts_base_init(&gatts_cbs, NULL);
    printf("gatts_base_init: ret=%d\n", ret);

    /* Register GATTC app — retry until RPC transport is ready
     * (no ready callback exposed by the RPC layer) */
    for (int attempt = 1; ; attempt++) {
        ret = a_mtkapi_bt_gattc_register_app("d3f8a120-5c6e-4a93-b8d2-9e7f01234abc");
        if (ret == 0) break;
        if (attempt >= 30) {
            fprintf(stderr, "gattc_register_app failed after %d attempts (ret=%d)\n", attempt, ret);
            break;
        }
        usleep(200000);
    }
    printf("gattc_register_app: ret=%d\n", ret);
    if (!wait_for(&g_client_if, 3000))
        fprintf(stderr, "WARNING: client_if not received, using 0\n");

    if (g_client_if < 0)
        g_client_if = 0;

    /* Register GATT server */
    ret = a_mtkapi_bt_gatts_register_server(SERVICE_UUID);
    printf("gatts_register_server: ret=%d\n", ret);
    if (!wait_for(&g_server_if, 3000))
        fprintf(stderr, "GATTS register failed. Continuing with advertising only.\n");

    /* Set up GATT service: service + characteristic + CCCD */
    if (g_server_if >= 0) {
        a_mtkapi_bt_gatts_add_service(g_server_if, SERVICE_UUID, 1, 4);
        wait_for(&g_srvc_handle, 2000);

        if (g_srvc_handle >= 0) {
            a_mtkapi_bt_gatts_add_char(g_server_if, g_srvc_handle, CHAR_PAGE_UUID,
                                        GATT_PROP_READ | GATT_PROP_WRITE | GATT_PROP_NOTIFY,
                                        GATT_PERM_READ | GATT_PERM_WRITE);
            wait_for(&g_char_handle, 2000);

            a_mtkapi_bt_gatts_add_desc(g_server_if, g_srvc_handle, CCCD_UUID,
                                        GATT_PERM_READ | GATT_PERM_WRITE);
            wait_for(&g_descr_handle, 2000);

            a_mtkapi_bt_gatts_start_service(g_server_if, g_srvc_handle, 2);
            wait_for(&g_srvc_started, 2000);
        }
    }

    /* Start legacy BLE advertising */
    start_legacy_advertising();

    printf("\n=== BLE Peripheral running ===\n");
    printf("Service UUID: %s\n", SERVICE_UUID);
    printf("Char UUID:    %s\n", CHAR_PAGE_UUID);
    printf("Socket:       %s\n", SOCKET_PATH);
    return 0;
}

/* --- Main --- */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* Use sigaction without SA_RESTART so blocking read() gets EINTR */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGABRT, crash_handler);

    setbuf(stdout, NULL);
    printf("=== Kobo BLE Peripheral ===\n");

    if (bt_init() != 0) {
        fprintf(stderr, "BT init failed\n");
        return 1;
    }

    /* Block on socket — plugin sends CMD_HEALTH_CHECK on resume */
    while (g_running) {
        if (g_sock_fd < 0) {
            /* Connect to plugin socket */
            g_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (g_sock_fd < 0) {
                printf("[SOCK] socket: %s\n", strerror(errno));
                sleep(1);
                continue;
            }
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
            if (connect(g_sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                printf("[SOCK] connect: %s\n", strerror(errno));
                close(g_sock_fd);
                g_sock_fd = -1;
                sleep(1);
                continue;
            }
            printf("[SOCK] connected to plugin\n");
        }

        unsigned char cmd;
        ssize_t n = read(g_sock_fd, &cmd, 1);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            printf("[SOCK] plugin disconnected\n");
            close(g_sock_fd);
            g_sock_fd = -1;
            continue;
        }

        if (cmd == CMD_HEALTH_CHECK) {
            printf("[HEALTH] Health check received, testing BT stack...\n");
            /* Try re-advertising — if the stack is stale this will fail or hang */
            INT32 ret = start_legacy_advertising();
            if (ret != 0) {
                printf("[HEALTH] BT stack stale (adv ret=%d) — exiting for respawn\n", ret);
                g_health_fail = 1;
                g_running = 0;
            } else {
                printf("[HEALTH] BT stack OK\n");
            }
        }
    }

    /* Cleanup */
    int exit_code = g_health_fail ? EXIT_HEALTH_FAIL : 0;
    printf("\nShutting down (exit code %d)...\n", exit_code);
    if (g_sock_fd >= 0)
        close(g_sock_fd);
    if (g_stop_adv_set && g_client_if >= 0)
        g_stop_adv_set(g_client_if);
    if (g_server_if >= 0 && g_stop_service)
        g_stop_service(g_server_if, g_srvc_handle);
    if (g_lib)
        dlclose(g_lib);
    printf("Done.\n");
    return exit_code;
}

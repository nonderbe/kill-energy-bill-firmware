// Kill Energy Bill — HomeWizard P1 meter poller (OpenBK port)
//
// Polls two HTTP endpoints on the meter every N seconds (async, non-blocking):
//   GET /api/v1/data     (every 5 s)  → active_power_w → KillBill_SetPowerW()
//   GET /api/v1/telegram (every 60 s) → OBIS 1-0:1.6.0 → KillBill_UpdateMonthlyPeakW()
//
// Config stored in LittleFS:
//   /keb/p1_host  — IP or hostname of the P1 meter (e.g. "192.168.1.42")
//   /keb/p1_port  — TCP port, default 80
//
// If no host is configured, mDNS discovery runs in the background and fills in
// the host automatically.  Discovery retries every 60 s until a meter is found.
//
// To configure at runtime: KEB_SetP1Host <ip>  (optionally: KEB_SetP1Port <port>)

#include "../new_common.h"
#include "../new_pins.h"
#include "../cmnds/cmd_public.h"
#include "../logging/logging.h"
#include "../cJSON/cJSON.h"
#include "../pal/keb_pal.h"
#include "../net/keb_mdns_client.h"
#include "drv_killbill_peak.h"
#include "drv_killbill_p1.h"

#include <stdlib.h>
#include <string.h>

#define P1_HOST_KEY              "p1_host"
#define P1_PORT_KEY              "p1_port"
#define P1_DEFAULT_PORT          80
#define P1_DATA_INTERVAL_MS      5000
#define P1_TELEGRAM_INTERVAL_MS  60000
#define P1_DISCOVER_RETRY_MS     60000

static char     s_p1_host[64];
static uint16_t s_port;
static uint32_t s_last_data_ms;
static uint32_t s_last_telegram_ms;
static uint32_t s_last_ok_ms;
static bool     s_data_in_flight;
static bool     s_telegram_in_flight;

// mDNS discovery state — written from background task, read from main loop
static volatile bool s_discover_pending;   // true = result ready to apply
static char          s_discovered_host[64];
static uint16_t      s_discovered_port;
static uint32_t      s_last_discover_ms;
static bool          s_discover_running;

// Parse the second float in an OBIS line like: 1-0:1.6.0(timestamp)(value*kW)
static float parse_obis_second_float(const char *data, const char *obis) {
    const char *pos = strstr(data, obis);
    if (!pos) return 0.0f;
    const char *first_open  = strchr(pos, '(');
    if (!first_open) return 0.0f;
    const char *first_close = strchr(first_open, ')');
    if (!first_close) return 0.0f;
    const char *second_open = strchr(first_close + 1, '(');
    if (!second_open) return 0.0f;
    return strtof(second_open + 1, NULL);
}

static void on_data_response(int status, const char *body, void *user) {
    s_data_in_flight = false;
    if (status != 200 || !body) {
        keb_log("P1", "data HTTP %d", status);
        return;
    }
    cJSON *json = cJSON_Parse(body);
    if (!json) {
        keb_log("P1", "data JSON parse error");
        return;
    }

    int smr = 0;
    cJSON *j = cJSON_GetObjectItem(json, "smr_version");
    if (j && cJSON_IsNumber(j)) smr = (int)j->valuedouble;

    if (smr == 0) {
        keb_log("P1", "smr_version=0 (P1 port not activated on meter)");
        cJSON_Delete(json);
        return;
    }

    int net_w = 0;
    j = cJSON_GetObjectItem(json, "active_power_w");
    if (j && cJSON_IsNumber(j)) net_w = (int)j->valuedouble;

    cJSON_Delete(json);

    s_last_ok_ms = keb_millis();
    KillBill_SetPowerW(net_w);
    keb_log("P1", "smr=%d %dW", smr, net_w);
}

static void on_telegram_response(int status, const char *body, void *user) {
    s_telegram_in_flight = false;
    if (status != 200 || !body) return; // optional endpoint — not all firmware versions support it

    float kw = parse_obis_second_float(body, "1-0:1.6.0");
    if (kw > 0.0f) {
        int monthly_w = (int)(kw * 1000.0f + 0.5f);
        KillBill_UpdateMonthlyPeakW(monthly_w);
    }
}

static commandResult_t P1_SetHostCmd(const void *ctx, const char *cmd,
                                     const char *args, int flags) {
    Tokenizer_TokenizeString(args, 0);
    if (Tokenizer_CheckArgsCountAndPrintWarning(cmd, 1))
        return CMD_RES_NOT_ENOUGH_ARGUMENTS;
    const char *host = Tokenizer_GetArg(0);
    strncpy(s_p1_host, host, sizeof(s_p1_host) - 1);
    s_p1_host[sizeof(s_p1_host) - 1] = '\0';
    keb_cfg_set_str(P1_HOST_KEY, s_p1_host);
    s_last_data_ms     = 0; // trigger immediate poll
    s_last_telegram_ms = 0;
    keb_log("P1", "host set: %s", s_p1_host);
    return CMD_RES_OK;
}

static commandResult_t P1_SetPortCmd(const void *ctx, const char *cmd,
                                     const char *args, int flags) {
    Tokenizer_TokenizeString(args, 0);
    if (Tokenizer_CheckArgsCountAndPrintWarning(cmd, 1))
        return CMD_RES_NOT_ENOUGH_ARGUMENTS;
    s_port = (uint16_t)Tokenizer_GetArgInteger(0);
    keb_cfg_set_i32(P1_PORT_KEY, (int32_t)s_port);
    keb_log("P1", "port set: %u", s_port);
    return CMD_RES_OK;
}

// Background task: runs keb_mdns_discover_p1() then signals the main loop.
static void p1_discover_task(void *arg) {
    (void)arg;
    char host[64] = {0};
    uint16_t port = 80;
    keb_log("P1", "mDNS discovery started");
    bool ok = keb_mdns_discover_p1(host, sizeof(host), &port);
    if (ok) {
        strncpy(s_discovered_host, host, sizeof(s_discovered_host) - 1);
        s_discovered_host[sizeof(s_discovered_host) - 1] = '\0';
        s_discovered_port   = port;
        s_discover_pending  = true;  // signal main loop
        keb_log("P1", "mDNS: found %s:%u", host, port);
    } else {
        keb_log("P1", "mDNS: no P1 meter found");
    }
    s_discover_running = false;
}

static void p1_start_discovery(void) {
    if (s_discover_running) return;
    s_discover_running   = true;
    s_last_discover_ms   = keb_millis();
    keb_run_in_background("keb_p1_disc", p1_discover_task, NULL);
}

void P1_Init(void) {
    s_p1_host[0]         = '\0';
    s_port               = P1_DEFAULT_PORT;
    s_last_data_ms       = 0;
    s_last_telegram_ms   = 0;
    s_last_ok_ms         = 0;
    s_data_in_flight     = false;
    s_telegram_in_flight = false;
    s_discover_pending   = false;
    s_discover_running   = false;
    s_last_discover_ms   = 0;
    s_discovered_host[0] = '\0';
    s_discovered_port    = 80;

    keb_cfg_get_str(P1_HOST_KEY, s_p1_host, sizeof(s_p1_host));
    int32_t port;
    if (keb_cfg_get_i32(P1_PORT_KEY, &port)) s_port = (uint16_t)port;

    //cmddetail:{"name":"KEB_SetP1Host","args":"[ip]",
    //cmddetail:"descr":"Set HomeWizard P1 meter IP/hostname for Kill Energy Bill.",
    //cmddetail:"fn":"P1_SetHostCmd","file":"driver/drv_killbill_p1.c","requires":"",
    //cmddetail:"examples":"KEB_SetP1Host 192.168.1.42"}
    CMD_RegisterCommand("KEB_SetP1Host", P1_SetHostCmd, NULL);

    //cmddetail:{"name":"KEB_SetP1Port","args":"[port]",
    //cmddetail:"descr":"Set HomeWizard P1 meter HTTP port (default 80).",
    //cmddetail:"fn":"P1_SetPortCmd","file":"driver/drv_killbill_p1.c","requires":"",
    //cmddetail:"examples":"KEB_SetP1Port 80"}
    CMD_RegisterCommand("KEB_SetP1Port", P1_SetPortCmd, NULL);

    if (s_p1_host[0] != '\0') {
        keb_log("P1", "host=%s port=%u", s_p1_host, s_port);
    } else {
        keb_log("P1", "no host — starting mDNS discovery");
        p1_start_discovery();
    }
}

void P1_Update(void) {
    uint32_t now = keb_millis();

    // Apply discovery result from background task
    if (s_discover_pending) {
        s_discover_pending = false;
        strncpy(s_p1_host, s_discovered_host, sizeof(s_p1_host) - 1);
        s_p1_host[sizeof(s_p1_host) - 1] = '\0';
        s_port = s_discovered_port;
        keb_cfg_set_str(P1_HOST_KEY, s_p1_host);
        keb_cfg_set_i32(P1_PORT_KEY, (int32_t)s_port);
        s_last_data_ms     = 0; // trigger immediate poll
        s_last_telegram_ms = 0;
        keb_log("P1", "applied discovered host=%s port=%u", s_p1_host, s_port);
    }

    // Retry discovery if still unconfigured
    if (s_p1_host[0] == '\0') {
        if (!s_discover_running &&
            (s_last_discover_ms == 0 || (now - s_last_discover_ms) >= P1_DISCOVER_RETRY_MS)) {
            p1_start_discovery();
        }
        return;
    }

    if (!s_data_in_flight &&
        (s_last_data_ms == 0 || (now - s_last_data_ms) >= P1_DATA_INTERVAL_MS)) {
        s_last_data_ms   = now;
        s_data_in_flight = true;
        char url[128];
        snprintf(url, sizeof(url), "http://%s:%u/api/v1/data", s_p1_host, (unsigned)s_port);
        keb_http_get(url, 5000, on_data_response, NULL);
    }

    if (!s_telegram_in_flight &&
        (s_last_telegram_ms == 0 || (now - s_last_telegram_ms) >= P1_TELEGRAM_INTERVAL_MS)) {
        s_last_telegram_ms   = now;
        s_telegram_in_flight = true;
        char url[128];
        snprintf(url, sizeof(url), "http://%s:%u/api/v1/telegram", s_p1_host, (unsigned)s_port);
        keb_http_get(url, 5000, on_telegram_response, NULL);
    }
}

const char *P1_GetHost(void) {
    return s_p1_host;
}

bool P1_IsAlive(void) {
    if (s_last_ok_ms == 0) return false;
    return (keb_millis() - s_last_ok_ms) < 30000;
}

// Kill Energy Bill — multi-plug cascade coordinator (OpenBK port)
//
// Equivalent to coordinator.cpp in the Arduino firmware.
// OpenBK manages the MQTT connection; this driver just registers a callback
// and publishes on the shared coordinator topic.
//
// Topic: killbill/coordinator/peak
// Payload (JSON):
//   {"device_id":"<client_id>","monthly_peak_w":<int>,"priority":<int>}
//
// Algorithm:
//   - Receive peer plug status every 15 s
//   - Track the highest monthly_peak_w across all active plugs
//   - Feed that maximum into KillBill_UpdateMonthlyPeakW() so the peak guard
//     uses the most conservative threshold seen anywhere on the network
//   - Plugs that have not reported within 60 s are marked stale

#include "../new_common.h"
#include "../new_pins.h"
#include "../new_cfg.h"
#include "../obk_config.h"
#include "../cmnds/cmd_public.h"
#include "../logging/logging.h"
#include "../cJSON/cJSON.h"
#include "../pal/keb_pal.h"
#include "drv_killbill_peak.h"
#include "drv_killbill_coordinator.h"

#if ENABLE_MQTT
#include "../mqtt/new_mqtt.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define COORD_TOPIC          "killbill/coordinator/peak"
#define COORD_TOPIC_BASE     "killbill/coordinator/"
#define COORD_MQTT_CB_ID     100   // must not collide with OpenBK built-in IDs 1–8
#define COORD_MAX_PLUGS      16
#define COORD_PUBLISH_MS     15000
#define COORD_STALE_MS       60000

typedef struct {
    char     device_id[24];
    uint8_t  priority;
    int      monthly_peak_w;
    uint32_t last_seen_ms;
    bool     active;
} PlugInfo_t;

static PlugInfo_t s_plugs[COORD_MAX_PLUGS];
static int        s_plug_count;
static uint32_t   s_last_publish_ms;
static int        s_priority;

// Returns the highest monthly_peak_w across all active peers and ourselves.
static int get_network_peak(void) {
    int max_peak = KillBill_GetMonthlyPeakW();
    for (int i = 0; i < s_plug_count; i++) {
        if (s_plugs[i].active && s_plugs[i].monthly_peak_w > max_peak)
            max_peak = s_plugs[i].monthly_peak_w;
    }
    return max_peak;
}

static void clean_stale(void) {
    uint32_t now = keb_millis();
    for (int i = 0; i < s_plug_count; i++) {
        if (s_plugs[i].active && (now - s_plugs[i].last_seen_ms) > COORD_STALE_MS) {
            keb_log("COORD", "plug '%s' timed out", s_plugs[i].device_id);
            s_plugs[i].active = false;
        }
    }
}

#if ENABLE_MQTT
static int keb_coordinator_on_mqtt(obk_mqtt_request_t *request) {
    if (strcmp(request->topic, COORD_TOPIC) != 0)
        return 0;

    int len = request->receivedLen;
    char *buf = (char *)malloc(len + 1);
    if (!buf) return 0;
    memcpy(buf, request->received, len);
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) return 0;

    cJSON *j_id   = cJSON_GetObjectItem(json, "device_id");
    cJSON *j_peak = cJSON_GetObjectItem(json, "monthly_peak_w");
    cJSON *j_prio = cJSON_GetObjectItem(json, "priority");

    const char *dev_id = (j_id && cJSON_IsString(j_id)) ? j_id->valuestring : NULL;
    if (!dev_id || dev_id[0] == '\0') {
        cJSON_Delete(json);
        return 0;
    }

    // Ignore own broadcasts
    if (strcmp(dev_id, CFG_GetMQTTClientId()) == 0) {
        cJSON_Delete(json);
        return 0;
    }

    int peak = (j_peak && cJSON_IsNumber(j_peak)) ? (int)j_peak->valuedouble : 0;
    int prio = (j_prio && cJSON_IsNumber(j_prio)) ? (int)j_prio->valuedouble : 5;

    // Find existing entry or allocate a new slot
    PlugInfo_t *slot = NULL;
    for (int i = 0; i < s_plug_count; i++) {
        if (strcmp(s_plugs[i].device_id, dev_id) == 0) {
            slot = &s_plugs[i];
            break;
        }
    }
    if (!slot && s_plug_count < COORD_MAX_PLUGS) {
        slot = &s_plugs[s_plug_count++];
        strncpy(slot->device_id, dev_id, sizeof(slot->device_id) - 1);
        slot->device_id[sizeof(slot->device_id) - 1] = '\0';
    }
    cJSON_Delete(json);

    if (!slot) return 0;   // table full

    slot->monthly_peak_w = peak;
    slot->priority       = (uint8_t)prio;
    slot->last_seen_ms   = keb_millis();
    slot->active         = true;

    int net_peak = get_network_peak();
    KillBill_UpdateMonthlyPeakW(net_peak);

    keb_log("COORD", "plug '%s' prio=%d peak=%dW (net_max=%dW)",
            dev_id, prio, peak, net_peak);

    return 0;  // 0 = let OpenBK continue processing
}
#endif // ENABLE_MQTT

static commandResult_t Coordinator_SetPriority(const void *ctx, const char *cmd,
                                               const char *args, int flags) {
    Tokenizer_TokenizeString(args, 0);
    if (Tokenizer_CheckArgsCountAndPrintWarning(cmd, 1))
        return CMD_RES_NOT_ENOUGH_ARGUMENTS;
    s_priority = Tokenizer_GetArgInteger(0);
    keb_cfg_set_i32("keb_priority", (int32_t)s_priority);
    keb_log("COORD", "priority = %d", s_priority);
    return CMD_RES_OK;
}

void Coordinator_Init(void) {
    memset(s_plugs, 0, sizeof(s_plugs));
    s_plug_count      = 0;
    s_last_publish_ms = 0;
    s_priority        = 5;

    int32_t v;
    if (keb_cfg_get_i32("keb_priority", &v)) s_priority = (int)v;

    //cmddetail:{"name":"KEB_SetPriority","args":"[1-16]",
    //cmddetail:"descr":"Set Kill Energy Bill cascade priority (1=first to shed, 16=last). Default 5.",
    //cmddetail:"fn":"Coordinator_SetPriority","file":"driver/drv_killbill_coordinator.c","requires":"",
    //cmddetail:"examples":"KEB_SetPriority 1"}
    CMD_RegisterCommand("KEB_SetPriority", Coordinator_SetPriority, NULL);

#if ENABLE_MQTT
    MQTT_RegisterCallback(COORD_TOPIC_BASE, COORD_TOPIC, COORD_MQTT_CB_ID,
                          keb_coordinator_on_mqtt);
    keb_log("COORD", "started (priority=%d, topic=%s)", s_priority, COORD_TOPIC);
#else
    keb_log("COORD", "started (priority=%d, MQTT not compiled — cascade disabled)",
            s_priority);
#endif
}

void Coordinator_OnEverySecond(void) {
    uint32_t now = keb_millis();
    if (now - s_last_publish_ms < COORD_PUBLISH_MS) return;
    s_last_publish_ms = now;

    clean_stale();

#if ENABLE_MQTT
    if (!MQTT_IsReady()) return;

    char json[128];
    snprintf(json, sizeof(json),
             "{\"device_id\":\"%s\",\"monthly_peak_w\":%d,\"priority\":%d}",
             CFG_GetMQTTClientId(),
             KillBill_GetMonthlyPeakW(),
             s_priority);

    // OBK_PUBLISH_FLAG_RAW_TOPIC_NAME: sChannel is used as the full topic as-is.
    MQTT_Publish("", COORD_TOPIC, json,
                 OBK_PUBLISH_FLAG_RAW_TOPIC_NAME | OBK_PUBLISH_FLAG_QOS_ZERO);
#endif
}

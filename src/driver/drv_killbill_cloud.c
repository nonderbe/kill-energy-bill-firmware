// Kill Energy Bill — cloud feature-flag driver for OpenBK (BK7231N + ESP32)
//
// GET http://firmware.kill-energy-bill.com/api/v1/features?device_id=XXX
// Response: {"cascade":true,"advanced_scheduling":false,"premium_reports":false}
//
// Uses HTTP (not HTTPS) because mbedTLS does not fit on BK7231N.
// The features endpoint is on the same OTA server, which is trusted infrastructure.

#include "../new_common.h"
#include "../new_pins.h"
#include "../new_cfg.h"
#include "../logging/logging.h"
#include "../pal/keb_pal.h"
#include "../cJSON/cJSON.h"
#include "drv_killbill_cloud.h"

#if ENABLE_DRIVER_KILLBILL

#include <string.h>
#include <stdio.h>

#define KEB_CLOUD_PRIMARY   "http://firmware.kill-energy-bill.com"
#define KEB_CLOUD_SECONDARY "http://firmware.local-share.com"
#define KEB_CLOUD_PATH      "/api/v1/features"

#define KEB_CLOUD_INTERVAL_S  86400u   // 24 hours between successful polls
#define KEB_CLOUD_RETRY_S       300u   // 5 minutes between failed polls
#define KEB_CLOUD_INITIAL_S      60u   // first poll 60s after boot (WiFi needs time)
#define KEB_CLOUD_TIMEOUT_MS   5000

// Free tier is the default — stays active if cloud is unreachable.
static KebFeatures s_features = {
    .cascade             = true,
    .advanced_scheduling = false,
    .premium_reports     = false,
};

static uint32_t s_tick_s = 0;
static uint32_t s_wait_s = KEB_CLOUD_INITIAL_S;
static bool     s_pending = false;   // true while an HTTP request is in-flight

static char s_primary_url[160];
static char s_secondary_url[160];

bool keb_feature_enabled(const char *name) {
    if (strcmp(name, "cascade")              == 0) return s_features.cascade;
    if (strcmp(name, "advanced_scheduling")  == 0) return s_features.advanced_scheduling;
    if (strcmp(name, "premium_reports")      == 0) return s_features.premium_reports;
    return false;
}

const KebFeatures *keb_features(void) { return &s_features; }

// ---- HTTP callbacks ----------------------------------------------------------

static void parse_and_apply(const char *body) {
    cJSON *doc = cJSON_Parse(body);
    if (!doc) return;

    cJSON *j;
    j = cJSON_GetObjectItem(doc, "cascade");
    if (j) s_features.cascade = cJSON_IsTrue(j);

    j = cJSON_GetObjectItem(doc, "advanced_scheduling");
    if (j) s_features.advanced_scheduling = cJSON_IsTrue(j);

    j = cJSON_GetObjectItem(doc, "premium_reports");
    if (j) s_features.premium_reports = cJSON_IsTrue(j);

    cJSON_Delete(doc);

    keb_log("CLOUD", "features: cascade=%d adv_sched=%d prem_reports=%d",
            s_features.cascade, s_features.advanced_scheduling, s_features.premium_reports);
}

static void on_secondary(int status, const char *body, void *user) {
    (void)user;
    s_pending = false;
    if (status == 200 && body && body[0]) {
        parse_and_apply(body);
        s_wait_s = KEB_CLOUD_INTERVAL_S;
    } else {
        keb_log("CLOUD", "secondary failed (status=%d), retry in %ds",
                status, (int)KEB_CLOUD_RETRY_S);
        s_wait_s = KEB_CLOUD_RETRY_S;
    }
}

static void on_primary(int status, const char *body, void *user) {
    (void)user;
    if (status == 200 && body && body[0]) {
        s_pending = false;
        parse_and_apply(body);
        s_wait_s = KEB_CLOUD_INTERVAL_S;
        return;
    }
    // Primary unreachable — try secondary
    keb_log("CLOUD", "primary failed (status=%d), trying secondary", status);
    keb_http_get(s_secondary_url, KEB_CLOUD_TIMEOUT_MS, on_secondary, NULL);
}

// ---- Public API -------------------------------------------------------------

void KebCloud_Init(void) {
    s_tick_s  = 0;
    s_wait_s  = KEB_CLOUD_INITIAL_S;
    s_pending = false;
    // Defaults already set by static initialiser above.
}

void KebCloud_Update(void) {
    if (s_pending) return;

    s_tick_s++;
    if (s_tick_s < s_wait_s) return;
    s_tick_s = 0;

    // Build URLs with device_id for per-device tier lookup.
    const char *dev_id = CFG_GetMQTTClientId();
    if (!dev_id || dev_id[0] == '\0') dev_id = "unknown";

    snprintf(s_primary_url,   sizeof(s_primary_url),
             KEB_CLOUD_PRIMARY   KEB_CLOUD_PATH "?device_id=%s", dev_id);
    snprintf(s_secondary_url, sizeof(s_secondary_url),
             KEB_CLOUD_SECONDARY KEB_CLOUD_PATH "?device_id=%s", dev_id);

    s_pending = true;
    keb_http_get(s_primary_url, KEB_CLOUD_TIMEOUT_MS, on_primary, NULL);
}

#endif // ENABLE_DRIVER_KILLBILL

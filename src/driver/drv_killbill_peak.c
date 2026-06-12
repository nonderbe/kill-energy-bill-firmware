// Kill Energy Bill — peak guard driver
// Monitors P1 net power and sheds relay when quarter-peak approaches monthly peak.
//
// Algorithm:
//   quarter_peak = rolling 15-min average of P1 net power (W)
//   monthly_peak = OBIS 1-0:1.6.0 from P1 meter (never locally tracked)
//   threshold    = monthly_peak + buffer_w
//   if quarter_peak >= threshold  → relay OFF (shed)
//   if quarter_peak < threshold - hysteresis_w → relay ON (restore)
//
// Min monthly peak: 2500 W (Flemish exemption — Vlaamse vrijstelling)

#include "../new_common.h"
#include "../new_pins.h"
#include "../new_cfg.h"
#include "../cmnds/cmd_public.h"
#include "../logging/logging.h"
#include "drv_local.h"
#include "../httpserver/new_http.h"
#include "drv_killbill_peak.h"
#include "drv_killbill_p1.h"
#include "drv_killbill_coordinator.h"
#include "drv_killbill_webui.h"
#include "drv_killbill_ota.h"
#include "drv_killbill_cloud.h"
#include "../pal/keb_pal.h"

#define KEB_MIN_MONTHLY_PEAK_W   2500
#define KEB_DEFAULT_BUFFER_W     100
#define KEB_DEFAULT_HYSTERESIS_W 50
#define KEB_WINDOW_SECONDS       900    // 15 minutes
#define KEB_RELAY_CHANNEL        1      // OpenBK channel index for the relay

static int   g_buffer_w       = KEB_DEFAULT_BUFFER_W;
static int   g_hysteresis_w   = KEB_DEFAULT_HYSTERESIS_W;
static int   g_monthly_peak_w = 0;
static int   g_quarter_peak_w = 0;
static int   g_last_power_w   = 0;
static bool  g_shed_active    = false;

// Rolling window (one sample per second via OnEverySecond)
static int   g_window[KEB_WINDOW_SECONDS];
static int   g_window_idx   = 0;
static int   g_window_count = 0;
static long  g_window_sum   = 0;

static void KillBill_AddSample(int power_w) {
    g_last_power_w = power_w;
    if (g_window_count < KEB_WINDOW_SECONDS) {
        g_window_count++;
    } else {
        g_window_sum -= g_window[g_window_idx];
    }
    g_window[g_window_idx] = power_w;
    g_window_sum += power_w;
    g_window_idx = (g_window_idx + 1) % KEB_WINDOW_SECONDS;
    g_quarter_peak_w = (g_window_count > 0) ? (int)(g_window_sum / g_window_count) : 0;
}

static void KillBill_Evaluate(void) {
    int monthly = g_monthly_peak_w;
    if (monthly < KEB_MIN_MONTHLY_PEAK_W) monthly = KEB_MIN_MONTHLY_PEAK_W;
    int threshold = monthly + g_buffer_w;

    if (!g_shed_active && g_quarter_peak_w >= threshold) {
        g_shed_active = true;
        keb_relay_set(KEB_RELAY_CHANNEL, false);
        keb_log("PKG", "SHED: quarter_peak %dW >= threshold %dW — relay OFF",
                g_quarter_peak_w, threshold);
    } else if (g_shed_active && g_quarter_peak_w < (threshold - g_hysteresis_w)) {
        g_shed_active = false;
        keb_relay_set(KEB_RELAY_CHANNEL, true);
        keb_log("PKG", "RESTORE: quarter_peak %dW < %dW — relay ON",
                g_quarter_peak_w, threshold - g_hysteresis_w);
    }
}

static commandResult_t KillBill_SetBuffer(const void *ctx, const char *cmd,
                                           const char *args, int flags) {
    Tokenizer_TokenizeString(args, 0);
    if (Tokenizer_CheckArgsCountAndPrintWarning(cmd, 1))
        return CMD_RES_NOT_ENOUGH_ARGUMENTS;
    g_buffer_w = Tokenizer_GetArgInteger(0);
    keb_cfg_set_i32("keb_buffer_w", g_buffer_w);
    keb_log("PKG", "buffer_w = %d W", g_buffer_w);
    return CMD_RES_OK;
}

static commandResult_t KillBill_SetHysteresis(const void *ctx, const char *cmd,
                                               const char *args, int flags) {
    Tokenizer_TokenizeString(args, 0);
    if (Tokenizer_CheckArgsCountAndPrintWarning(cmd, 1))
        return CMD_RES_NOT_ENOUGH_ARGUMENTS;
    g_hysteresis_w = Tokenizer_GetArgInteger(0);
    keb_cfg_set_i32("keb_hysteresis_w", g_hysteresis_w);
    keb_log("PKG", "hysteresis_w = %d W", g_hysteresis_w);
    return CMD_RES_OK;
}

static commandResult_t KillBill_SetMonthlyPeak(const void *ctx, const char *cmd,
                                                const char *args, int flags) {
    Tokenizer_TokenizeString(args, 0);
    if (Tokenizer_CheckArgsCountAndPrintWarning(cmd, 1))
        return CMD_RES_NOT_ENOUGH_ARGUMENTS;
    int v = Tokenizer_GetArgInteger(0);
    if (v < KEB_MIN_MONTHLY_PEAK_W) v = KEB_MIN_MONTHLY_PEAK_W;
    g_monthly_peak_w = v;
    keb_log("PKG", "monthly_peak = %d W", g_monthly_peak_w);
    return CMD_RES_OK;
}

// Called by P1 reader to push a live power measurement (W).
void KillBill_SetPowerW(int power_w) {
    KillBill_AddSample(power_w);
    KillBill_Evaluate();
}

int KillBill_GetMonthlyPeakW(void) {
    return g_monthly_peak_w;
}

// Called by P1 reader or coordinator to update monthly peak.
void KillBill_UpdateMonthlyPeakW(int w) {
    if (w < KEB_MIN_MONTHLY_PEAK_W) w = KEB_MIN_MONTHLY_PEAK_W;
    if (w != g_monthly_peak_w) {
        g_monthly_peak_w = w;
        keb_log("PKG", "monthly_peak updated: %dW", w);
    }
}

// startDriver KillBill
void KillBill_Init(void) {
    // Fixed shared mDNS hostname — all Kill Energy Bill plugs advertise as
    // kill-energy-bill.local.  MQTT client ID stays MAC-derived and stays unique.
    // Applies on the next boot (mDNS reads the name at startup before drivers run).
    {
        const char *cur = CFG_GetShortDeviceName();
        if (strncmp(cur, "kill-energy-bill", 16) != 0) {
            CFG_SetShortDeviceName("kill-energy-bill");
        }
    }

    // Load persisted config
    int32_t v;
    if (keb_cfg_get_i32("keb_buffer_w", &v))    g_buffer_w    = (int)v;
    if (keb_cfg_get_i32("keb_hysteresis_w", &v)) g_hysteresis_w = (int)v;

    //cmddetail:{"name":"KEB_SetBuffer","args":"[watts]",
    //cmddetail:"descr":"Set Kill Energy Bill buffer above monthly peak (W). Default 0.",
    //cmddetail:"fn":"KillBill_SetBuffer","file":"driver/drv_killbill_peak.c","requires":"",
    //cmddetail:"examples":"KEB_SetBuffer 100"}
    CMD_RegisterCommand("KEB_SetBuffer", KillBill_SetBuffer, NULL);

    //cmddetail:{"name":"KEB_SetHysteresis","args":"[watts]",
    //cmddetail:"descr":"Set Kill Energy Bill hysteresis (W). Default 50.",
    //cmddetail:"fn":"KillBill_SetHysteresis","file":"driver/drv_killbill_peak.c","requires":"",
    //cmddetail:"examples":"KEB_SetHysteresis 50"}
    CMD_RegisterCommand("KEB_SetHysteresis", KillBill_SetHysteresis, NULL);

    //cmddetail:{"name":"KEB_SetMonthlyPeak","args":"[watts]",
    //cmddetail:"descr":"Override monthly peak from P1 meter (W). Min 2500 W (Flemish exemption).",
    //cmddetail:"fn":"KillBill_SetMonthlyPeak","file":"driver/drv_killbill_peak.c","requires":"",
    //cmddetail:"examples":"KEB_SetMonthlyPeak 3500"}
    CMD_RegisterCommand("KEB_SetMonthlyPeak", KillBill_SetMonthlyPeak, NULL);

    keb_log("PKG", "peak guard started (window=%ds, min_peak=%dW, buffer=%dW, hyst=%dW)",
            KEB_WINDOW_SECONDS, KEB_MIN_MONTHLY_PEAK_W, g_buffer_w, g_hysteresis_w);

    P1_Init();
    Coordinator_Init();
    KillBill_WebUI_Init();
    OTA_Init();
    KebCloud_Init();
}

void KillBill_OnEverySecond(void) {
    P1_Update();
    Coordinator_OnEverySecond();
    OTA_Update();
    KebCloud_Update();
}

void KillBill_AppendInformationToHTTPIndexPage(http_request_t *request, int bPreState) {
    if (bPreState) return;
    hprintf255(request,
        "<h2>Kill Energy Bill</h2>"
        "<p>Quarter peak: <b>%d W</b> | Monthly peak: <b>%d W</b> | "
        "Buffer: %d W | Shed: <b>%s</b></p>",
        g_quarter_peak_w,
        g_monthly_peak_w < KEB_MIN_MONTHLY_PEAK_W ? KEB_MIN_MONTHLY_PEAK_W : g_monthly_peak_w,
        g_buffer_w,
        g_shed_active ? "YES" : "no");
}

int KillBill_GetQuarterPeakW(void)  { return g_quarter_peak_w; }
int KillBill_GetLastPowerW(void)    { return g_last_power_w; }
int KillBill_GetBufferW(void)       { return g_buffer_w; }
int KillBill_GetHysteresisW(void)   { return g_hysteresis_w; }
bool KillBill_IsShedActive(void)    { return g_shed_active; }

void KillBill_StopDriver(void) {
    g_shed_active  = false;
    g_window_idx   = 0;
    g_window_count = 0;
    g_window_sum   = 0;
}

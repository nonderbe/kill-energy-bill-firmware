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
//
// TODO (Stap 3): port rolling-window algorithm from peak_guard.cpp

#include "../new_common.h"
#include "../new_pins.h"
#include "../new_cfg.h"
#include "../cmnds/cmd_public.h"
#include "../logging/logging.h"
#include "drv_local.h"
#include "../httpserver/new_http.h"
#include "drv_killbill_peak.h"

#define KEB_MIN_MONTHLY_PEAK_W   2500
#define KEB_DEFAULT_BUFFER_W     0
#define KEB_DEFAULT_HYSTERESIS_W 50
#define KEB_WINDOW_SECONDS       900    // 15 minutes

static int   g_buffer_w     = KEB_DEFAULT_BUFFER_W;
static int   g_hysteresis_w = KEB_DEFAULT_HYSTERESIS_W;
static int   g_monthly_peak_w = 0;
static int   g_quarter_peak_w = 0;
static bool  g_shed_active  = false;

// Rolling window (samples collected once per second)
static int   g_window[KEB_WINDOW_SECONDS];
static int   g_window_idx   = 0;
static int   g_window_count = 0;
static long  g_window_sum   = 0;

static void KillBill_AddSample(int power_w) {
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

static commandResult_t KillBill_SetBuffer(const void *ctx, const char *cmd,
                                           const char *args, int flags) {
    Tokenizer_TokenizeString(args, 0);
    if (Tokenizer_CheckArgsCountAndPrintWarning(cmd, 1))
        return CMD_RES_NOT_ENOUGH_ARGUMENTS;
    g_buffer_w = Tokenizer_GetArgInteger(0);
    ADDLOG_INFO(LOG_FEATURE_DRV, "KEB: buffer_w set to %d W", g_buffer_w);
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
    ADDLOG_INFO(LOG_FEATURE_DRV, "KEB: monthly_peak set to %d W", g_monthly_peak_w);
    return CMD_RES_OK;
}

// startDriver KillBill
void KillBill_Init(void) {
    //cmddetail:{"name":"KEB_SetBuffer","args":"[watts]",
    //cmddetail:"descr":"Set Kill Energy Bill buffer above monthly peak (W). Default 0.",
    //cmddetail:"fn":"KillBill_SetBuffer","file":"driver/drv_killbill_peak.c","requires":"",
    //cmddetail:"examples":"KEB_SetBuffer 100"}
    CMD_RegisterCommand("KEB_SetBuffer", KillBill_SetBuffer, NULL);

    //cmddetail:{"name":"KEB_SetMonthlyPeak","args":"[watts]",
    //cmddetail:"descr":"Override monthly peak from P1 meter (W). Min 2500 W (Flemish exemption).",
    //cmddetail:"fn":"KillBill_SetMonthlyPeak","file":"driver/drv_killbill_peak.c","requires":"",
    //cmddetail:"examples":"KEB_SetMonthlyPeak 3500"}
    CMD_RegisterCommand("KEB_SetMonthlyPeak", KillBill_SetMonthlyPeak, NULL);

    ADDLOG_INFO(LOG_FEATURE_DRV, "KEB: peak guard started (window=%ds, min_peak=%dW)",
                KEB_WINDOW_SECONDS, KEB_MIN_MONTHLY_PEAK_W);
}

void KillBill_OnEverySecond(void) {
    // TODO (Stap 3): fetch P1 net power from drv_killbill_p1, feed into rolling window,
    // evaluate threshold and toggle relay via CHANNEL_Set().
    // For now: no-op placeholder so the driver compiles and registers.
    (void)KillBill_AddSample;   // suppress unused-function warning until wired up
}

void KillBill_AppendInformationToHTTPIndexPage(http_request_t *request, int bPreState) {
    if (bPreState) return;
    hprintf255(request,
        "<h2>Kill Energy Bill</h2>"
        "<p>Quarter peak: %d W | Monthly peak: %d W | Shed: %s</p>",
        g_quarter_peak_w,
        g_monthly_peak_w,
        g_shed_active ? "YES" : "no");
}

void KillBill_StopDriver(void) {
    g_shed_active  = false;
    g_window_idx   = 0;
    g_window_count = 0;
    g_window_sum   = 0;
}

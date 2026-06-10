// Kill Energy Bill — branded web UI (OpenBK port)
//
// Routes registered:
//   GET  /           → KEB dashboard (SPA — live data via /api/state)
//   GET  /keb/cfg    → KEB config page (buffer_w, hysteresis_w, p1_host, priority)
//   GET  /api/state  → JSON: current KEB state
//   POST /api/config → save KEB config params (form-encoded body)

#include "../new_common.h"
#include "../new_pins.h"
#include "../new_cfg.h"
#include "../cmnds/cmd_public.h"
#include "../logging/logging.h"
#include "../httpserver/new_http.h"
#include "../pal/keb_pal.h"
#include "drv_killbill_peak.h"
#include "drv_killbill_p1.h"
#include "drv_killbill_ota.h"
#include "drv_killbill_cloud.h"
#include "drv_killbill_webui.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LOG_FEATURE LOG_FEATURE_HTTP

// ---------------------------------------------------------------------------
// CSS shared by all KEB pages (design tokens from CLAUDE.md)
// ---------------------------------------------------------------------------
static const char KEB_CSS[] =
    ":root{"
    "--g7:#15803d;--g8:#166534;--g6:#16a34a;--g5:#f0fdf4;"
    "--gr50:#f9fafb;--gr200:#e5e7eb;--gr500:#6b7280;--gr700:#374151;--gr900:#111827;"
    "--r6:#dc2626;--a6:#d97706;"
    "}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',system-ui,sans-serif;"
    "margin:0;background:var(--gr50);color:var(--gr900);}"
    ".hdr{background:#fff;border-bottom:1px solid var(--gr200);padding:12px 20px;"
    "display:flex;justify-content:space-between;align-items:center;}"
    ".hdr h1{margin:0;font-size:1.2rem;color:var(--g7);font-weight:700;}"
    "nav a{color:var(--gr700);text-decoration:none;margin-left:16px;font-size:.9rem;}"
    "nav a:hover{color:var(--g7);}"
    ".content{padding:20px;max-width:900px;margin:auto;}"
    ".card{background:#fff;border:1px solid var(--gr200);border-radius:8px;padding:16px;margin-bottom:16px;}"
    ".card h2{margin-top:0;color:var(--g7);font-size:1rem;font-weight:600;}"
    ".hero{border-radius:10px;padding:18px 20px;margin-bottom:16px;transition:background .4s,border-color .4s;}"
    ".hero.ok{background:#f0fdf4;border:1px solid #bbf7d0;}"
    ".hero.warn{background:#fffbeb;border:1px solid #fde68a;}"
    ".hero.crit{background:#fef2f2;border:1px solid #fecaca;}"
    ".hero.neutral{background:#f9fafb;border:1px solid #e5e7eb;}"
    ".hero-status{display:flex;align-items:center;gap:10px;margin-bottom:4px;}"
    ".hero-dot{width:11px;height:11px;border-radius:50%;flex-shrink:0;}"
    ".hero.ok .hero-dot{background:#16a34a;}"
    ".hero.warn .hero-dot{background:#d97706;}"
    ".hero.crit .hero-dot{background:#dc2626;animation:pulse 1.4s ease-in-out infinite;}"
    ".hero.neutral .hero-dot{background:#9ca3af;}"
    ".hero-title{font-size:1.1rem;font-weight:700;}"
    ".hero.ok .hero-title{color:#166534;}"
    ".hero.warn .hero-title{color:#92400e;}"
    ".hero.crit .hero-title{color:#991b1b;}"
    ".hero.neutral .hero-title{color:#374151;}"
    ".hero-sub{font-size:.83rem;margin-bottom:14px;}"
    ".hero.ok .hero-sub,.hero.ok .hero-meta{color:#166534;}"
    ".hero.warn .hero-sub,.hero.warn .hero-meta{color:#92400e;}"
    ".hero.crit .hero-sub,.hero.crit .hero-meta{color:#991b1b;}"
    ".hero.neutral .hero-sub,.hero.neutral .hero-meta{color:#6b7280;}"
    ".hero-power{font-size:2rem;font-weight:700;line-height:1.1;margin-bottom:12px;}"
    ".hero.ok .hero-power{color:#15803d;}"
    ".hero.warn .hero-power{color:#d97706;}"
    ".hero.crit .hero-power{color:#dc2626;}"
    ".hero.neutral .hero-power{color:#374151;}"
    ".hero-meta{font-size:.8rem;margin-bottom:12px;opacity:.75;}"
    ".bar-row{display:flex;align-items:center;gap:10px;}"
    ".bar-track{background:rgba(0,0,0,.08);border-radius:4px;height:7px;overflow:hidden;flex:1;}"
    ".bar-fill{height:100%;border-radius:4px;transition:width .6s ease;}"
    ".hero.ok .bar-fill{background:#16a34a;}"
    ".hero.warn .bar-fill{background:#d97706;}"
    ".hero.crit .bar-fill{background:#dc2626;}"
    ".bar-pct{font-size:.78rem;font-weight:600;min-width:38px;text-align:right;}"
    ".hero.ok .bar-pct{color:#166534;}"
    ".hero.warn .bar-pct{color:#92400e;}"
    ".hero.crit .bar-pct{color:#991b1b;}"
    ".tile-row{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:16px;}"
    ".tile{border-radius:8px;padding:16px;text-align:center;border:1px solid var(--gr200);background:#fff;}"
    ".tile.ton{background:var(--g5);border-color:#bbf7d0;}"
    ".tile.toff{background:#fef2f2;border-color:#fecaca;}"
    ".tile-val{font-size:1.7rem;font-weight:700;color:var(--gr900);}"
    ".tile.ton .tile-val{color:var(--g6);}"
    ".tile.toff .tile-val{color:var(--r6);}"
    ".tile-lbl{font-size:.74rem;color:var(--gr500);margin-top:5px;}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;}"
    ".metric{background:var(--g5);border:1px solid #bbf7d0;border-radius:6px;padding:12px;text-align:center;}"
    ".metric .val{font-size:1.8rem;font-weight:bold;color:var(--g7);}"
    ".metric .val.on{color:var(--g6);}"
    ".metric .val.off{color:var(--r6);}"
    ".metric .lbl{font-size:.75rem;color:var(--gr500);margin-top:4px;}"
    "label{font-size:.85rem;color:var(--gr700);display:block;margin-bottom:2px;}"
    "input{background:#fff;color:var(--gr900);border:1px solid #d1d5db;border-radius:4px;"
    "padding:6px 10px;width:100%;box-sizing:border-box;margin-bottom:8px;}"
    "input:focus{outline:none;border-color:var(--g7);}"
    ".btn{background:var(--g7);color:#fff;border:none;border-radius:6px;padding:8px 20px;"
    "cursor:pointer;font-size:.9rem;font-weight:600;}"
    ".btn:hover{background:var(--g8);}"
    ".on{color:var(--g6);}.off{color:var(--r6);}"
    ".ok-box{background:#f0fdf4;color:#166534;border:1px solid #bbf7d0;border-radius:4px;"
    "padding:8px 12px;margin-bottom:12px;display:none;}"
    ".hint{font-size:.78rem;color:var(--gr500);margin-top:-4px;margin-bottom:8px;}"
    ".form-row{margin-bottom:14px;}"
    "@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}";

// ---------------------------------------------------------------------------
// Nav HTML fragment (shared by all pages)
// ---------------------------------------------------------------------------
static const char KEB_NAV[] =
    "<div class=hdr><h1>&#9889; Kill Energy Bill</h1>"
    "<nav>"
    "<a href='/'>Home</a>"
    "<a href='/keb/cfg'>Instellingen</a>"
    "<a href='/obk'>OpenBK</a>"
    "</nav></div>";

// ---------------------------------------------------------------------------
// Dashboard page HTML (SPA — live data via /api/state)
// ---------------------------------------------------------------------------
static int keb_serve_dashboard(http_request_t *request) {
    http_setup(request, httpMimeTypeHTML);
    poststr(request,
        "<!DOCTYPE html><html><head>"
        "<meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta name=theme-color content=#15803d>"
        "<title>Kill Energy Bill</title>"
        "<style>");
    poststr(request, KEB_CSS);
    poststr(request, "</style></head><body>");
    poststr(request, KEB_NAV);
    poststr(request,
        "<div class=content>"

        // Hero card
        "<div class='hero ok' id=hero>"
        "<div class=hero-status>"
        "<span class=hero-dot></span>"
        "<span class=hero-title id=htitle>--</span>"
        "</div>"
        "<div class=hero-sub id=hsub>Even laden&hellip;</div>"
        "<div class=hero-power id=hpw>-- W</div>"
        "<div class=hero-meta>Maandpiek: <span id=hmp>--</span></div>"
        "<div class=bar-row>"
        "<div class=bar-track><div class=bar-fill id=hbar style='width:0'></div></div>"
        "<span class=bar-pct id=hpct>--</span>"
        "</div></div>"

        // Metric tiles
        "<div class=tile-row>"
        "<div class=tile id=relayTile>"
        "<div class=tile-val id=relayVal>--</div>"
        "<div class=tile-lbl>Apparaat</div>"
        "</div>"
        "<div class=tile>"
        "<div class=tile-val id=qpVal>--</div>"
        "<div class=tile-lbl>Kwartierpiek</div>"
        "</div>"
        "</div>"

        // Details card
        "<div class=card>"
        "<h2>Details</h2>"
        "<div class=grid>"
        "<div class=metric><div class=val id=mpVal>--</div><div class=lbl>Maandpiek</div></div>"
        "<div class=metric><div class=val id=thrVal>--</div><div class=lbl>Drempel</div></div>"
        "<div class=metric><div class=val id=p1Val>--</div><div class=lbl>P1 Meter</div></div>"
        "</div></div>"

        "</div>" // .content

        "<script>"
        "function fmt(w){"
        "if(w>=1000)return(w/1000).toFixed(2).replace('.',',')+' kW';"
        "return w+' W';}"
        "function upd(d){"
        "var thr=d.threshold_w||0;"
        "var pct=thr>0?Math.min(100,Math.round(d.quarter_peak_w/thr*100)):0;"
        "var shed=d.shed_active;"
        "var cls,title,sub;"
        "if(shed){cls='crit';title='Ingegrepen';sub='Apparaat uitgeschakeld om piek te vermijden';}"
        "else if(pct>=90){cls='warn';title='Let op';sub='Verbruik nadert drempel — '+pct+'% bereikt';}"
        "else{cls='ok';title='Alles goed';sub='Uw verbruik is veilig';}"
        "document.getElementById('hero').className='hero '+cls;"
        "document.getElementById('htitle').textContent=title;"
        "document.getElementById('hsub').textContent=sub;"
        "document.getElementById('hpw').textContent=fmt(d.power_w);"
        "document.getElementById('hmp').textContent=fmt(d.monthly_peak_w);"
        "document.getElementById('hbar').style.width=pct+'%';"
        "document.getElementById('hpct').textContent=pct+'%';"
        "document.getElementById('qpVal').textContent=fmt(d.quarter_peak_w);"
        "document.getElementById('mpVal').textContent=fmt(d.monthly_peak_w);"
        "document.getElementById('thrVal').textContent=fmt(thr);"
        "var rt=document.getElementById('relayTile');"
        "var rv=document.getElementById('relayVal');"
        "if(d.relay_on){rt.className='tile ton';rv.textContent='AAN';}"
        "else{rt.className='tile toff';rv.textContent='UIT';}"
        "var p1=document.getElementById('p1Val');"
        "if(d.p1_alive){p1.textContent=d.p1_host||'OK';p1.className='val on';}"
        "else{p1.textContent=d.p1_host?'Geen data':'Niet geconfigureerd';p1.className='val off';}"
        "}"
        "function poll(){fetch('/api/state').then(function(r){return r.json();}).then(upd).catch(function(){});}"
        "setInterval(poll,2000);"
        "poll();"
        "</script>"
        "</body></html>");
    poststr(request, NULL);
    return 0;
}

// ---------------------------------------------------------------------------
// Config page HTML
// ---------------------------------------------------------------------------
static int keb_serve_config(http_request_t *request) {
    http_setup(request, httpMimeTypeHTML);
    poststr(request,
        "<!DOCTYPE html><html><head>"
        "<meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta name=theme-color content=#15803d>"
        "<title>Instellingen — Kill Energy Bill</title>"
        "<style>");
    poststr(request, KEB_CSS);
    poststr(request, "</style></head><body>");
    poststr(request, KEB_NAV);
    poststr(request,
        "<div class=content>"
        "<div class=card>"
        "<h2>Piekbewaking</h2>"
        "<div id=ok-box class=ok-box>&#10003; Opgeslagen</div>"
        "<div class=form-row>"
        "<label>Buffer boven maandpiek (W)</label>"
        "<input type=number id=buf min=0 max=2000>"
        "<div class=hint>Extra marge boven maandpiek voordat ingegrepen wordt. Aanbevolen: 0&ndash;200 W.</div>"
        "</div>"
        "<div class=form-row>"
        "<label>Hysteresis (W)</label>"
        "<input type=number id=hys min=0 max=500>"
        "<div class=hint>Verschil waarmee verbruik onder drempel moet dalen voordat apparaat terug inschakelt.</div>"
        "</div>"
        "<div class=form-row>"
        "<label>Cascade prioriteit (1&ndash;16)</label>"
        "<input type=number id=prio min=1 max=16>"
        "<div class=hint>1 = eerste plug die uitgeschakeld wordt.</div>"
        "</div>"
        "</div>"
        "<div class=card>"
        "<h2>P1-dongle</h2>"
        "<div class=form-row>"
        "<label>IP-adres of hostnaam</label>"
        "<input type=text id=p1h placeholder='bijv. 192.168.1.42'>"
        "<div class=hint>HomeWizard P1 meter op uw netwerk.</div>"
        "</div>"
        "</div>"
        "<button class=btn onclick='save()'>Opslaan</button>"
        "</div>"

        "<script>"
        // Load current values from /api/state on page load
        "fetch('/api/state').then(function(r){return r.json();}).then(function(d){"
        "document.getElementById('buf').value=d.buffer_w||0;"
        "document.getElementById('hys').value=d.hysteresis_w||50;"
        "document.getElementById('prio').value=d.priority||5;"
        "document.getElementById('p1h').value=d.p1_host||'';"
        "}).catch(function(){});"

        // Send form-encoded POST
        "function save(){"
        "var body=["
        "'buffer_w='+encodeURIComponent(document.getElementById('buf').value),"
        "'hysteresis_w='+encodeURIComponent(document.getElementById('hys').value),"
        "'priority='+encodeURIComponent(document.getElementById('prio').value),"
        "'p1_host='+encodeURIComponent(document.getElementById('p1h').value)"
        "].join('&');"
        "fetch('/api/config',{"
        "method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:body"
        "}).then(function(r){return r.json();})"
        ".then(function(r){"
        "if(r.ok){"
        "var b=document.getElementById('ok-box');b.style.display='block';"
        "setTimeout(function(){b.style.display='none';},3000);"
        "}"
        "}).catch(function(){});"
        "}"
        "</script>"
        "</body></html>");
    poststr(request, NULL);
    return 0;
}

// ---------------------------------------------------------------------------
// /api/state — JSON response
// ---------------------------------------------------------------------------
static int keb_api_state(http_request_t *request) {
    int power_w   = KillBill_GetLastPowerW();
    int qp_w      = KillBill_GetQuarterPeakW();
    int mp_w      = KillBill_GetMonthlyPeakW();
    int buf_w     = KillBill_GetBufferW();
    int hyst_w    = KillBill_GetHysteresisW();
    bool shed     = KillBill_IsShedActive();
    bool relay_on = keb_relay_get(1);
    bool p1_alive = P1_IsAlive();
    const char *p1host = P1_GetHost();

    int32_t prio = 5;
    keb_cfg_get_i32("keb_priority", &prio);

    int monthly_eff = (mp_w < 2500) ? 2500 : mp_w;
    int threshold   = monthly_eff + buf_w;

    // Emit JSON as multiple poststr segments to avoid a single large snprintf buffer
    http_setup(request, httpMimeTypeJson);
    poststr(request, "{");
    hprintf255(request, "\"power_w\":%d,\"quarter_peak_w\":%d,", power_w, qp_w);
    hprintf255(request, "\"monthly_peak_w\":%d,\"threshold_w\":%d,", mp_w, threshold);
    hprintf255(request, "\"buffer_w\":%d,\"hysteresis_w\":%d,", buf_w, hyst_w);
    hprintf255(request, "\"priority\":%d,", (int)prio);
    hprintf255(request, "\"shed_active\":%s,\"relay_on\":%s,",
               shed ? "true" : "false",
               relay_on ? "true" : "false");
    hprintf255(request, "\"p1_alive\":%s,\"p1_host\":\"%s\",",
               p1_alive ? "true" : "false",
               (p1host && p1host[0]) ? p1host : "");
    hprintf255(request, "\"firmware_version\":\"%s\",", KEB_BK_FIRMWARE_VERSION);
    hprintf255(request, "\"features\":{\"cascade\":%s,\"advanced_scheduling\":%s,\"premium_reports\":%s}}",
               keb_feature_enabled("cascade")             ? "true" : "false",
               keb_feature_enabled("advanced_scheduling")  ? "true" : "false",
               keb_feature_enabled("premium_reports")      ? "true" : "false");
    poststr(request, NULL);
    return 0;
}

// ---------------------------------------------------------------------------
// /api/config — POST handler (form-encoded body)
// ---------------------------------------------------------------------------
static int keb_api_config_save(http_request_t *request) {
    const char *body = request->bodystart;
    char tmp[64];

    if (!body || request->bodylen <= 0) {
        http_setup(request, httpMimeTypeJson);
        poststr(request, "{\"ok\":false,\"error\":\"no body\"}");
        poststr(request, NULL);
        return 0;
    }

    // buffer_w — delegate to KEB_SetBuffer which updates g_buffer_w and persists
    if (http_getRawArg(body, "buffer_w", tmp, sizeof(tmp)) && atoi(tmp) >= 0)
        CMD_ExecuteCommandArgs("KEB_SetBuffer", tmp, 0);

    // hysteresis_w
    if (http_getRawArg(body, "hysteresis_w", tmp, sizeof(tmp)) && atoi(tmp) >= 0)
        CMD_ExecuteCommandArgs("KEB_SetHysteresis", tmp, 0);

    // priority — no dedicated command; persist directly
    if (http_getRawArg(body, "priority", tmp, sizeof(tmp))) {
        int v = atoi(tmp);
        if (v >= 1 && v <= 16)
            keb_cfg_set_i32("keb_priority", (int32_t)v);
    }

    // p1_host — delegate to KEB_SetP1Host
    if (http_getRawArg(body, "p1_host", tmp, sizeof(tmp)) && tmp[0])
        CMD_ExecuteCommandArgs("KEB_SetP1Host", tmp, 0);

    http_setup(request, httpMimeTypeJson);
    poststr(request, "{\"ok\":true}");
    poststr(request, NULL);
    return 0;
}

// ---------------------------------------------------------------------------
// Init — called from KillBill_Init() after sub-drivers are ready
// ---------------------------------------------------------------------------
void KillBill_WebUI_Init(void) {
    HTTP_RegisterCallback("/",           HTTP_GET,  keb_serve_dashboard,  0);
    HTTP_RegisterCallback("/keb/cfg",    HTTP_GET,  keb_serve_config,     0);
    HTTP_RegisterCallback("/api/state",  HTTP_GET,  keb_api_state,        0);
    HTTP_RegisterCallback("/api/config", HTTP_POST, keb_api_config_save,  0);
    keb_log("WEBUI", "routes registered: / /keb/cfg /api/state /api/config");
}

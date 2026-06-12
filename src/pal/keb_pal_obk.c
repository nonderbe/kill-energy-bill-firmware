// Kill Energy Bill — PAL implementation for OpenBK (BK7231N + ESP32)
//
// All KEB business-logic modules include keb_pal.h and call these functions.
// No Arduino types (String, millis, Serial) appear outside this file.

#include "../new_common.h"
#include "keb_pal.h"
#include "../new_pins.h"
#include "../logging/logging.h"
#include "../cmnds/cmd_public.h"
#if ENABLE_LITTLEFS
#include "../littlefs/our_lfs.h"
#endif
#include "../obk_config.h"
#if ENABLE_SEND_POSTANDGET
#include "../httpclient/http_client.h"
#endif
#include "../quicktick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ---- Time ------------------------------------------------------------------

uint32_t keb_millis(void) {
    return (uint32_t)g_timeMs;
}

// ---- Logging ---------------------------------------------------------------

void keb_log(const char *module, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    addLogAdv(LOG_INFO, LOG_FEATURE_DRV, "[%s] %s", module, buf);
}

// ---- Config (LittleFS, one file per key at /keb/<key>) --------------------
// Stored as a plain text file containing the value string.
// Max key length: 32 chars.  Max value length: 64 chars.

#define KEB_CFG_PATH_MAX 48

static void keb_cfg_path(const char *key, char *out) {
    snprintf(out, KEB_CFG_PATH_MAX, "/keb/%s", key);
}

bool keb_cfg_get_str(const char *key, char *out, size_t len) {
    char path[KEB_CFG_PATH_MAX];
    keb_cfg_path(key, path);
    byte *raw = LFS_ReadFile(path);
    if (!raw) return false;
    strncpy(out, (const char *)raw, len - 1);
    out[len - 1] = '\0';
    // trim trailing newline written by keb_cfg_set_str
    size_t n = strlen(out);
    if (n > 0 && out[n - 1] == '\n') out[n - 1] = '\0';
    free(raw);
    return true;
}

bool keb_cfg_set_str(const char *key, const char *val) {
    char path[KEB_CFG_PATH_MAX];
    keb_cfg_path(key, path);
    char buf[66];
    snprintf(buf, sizeof(buf), "%s\n", val);
#if ENABLE_LITTLEFS
    // Ensure /keb/ directory exists — lfs_mkdir returns LFS_ERR_EXIST when it
    // already exists, which is fine to ignore.
    lfs_mkdir(&lfs, "/keb");
#endif
    return LFS_WriteFile(path, (const byte *)buf, (int)strlen(buf), false) == 0;
}

bool keb_cfg_get_i32(const char *key, int32_t *out) {
    char buf[24];
    if (!keb_cfg_get_str(key, buf, sizeof(buf))) return false;
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf) return false;
    *out = (int32_t)v;
    return true;
}

bool keb_cfg_set_i32(const char *key, int32_t val) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", (int)val);
    return keb_cfg_set_str(key, buf);
}

// ---- Relay -----------------------------------------------------------------

void keb_relay_set(int ch, bool on) {
    CHANNEL_Set(ch, on ? 1 : 0, 0);
}

bool keb_relay_get(int ch) {
    return CHANNEL_Get(ch) != 0;
}

// ---- Async HTTP GET --------------------------------------------------------
// Only available on platforms that include the httpclient stack.

#if ENABLE_SEND_POSTANDGET

#define KEB_HTTP_BODY_SIZE 4096

typedef struct {
    keb_http_cb  cb;
    void        *user;
} keb_http_ctx_t;

static int keb_http_callback(httprequest_t *req) {
    keb_http_ctx_t *ctx = (keb_http_ctx_t *)req->usercontext;
    if (!ctx) return 0;

    // state 2 = complete, negative = error
    if (req->state == 2) {
        ctx->cb(200, req->client_data.response_buf, ctx->user);
    } else if (req->state < 0) {
        ctx->cb(req->state, NULL, ctx->user);
    } else {
        return 0; // still in progress
    }

    free(ctx);
    req->usercontext = NULL;
    // body_buf freed automatically via HTTPREQUEST_FLAG_FREE_RESPONSEBUF
    return 0;
}

void keb_http_get(const char *url, int timeout_ms, keb_http_cb cb, void *user) {
    (void)timeout_ms; // OpenBK async client has its own timeout

    char *url_copy = strdup(url);
    if (!url_copy) { cb(-1, NULL, user); return; }

    char *body_buf = (char *)malloc(KEB_HTTP_BODY_SIZE);
    if (!body_buf) {
        free(url_copy);
        cb(-1, NULL, user);
        return;
    }
    body_buf[0] = '\0';

    keb_http_ctx_t *ctx = (keb_http_ctx_t *)malloc(sizeof(keb_http_ctx_t));
    if (!ctx) {
        free(url_copy);
        free(body_buf);
        cb(-1, NULL, user);
        return;
    }
    ctx->cb   = cb;
    ctx->user = user;

    httprequest_t *req = (httprequest_t *)malloc(sizeof(httprequest_t));
    if (!req) {
        free(url_copy);
        free(body_buf);
        free(ctx);
        cb(-1, NULL, user);
        return;
    }
    memset(req, 0, sizeof(*req));
    req->url                         = url_copy;
    req->method                      = HTTPCLIENT_GET;
    // httpclient_parse_host() compares against uppercase "HTTP://" — case-sensitive —
    // so lowercase http:// URLs leave req->port = 0, which causes connect() to port 0.
    // Pre-populate the port here so the async thread starts with the right default.
    if (strncmp(url, "https://", 8) == 0) req->port = 443;
    else if (strncmp(url, "http://", 7) == 0) req->port = 80;
    // FREE_URLONDONE: library will call free(req->url) after callback
    req->flags                       = HTTPREQUEST_FLAG_FREE_SELFONDONE |
                                       HTTPREQUEST_FLAG_FREE_URLONDONE  |
                                       HTTPREQUEST_FLAG_FREE_RESPONSEBUF;
    req->client_data.response_buf     = body_buf;
    req->client_data.response_buf_len = KEB_HTTP_BODY_SIZE;
    req->data_callback               = keb_http_callback;
    req->usercontext                 = ctx;

    HTTPClient_Async_SendGeneric(req);
}

#else // !ENABLE_SEND_POSTANDGET

void keb_http_get(const char *url, int timeout_ms, keb_http_cb cb, void *user) {
    (void)url; (void)timeout_ms;
    cb(-1, NULL, user); // HTTP not supported on this platform
}

#endif // ENABLE_SEND_POSTANDGET

// ---- Network ---------------------------------------------------------------

#if !WINDOWS
#include <lwip/netif.h>

uint32_t keb_get_ipv4(void) {
    if (!netif_default) return 0;
    return netif_default->ip_addr.addr;
}
#else
uint32_t keb_get_ipv4(void) { return 0; }
#endif

// ---- Background task -------------------------------------------------------

typedef struct {
    keb_task_fn  fn;
    void        *arg;
} keb_bg_args_t;

// beken_thread_function_t is void(*)(void*) on ESP32 IDF and void(*)(void*) on BK7231N.
// Returning void matches the type directly on ESP-IDF; BK7231N ignores the return value.
static void keb_bg_entry(void *raw) {
    keb_bg_args_t *a = (keb_bg_args_t *)raw;
    a->fn(a->arg);
    free(a);
    rtos_delete_thread(NULL);
}

void keb_run_in_background(const char *name, keb_task_fn fn, void *arg) {
    keb_bg_args_t *a = (keb_bg_args_t *)malloc(sizeof(keb_bg_args_t));
    if (!a) return;
    a->fn  = fn;
    a->arg = arg;
    rtos_create_thread(NULL, BEKEN_APPLICATION_PRIORITY, name,
                       (beken_thread_function_t)keb_bg_entry,
                       4096, (beken_thread_arg_t)a);
}

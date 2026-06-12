#pragma once
// Kill Energy Bill — Platform Abstraction Layer
// One implementation: keb_pal_obk.c (OpenBK, works for both BK7231N and ESP32 targets)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- Time ---
// Returns milliseconds since boot (wraps ~every 49 days).
uint32_t keb_millis(void);

// --- Logging ---
// module: short tag ("PKG", "P1", "OTA", ...).  fmt: printf-style.
void keb_log(const char *module, const char *fmt, ...);

// --- Persistent config ---
// Keys are short ASCII strings (max 32 chars).
// Values are UTF-8 strings (max 64 chars for str; i32 stored as decimal).
// Returns false when the key does not exist or the value cannot be parsed.
bool keb_cfg_get_str(const char *key, char *out, size_t len);
bool keb_cfg_set_str(const char *key, const char *val);
bool keb_cfg_get_i32(const char *key, int32_t *out);
bool keb_cfg_set_i32(const char *key, int32_t val);

// --- Relay ---
// ch: OpenBK channel index (1-based, matches the pin-role assignment).
void keb_relay_set(int ch, bool on);
bool keb_relay_get(int ch);

// --- Async HTTP GET ---
// cb is called exactly once: on success (status=200, body=response text)
// or on failure (status<0 for network error, body=NULL).
// user is passed through unchanged to the callback.
typedef void (*keb_http_cb)(int status, const char *body, void *user);
void keb_http_get(const char *url, int timeout_ms, keb_http_cb cb, void *user);

// --- Background task ---
// Spawns a new FreeRTOS/RTOS task that calls fn(arg) once then exits.
// Stack size is fixed at 4 KB — suitable for blocking I/O (mDNS discovery, etc.).
// fn must be safe to call from a task context; arg ownership transfers to fn.
typedef void (*keb_task_fn)(void *arg);
void keb_run_in_background(const char *name, keb_task_fn fn, void *arg);

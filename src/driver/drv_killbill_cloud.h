// Kill Energy Bill — cloud feature-flag driver for OpenBK (BK7231N + ESP32)
//
// Polls /api/v1/features on the OTA server every 24 hours and caches the
// result in RAM.  The cloud is always the source of truth; no NVS persistence.
//
// Default (until first successful poll): free tier (cascade=true, rest false).
// If the server is unreachable, the cached/default flags remain in effect.

#pragma once
#include <stdbool.h>

typedef struct {
    bool cascade;
    bool advanced_scheduling;
    bool premium_reports;
} KebFeatures;

void KebCloud_Init(void);
void KebCloud_Update(void);           // call every second from KillBill_OnEverySecond()
bool keb_feature_enabled(const char *name);
const KebFeatures *keb_features(void);

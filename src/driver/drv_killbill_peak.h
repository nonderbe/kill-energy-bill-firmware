#pragma once

#include "../httpserver/new_http.h"

// Kill Energy Bill — peak guard driver
// Monitors P1 net power and sheds relay when quarter-peak approaches monthly peak.
// Ported from kill-energy-bill Arduino firmware (peak_guard.cpp).

void KillBill_Init(void);
void KillBill_OnEverySecond(void);
void KillBill_AppendInformationToHTTPIndexPage(http_request_t *request, int bPreState);
void KillBill_StopDriver(void);
// Called by P1 reader to push a live power measurement (W).
void KillBill_SetPowerW(int power_w);
// Called by P1 reader or coordinator to update monthly peak (W).
void KillBill_UpdateMonthlyPeakW(int w);
// State getters used by the web UI driver.
int  KillBill_GetMonthlyPeakW(void);
int  KillBill_GetQuarterPeakW(void);
int  KillBill_GetLastPowerW(void);
int  KillBill_GetBufferW(void);
int  KillBill_GetHysteresisW(void);
bool KillBill_IsShedActive(void);

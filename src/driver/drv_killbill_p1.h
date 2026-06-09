#pragma once

// Kill Energy Bill — HomeWizard P1 meter poller
// Polls /api/v1/data (5 s) and /api/v1/telegram (60 s).
// Feeds power to KillBill_SetPowerW() and monthly peak to KillBill_UpdateMonthlyPeakW().
// Called from KillBill_Init() / KillBill_OnEverySecond().

void P1_Init(void);
void P1_Update(void);
// Returns the configured P1 host string (empty if not set).
const char *P1_GetHost(void);
// Returns true if a successful response was received in the last 30 s.
bool P1_IsAlive(void);

#pragma once

// Kill Energy Bill — branded web UI (OpenBK port)
// Registers HTTP routes for the KEB dashboard, config, log and state API.
// Call KillBill_WebUI_Init() from KillBill_Init() after all other sub-drivers
// are initialised.

void KillBill_WebUI_Init(void);

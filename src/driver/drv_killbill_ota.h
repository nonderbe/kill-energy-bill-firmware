// Kill Energy Bill — OTA update driver for OpenBK (BK7231N + ESP32)
//
// Polls a version manifest, verifies Ed25519 signature over SHA-256 of the
// firmware binary, and performs the OTA update if a newer signed version is
// available.  Uses HTTP (not HTTPS) because mbedTLS does not fit on BK7231N.
// The Ed25519 signature proves the binary is authentic even over plain HTTP.

#pragma once

// Bump this for each release. The OTA server rejects downgrades.
#ifndef KEB_BK_FIRMWARE_VERSION
#define KEB_BK_FIRMWARE_VERSION "0.9.1"
#endif

void OTA_Init(void);
void OTA_Update(void);  // call every second from KillBill_OnEverySecond()

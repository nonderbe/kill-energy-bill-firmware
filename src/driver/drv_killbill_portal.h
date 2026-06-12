#pragma once
// Kill Energy Bill — Captive portal for AP mode
//
// Implements the same four-layer approach as the original Arduino firmware:
//   1. DNS server on port 53  — redirects all A queries to 192.168.4.1
//   2. TCP/443 intercept      — reads TLS ClientHello, replies plain HTTP 302
//   3. HTTP probe handlers    — /generate_204, /.well-known/captive-portal,
//                               Apple/Microsoft probes
//
// Call KillBill_Portal_Init() from KillBill_Init() at startup.
// The portal runs permanently; it has no effect once STA has an IP because
// no new clients will probe it.

void KillBill_Portal_Init(void);

#pragma once
// Kill Energy Bill — mDNS helpers
//
// P1 discovery: queries _hwenergy._tcp then _esphome._tcp + HTTP probe.
// Hostname resolution: probes for name conflicts and picks a unique suffix.
//
// Uses the QU (unicast-response) bit so all responses arrive on an ephemeral
// port — no conflict with the lwIP mDNS responder on port 5353.
//
// All functions are blocking; call from a dedicated FreeRTOS task.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---- P1 meter discovery ----------------------------------------------------

// Discover the first reachable P1 meter on the LAN.
// On success: writes the IP string and port to host_out/port_out, returns true.
// On failure (nothing found within timeout): returns false.
bool keb_mdns_discover_p1(char *host_out, size_t host_len, uint16_t *port_out);

// ---- Hostname conflict resolution ------------------------------------------

// Check whether hostname (without .local) is already claimed by another device.
// own_ip: device's own IPv4 (network byte order) — responses from this IP are
// treated as self-responses and do not count as conflicts.
// Returns true if a conflict is detected (another device holds the name).
bool keb_mdns_name_conflict(const char *hostname, uint32_t own_ip);

// Find the lowest-numbered unique variant of base_name on the LAN:
// tries base_name, base_name-1, base_name-2 … base_name-9.
// Writes the chosen name (without .local suffix) to out.
// own_ip: as above — responses from this IP are treated as self (no conflict).
// Always writes something; falls back to base_name if all variants are claimed.
void keb_mdns_resolve_hostname(const char *base_name, uint32_t own_ip,
                               char *out, size_t out_len);

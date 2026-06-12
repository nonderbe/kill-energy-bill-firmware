#pragma once
// Kill Energy Bill — mDNS P1 meter discovery
//
// Queries _hwenergy._tcp (HomeWizard) then _esphome._tcp (Slimmelezer / ESPHome).
// Uses the QU (unicast-response) bit so responses arrive on an ephemeral port,
// avoiding any conflict with the existing lwIP mDNS responder on port 5353.
//
// Blocking — intended to be called from a dedicated FreeRTOS task.
// Maximum total wall-clock time: ~7 s (2 s per service type + 3 s HTTP probe).

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Discover the first reachable P1 meter on the LAN.
// On success: writes the IP string and port to host_out/port_out, returns true.
// On failure (nothing found within timeout): returns false.
bool keb_mdns_discover_p1(char *host_out, size_t host_len, uint16_t *port_out);

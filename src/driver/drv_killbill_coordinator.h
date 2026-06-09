#pragma once

// Kill Energy Bill — multi-plug cascade coordinator (OpenBK port)
// Subscribes to killbill/coordinator/peak, tracks peer plugs, and feeds
// the highest known monthly peak back into the peak guard.

void Coordinator_Init(void);
void Coordinator_OnEverySecond(void);

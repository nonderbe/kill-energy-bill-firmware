# Kill Energy Bill — OpenBK7231N Fork

This is a fork of [openshwprojects/OpenBK7231T_App](https://github.com/openshwprojects/OpenBK7231T_App)
customised for the **Kill Energy Bill** smart plug firmware.

## Purpose

The Kill Energy Bill plug monitors P1 DSMR power consumption and sheds loads
to avoid Fluvius capacity tariff peaks (Belgian residential tariff based on
monthly quarter-hour peak in kW).

## Build targets

| Target | Output | Use |
|---|---|---|
| `OpenBK7231N` | `_QIO.bin` + `_UG.rbl` | Production — Tuya T1 / BK7231N hardware |
| `OpenESP32` | `_ESP32_4M.factory.bin` | Development & testing on ESP32 devkit via USB |

## Kill Energy Bill drivers (added in `src/driver/`)

| File | Status | Description |
|---|---|---|
| `drv_killbill_peak.c` | WIP | Rolling 15-min quarter-peak guard + relay cascade |
| `drv_killbill_p1.c` | planned | HomeWizard P1 meter HTTP polling + mDNS discovery |
| `drv_killbill_ota.c` | planned | Ed25519-signed OTA via kill-energy-bill.com |
| `drv_killbill_cloud.c` | planned | Cloud registration + feature flags |

## Algorithm

```
quarter_peak  = rolling 15-min average of P1 net power (W)
monthly_peak  = OBIS 1-0:1.6.0 from P1 meter (never locally tracked)
threshold     = monthly_peak + buffer_w  (default: 0 W)

if quarter_peak >= threshold:
    switch lowest-priority relay OFF   (cascade)
if quarter_peak < (threshold - hysteresis_w):
    switch highest-priority relay ON   (restore)
```

Minimum monthly peak: 2500 W (Flemish exemption — Vlaamse vrijstelling).

## Upstream sync policy

This fork is the production base. Do **not** merge back into upstream.
Upstream fixes can be cherry-picked individually.

## Local ESP32 build (requires ESP-IDF v5.5.2)

```bash
git clone --branch v5.5.2 --depth 1 --recurse-submodules \
  https://github.com/espressif/esp-idf sdk/esp-idf
cd sdk/esp-idf && ./install.sh && cd ../..
. sdk/esp-idf/export.sh
make APP_VERSION=dev APP_NAME=kill-energy-bill-firmware VARIANT=4M OpenESP32
```

Output: `output/dev_4M/OpenESP32_dev_4M.factory.bin`
Flash: `python3 -m esptool --chip esp32 --port /dev/cu.usbserial-XXXX --baud 460800 write_flash --flash_size 4MB 0x0 <binary>`

// debug_flag.h — a single global "verbose debug" toggle, settable from the web (Settings page
// → config_store → the "set_verbose_debug" BLE command) and read by anything that wants to
// gate optional serial-log spam behind it (currently: drv_vl53l1x.c/drv_vl53l0x.c range
// diagnostics, hid_host.c's first-report hex dumps).
//
// Lives in its own dependency-free component rather than in sensor.c or config_store.c: sensor
// already REQUIRES hid_host (drv_gamepad.c reads its state), and config_store REQUIRES sensor —
// so neither hid_host nor config_store can REQUIRE sensor back without a circular component
// dependency. A tiny leaf component both sides can depend on directly sidesteps that.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool debug_flag_get(void);
void debug_flag_set(bool on);

#ifdef __cplusplus
}
#endif

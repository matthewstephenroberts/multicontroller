#include "debug_flag.h"

// volatile: set from the BLE command-handling task, read from the polling task — a plain
// bool read/write is atomic on this single-core-per-value access pattern, no lock needed for
// a flag that only ever flips between two states.
static volatile bool s_on;

bool debug_flag_get(void) { return s_on; }
void debug_flag_set(bool on) { s_on = on; }

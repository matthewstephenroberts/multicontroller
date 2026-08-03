// Nordic-UART-style service used by the MultiController firmware.
// Must match firmware/components/ble_svc/ble_svc.c.
export const NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
export const NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // host -> device (write)
export const NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // device -> host (notify)

// main.js — MultiController desktop wrapper. Loads the same web UI used in the browser inside
// a native window, and answers Electron's Bluetooth device-chooser prompt: Web Bluetooth in
// Electron has no built-in native picker the way Chrome's own browser UI does — the app must
// supply one via webContents' `select-bluetooth-device` event.
const { app, BrowserWindow, ipcMain } = require("electron");
const path = require("path");

// Must be set before app.whenReady() to take effect. Observed on macOS: Chromium's Bluetooth
// scan was using IOBluetoothDeviceInquiry (classic BR/EDR discovery) instead of CoreBluetooth
// LE scanning, which can never find a BLE-only peripheral like the board — these flags may
// route Electron onto the newer/correct BLE-scanning code path instead.
app.commandLine.appendSwitch("enable-experimental-web-platform-features");
app.commandLine.appendSwitch("enable-web-bluetooth");

// Set by scripts/dev-desktop.sh to point at the live Vite dev server instead of the bundled
// production build, so UI changes hot-reload without repackaging the app each time.
const DEV_SERVER_URL = process.env.MC_DEV_SERVER_URL;

let mainWindow;
let pickerWindow;
let pendingBluetoothCallback = null;
let scanTimeoutHandle = null;
let settleTimeoutHandle = null;
let latestDeviceList = [];

// How long to keep an ongoing scan alive with zero results before giving up. select-bluetooth-
// device fires repeatedly as Chromium's scan discovers devices, not once with a final list —
// cancelling on the very first (near-certainly empty) firing was aborting the scan before it
// had any real chance to find the board.
const SCAN_TIMEOUT_MS = 20000;

// How long to wait after the FIRST device appears before deciding auto-connect vs. picker.
// Devices are discovered one at a time across successive event firings — connecting the
// instant the list holds one entry would always grab whichever of two boards advertises
// first, and the picker would effectively never show. A short settle window lets any other
// board nearby turn up so the choice is real; one lone board just costs this small delay.
const DISCOVERY_SETTLE_MS = 1500;

function createMainWindow() {
  mainWindow = new BrowserWindow({
    width: 1100,
    height: 800,
    minWidth: 720,
    minHeight: 560,
    title: "MultiController",
    backgroundColor: "#0e1116",
    icon: path.join(__dirname, "build/icon.png"),
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  if (DEV_SERVER_URL) {
    mainWindow.loadURL(DEV_SERVER_URL);
  } else {
    mainWindow.loadFile(path.join(__dirname, "web-dist", "index.html"));
  }

  // Renderer-side failures (thrown before requestDevice() ever reaches the main process — a
  // missing navigator.bluetooth, a JS error in the click handler, etc.) never show up in this
  // process's own console.log output. Gated behind an env var so normal (kid) use never shows
  // a DevTools window — set MC_DEVTOOLS=1 when launching from a terminal to see it.
  if (process.env.MC_DEVTOOLS) {
    mainWindow.webContents.openDevTools({ mode: "detach" });
  }
  mainWindow.webContents.on("console-message", (_event, _level, message) => {
    console.log(`[renderer] ${message}`);
  });

  // Our own requestDevice() call already filters to the NUS service UUID (see bleClient.ts), so
  // in practice there's almost always exactly one candidate — the board itself. Auto-pick it so
  // kids never see a popup in the common case; only fall back to a tiny chooser window if more
  // than one MultiController board is advertising nearby.
  // NOTE: this is a webContents event, NOT a session event (unlike select-hid-device /
  // select-serial-port / select-usb-device, which live on session). Attaching it to
  // webContents.session is a silent no-op — no error, handler never registered — and Electron's
  // default behaviour then cancels every requestDevice() instantly with "User cancelled the
  // requestDevice() chooser".
  mainWindow.webContents.on("select-bluetooth-device", (event, deviceList, callback) => {
    event.preventDefault();
    console.log(`[bluetooth] select-bluetooth-device fired, ${deviceList.length} candidate(s)`);

    if (deviceList.length === 0) {
      // The scan is still running and just hasn't found anything YET — this event re-fires as
      // Chromium's discovery progresses, so don't cancel on an empty snapshot. Hold this
      // callback as the latest one to act on, and (re)start a timeout that only gives up after
      // a real window with sustained zero results, instead of the very first (near-certain)
      // empty firing.
      pendingBluetoothCallback = callback;
      if (scanTimeoutHandle) clearTimeout(scanTimeoutHandle);
      scanTimeoutHandle = setTimeout(() => {
        console.log(`[bluetooth] scan timed out after ${SCAN_TIMEOUT_MS}ms with no devices found — `
          + "check the board is powered/advertising, and macOS System Settings → Privacy & "
          + "Security → Bluetooth lists this app as allowed");
        scanTimeoutHandle = null;
        if (pendingBluetoothCallback) {
          pendingBluetoothCallback("");
          pendingBluetoothCallback = null;
        }
      }, SCAN_TIMEOUT_MS);
      return;
    }

    if (scanTimeoutHandle) { clearTimeout(scanTimeoutHandle); scanTimeoutHandle = null; }

    // At least one device found. Always track the freshest list + callback (each firing
    // supersedes the last), and if the picker is already open, live-update its list.
    pendingBluetoothCallback = callback;
    latestDeviceList = deviceList;
    if (pickerWindow) {
      pickerWindow.webContents.send(
        "device-list",
        deviceList.map((d) => ({ deviceId: d.deviceId, deviceName: d.deviceName || "Unknown device" })),
      );
      return;
    }

    // Don't decide on the first firing — wait a short settle window for any other boards to
    // be discovered, then: exactly one → auto-connect; several → show the picker.
    if (!settleTimeoutHandle) {
      settleTimeoutHandle = setTimeout(() => {
        settleTimeoutHandle = null;
        if (!pendingBluetoothCallback) return;
        if (latestDeviceList.length === 1) {
          console.log(`[bluetooth] auto-connecting to "${latestDeviceList[0].deviceName || latestDeviceList[0].deviceId}"`);
          const cb = pendingBluetoothCallback;
          pendingBluetoothCallback = null;
          cb(latestDeviceList[0].deviceId);
        } else {
          console.log(`[bluetooth] ${latestDeviceList.length} devices found — showing picker`);
          showDevicePicker(latestDeviceList);
        }
      }, DISCOVERY_SETTLE_MS);
    }
  });
}

function showDevicePicker(deviceList) {
  if (pickerWindow) pickerWindow.close();

  pickerWindow = new BrowserWindow({
    width: 360,
    height: 110 + deviceList.length * 46,
    parent: mainWindow,
    modal: true,
    resizable: false,
    title: "Select your MultiController board",
    webPreferences: {
      preload: path.join(__dirname, "picker-preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  pickerWindow.setMenuBarVisibility(false);
  pickerWindow.loadFile(path.join(__dirname, "device-picker.html"));
  pickerWindow.webContents.once("did-finish-load", () => {
    pickerWindow.webContents.send(
      "device-list",
      deviceList.map((d) => ({ deviceId: d.deviceId, deviceName: d.deviceName || "Unknown device" })),
    );
  });
  pickerWindow.on("closed", () => {
    // Closed via the OS window-close button rather than a Select/Cancel click — still have to
    // resolve the pending callback or Web Bluetooth's requestDevice() promise hangs forever.
    if (pendingBluetoothCallback) {
      pendingBluetoothCallback("");
      pendingBluetoothCallback = null;
    }
    pickerWindow = null;
  });
}

ipcMain.on("device-selected", (_event, deviceId) => {
  if (pendingBluetoothCallback) {
    pendingBluetoothCallback(deviceId);
    pendingBluetoothCallback = null;
  }
  if (pickerWindow) { pickerWindow.close(); pickerWindow = null; }
});

ipcMain.on("device-picker-cancelled", () => {
  if (pendingBluetoothCallback) {
    pendingBluetoothCallback("");
    pendingBluetoothCallback = null;
  }
  if (pickerWindow) { pickerWindow.close(); pickerWindow = null; }
});

app.whenReady().then(() => {
  createMainWindow();
  app.on("activate", () => {
    if (BrowserWindow.getAllWindows().length === 0) createMainWindow();
  });
});

app.on("window-all-closed", () => {
  if (process.platform !== "darwin") app.quit();
});

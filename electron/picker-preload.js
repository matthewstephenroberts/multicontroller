// picker-preload.js — bridges the tiny device-picker window (plain HTML/JS, no build step) to
// the main process over IPC, without giving that page direct Node/Electron access.
const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("picker", {
  onDeviceList: (cb) => ipcRenderer.on("device-list", (_event, list) => cb(list)),
  select: (deviceId) => ipcRenderer.send("device-selected", deviceId),
  cancel: () => ipcRenderer.send("device-picker-cancelled"),
});

// preload.js — runs in the main window's isolated preload context. The web app itself needs no
// privileged APIs (it just calls navigator.bluetooth like it would in a regular browser tab) —
// this file exists only so contextIsolation/nodeIntegration stay at their secure defaults
// without an empty webPreferences.preload causing Electron to warn about a missing file.

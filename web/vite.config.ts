import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Web Bluetooth requires a secure context: localhost is treated as secure, so
// `vite dev` / `vite preview` on localhost work without HTTPS.
export default defineConfig({
  plugins: [react()],
  server: { port: 5173, host: true },
  // Relative asset paths (not the default "/") so the production build also works loaded via
  // file:// (Electron's loadFile()) — an absolute "/assets/..." path resolves against the
  // filesystem root under file://, not the html file's own directory, so every script/css 404s
  // silently and the window just shows its background colour. Dev/preview (served over http)
  // aren't affected either way.
  base: "./",
});

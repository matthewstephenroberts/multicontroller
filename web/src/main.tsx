import React from "react";
import ReactDOM from "react-dom/client";
import { App } from "./App";
// Self-hosted via @fontsource (OFL-1.1 license, see THIRD_PARTY_NOTICES.md) rather than a
// Google Fonts CDN <link> — no external request at runtime, works offline, and the license terms
// ship in node_modules instead of depending on Google's CDN staying up. Only the weights actually
// used below are imported (each is a separate ~15-40KB woff2, not the whole family).
import "@fontsource/fredoka/500.css";
import "@fontsource/fredoka/600.css";
import "@fontsource/fredoka/700.css";
import "@fontsource/nunito/400.css";
import "@fontsource/nunito/600.css";
import "@fontsource/nunito/700.css";
import "@fontsource/nunito/800.css";
import "./styles.css";

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);

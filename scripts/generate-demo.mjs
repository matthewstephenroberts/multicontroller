#!/usr/bin/env node
// generate-demo.mjs — drives the web app's built-in demo mode (MockBleClient) with a headless
// browser and captures a longer walkthrough of every tab/feature as a sequence of PNG frames,
// then hands off to assemble-gif.py (Pillow) and ffmpeg to produce the GIF/WebM used in
// docs/hackster-article.md and the README. No real hardware or Bluetooth permission needed —
// same trick the original short demo used, just covering more of the app.
//
// Usage: node scripts/generate-demo.mjs [--light]
//   (no flag)  dark theme, frames -> scripts/_demo_frames        (used for the GIF/WebM)
//   --light    light theme, frames -> scripts/_demo_frames_light (better for print — the manual's
//              embedded screenshots use these instead of the dark GIF frames, since a dark UI
//              background burns far more ink/toner on a physical printout)
// Requires: puppeteer (root devDependency), python3 + Pillow (for the GIF), ffmpeg (for the WebM).

import { spawn } from "node:child_process";
import { mkdir, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import puppeteer from "puppeteer";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, "..");
const WEB_DIR = path.join(ROOT, "web");
const THEME = process.argv.includes("--light") ? "light" : "dark";
const FRAMES_DIR = path.join(ROOT, "scripts", THEME === "light" ? "_demo_frames_light" : "_demo_frames");
const ASSETS_DIR = path.join(ROOT, "docs", "assets");
const PORT = 5183;
const BASE_URL = `http://localhost:${PORT}`;

const VIEWPORT = { width: 1000, height: 680 };

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------------------------------------------------------------------------
// Dev server lifecycle
// ---------------------------------------------------------------------------

async function waitForServer(url, timeoutMs = 30000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    try {
      const res = await fetch(url);
      if (res.ok || res.status === 404) return; // 404 is fine, Vite is up
    } catch {
      // not up yet
    }
    await sleep(300);
  }
  throw new Error(`Dev server at ${url} did not become ready in time`);
}

function startDevServer() {
  const proc = spawn(
    "npx",
    ["vite", "--port", String(PORT), "--strictPort"],
    { cwd: WEB_DIR, stdio: ["ignore", "pipe", "pipe"] },
  );
  proc.stdout.on("data", () => {});
  proc.stderr.on("data", () => {});
  return proc;
}

// ---------------------------------------------------------------------------
// Puppeteer helpers
// ---------------------------------------------------------------------------

/** Click the first element matching `tag` whose trimmed textContent equals or contains `text`. */
async function clickByText(page, tag, text, { exact = false } = {}) {
  const handle = await page.evaluateHandle(
    (tag, text, exact) => {
      const els = Array.from(document.querySelectorAll(tag));
      const norm = (s) => (s || "").replace(/\s+/g, " ").trim();
      return (
        els.find((el) => (exact ? norm(el.textContent) === text : norm(el.textContent).includes(text))) || null
      );
    },
    tag,
    text,
    exact,
  );
  const element = handle.asElement();
  if (!element) {
    await handle.dispose();
    throw new Error(`clickByText: no <${tag}> containing "${text}"`);
  }
  await element.click();
  await element.dispose();
}

async function clickSelector(page, selector) {
  await page.waitForSelector(selector, { timeout: 5000 });
  await page.click(selector);
}

/** Click the "+ Add" button in the scan results row whose text contains `needle` (e.g. a sensor
 * guess like "m5_step16" or "as7341") — rows aren't otherwise addressable, since AddBtn has no
 * distinguishing id/class per device. */
async function clickAddForGuess(page, needle) {
  const handle = await page.evaluateHandle((needle) => {
    const rows = Array.from(document.querySelectorAll("table.grid tr"));
    const row = rows.find((r) => (r.textContent || "").includes(needle));
    if (!row) return null;
    const btns = Array.from(row.querySelectorAll("button"));
    return btns.find((b) => /add/i.test(b.textContent || "")) || null;
  }, needle);
  const btn = handle.asElement();
  if (!btn) {
    await handle.dispose();
    throw new Error(`no "+ Add" button in a row containing "${needle}"`);
  }
  await btn.click();
  await btn.dispose();
}

/** Click a sub-tab pill (the per-sensor or per-type strip in SensorConfigForm) inside the <nav>
 * with the given aria-label, matching by exact text or by prefix (sensor names from a scan carry
 * a generated `-<id>` suffix, so "startsWith" is what lets us target them without knowing the id). */
async function clickSubtab(page, ariaLabel, text, { startsWith = false } = {}) {
  const handle = await page.evaluateHandle(
    (ariaLabel, text, startsWith) => {
      const nav = document.querySelector(`nav[aria-label="${ariaLabel}"]`);
      if (!nav) return null;
      const btns = Array.from(nav.querySelectorAll("button"));
      return (
        btns.find((b) => {
          const t = (b.textContent || "").trim();
          return startsWith ? t.startsWith(text) : t === text;
        }) || null
      );
    },
    ariaLabel,
    text,
    startsWith,
  );
  const btn = handle.asElement();
  if (!btn) {
    await handle.dispose();
    throw new Error(`no subtab "${text}" in nav[aria-label="${ariaLabel}"]`);
  }
  await btn.click();
  await btn.dispose();
}

/** Expand a <details class="log"> section by clicking its <summary> if the summary's text
 * contains `text` — SensorConfigForm/LegoConfigForm's advanced sections (Binary maths, the two
 * hub-program blocks) render collapsed by default, unlike Word Blocks/live-packet which set the
 * `open` attribute themselves. */
async function expandDetails(page, text) {
  const handle = await page.evaluateHandle((text) => {
    const summaries = Array.from(document.querySelectorAll("details.log summary"));
    return summaries.find((s) => (s.textContent || "").includes(text)) || null;
  }, text);
  const summary = handle.asElement();
  if (!summary) {
    await handle.dispose();
    throw new Error(`no <details class="log"> summary containing "${text}"`);
  }
  await summary.click();
  await summary.dispose();
}

/** Scroll the app's one scrollable content region (.panel-scroll) — window-level scrolling does
 * nothing here since the app shell is a fixed-height flex column with its own inner scroller. */
async function scrollPanel(page, deltaY) {
  await page.evaluate((dy) => {
    document.querySelector(".panel-scroll")?.scrollBy({ top: dy, left: 0 });
  }, deltaY);
}

async function scrollPanelToTop(page) {
  await page.evaluate(() => {
    const el = document.querySelector(".panel-scroll");
    if (el) el.scrollTop = 0;
  });
}

async function tryStep(label, fn) {
  try {
    await fn();
  } catch (e) {
    console.warn(`  ⚠ skipped "${label}": ${e.message}`);
  }
}

// ---------------------------------------------------------------------------
// Frame capture manifest
// ---------------------------------------------------------------------------

class FrameRecorder {
  constructor(page, dir) {
    this.page = page;
    this.dir = dir;
    this.frames = [];
    this.n = 0;
  }

  /** Capture the current page state, held for `durationMs` when played back. */
  async capture(label, durationMs = 900) {
    this.n += 1;
    const file = `frame_${String(this.n).padStart(3, "0")}.png`;
    await this.page.screenshot({ path: path.join(this.dir, file) });
    this.frames.push({ file, durationMs, label });
    console.log(`  📸 ${file}  (${durationMs}ms)  ${label}`);
  }

  /** Capture several frames in a row, letting live/animating data (polling, charts) move. */
  async captureBurst(label, count, intervalMs, durationMs = 500) {
    for (let i = 0; i < count; i++) {
      await this.capture(`${label} (${i + 1}/${count})`, durationMs);
      if (i < count - 1) await sleep(intervalMs);
    }
  }

  async writeManifest() {
    await writeFile(
      path.join(this.dir, "manifest.json"),
      JSON.stringify({ frames: this.frames }, null, 2),
    );
  }
}

// ---------------------------------------------------------------------------
// The walkthrough itself
// ---------------------------------------------------------------------------

async function runWalkthrough(page, rec) {
  await page.goto(BASE_URL, { waitUntil: "networkidle0" });
  await sleep(400);

  // 1. Landing / welcome screen.
  await rec.capture("welcome screen", 1800);

  // 2. Enter demo mode.
  await clickByText(page, "button", "Try demo mode");
  await sleep(250);
  await rec.capture("connecting to demo device", 500);
  await page.waitForSelector(".tabbar", { timeout: 8000 });
  await sleep(300);
  await rec.capture("connected — demo badge + Sensors tab", 1800);

  // 3. Guide tab.
  await tryStep("open Guide tab", async () => {
    await clickByText(page, "button", "Guide");
    await sleep(300);
    await rec.capture("Guide tab overview", 1600);
  });

  // 4. Scan tab — empty, then scan, then add a sensor.
  await tryStep("open Scan tab", async () => {
    await clickByText(page, "button", "Scan");
    await sleep(300);
    await rec.capture("Scan tab — before scanning", 1000);
  });

  await tryStep("run a scan", async () => {
    await clickByText(page, "button", "Scan sensors");
    await sleep(500);
    await rec.capture("Scan tab — discovered sensors", 1800);
  });

  await tryStep("add the Step16 sensor", async () => {
    await clickAddForGuess(page, "m5_step16");
    await sleep(300);
    await rec.capture("Scan tab — Step16 Unit added", 1200);
  });

  await tryStep("add the AS7341 spectral sensor", async () => {
    await clickAddForGuess(page, "as7341");
    await sleep(300);
    await rec.capture("Scan tab — AS7341 spectral sensor added", 1200);
  });

  // 5. Sensors tab — cycle through every configured sensor to show each one's own capability
  // (colour calibration/teach palette, distance, 8-angle knobs, gamepad, step encoder, spectral).
  await tryStep("open Sensors tab", async () => {
    await clickByText(page, "button", "Sensors");
    await sleep(300);
    await rec.capture("Sensors tab — colour-sensor selected", 1800);
  });

  await tryStep("calibrate the colour sensor", async () => {
    await clickByText(page, "button", "Calibrate");
    await sleep(400);
    await rec.capture("Sensors tab — after Calibrate", 1000);
  });

  await tryStep("scroll to the colour calibration palette", async () => {
    await scrollPanel(page, 380);
    await sleep(250);
    await rec.capture("Sensors tab — colour calibration palette", 1600);
    await scrollPanel(page, 380);
    await sleep(250);
    await rec.capture("Sensors tab — teachable colour swatches", 1600);
    await scrollPanelToTop(page);
    await sleep(200);
  });

  await tryStep("switch to distance-sensor", async () => {
    await clickSubtab(page, "Choose a sensor to configure", "distance-sensor");
    await sleep(300);
    await rec.capture("Sensors tab — distance-sensor", 1500);
  });

  await tryStep("switch to knob-unit", async () => {
    await clickSubtab(page, "Choose a sensor to configure", "knob-unit");
    await sleep(300);
    await rec.capture("Sensors tab — knob-unit (8-Angle)", 1500);
  });

  await tryStep("switch to gamepad", async () => {
    await clickSubtab(page, "Choose a sensor to configure", "gamepad");
    await sleep(300);
    await rec.capture("Sensors tab — gamepad", 1500);
  });

  await tryStep("switch to the newly-added Step16 sensor", async () => {
    await clickSubtab(page, "Choose a sensor to configure", "m5_step16-", { startsWith: true });
    await sleep(300);
    await rec.capture("Sensors tab — Step16 Unit", 1500);
  });

  await tryStep("switch to the newly-added AS7341 sensor", async () => {
    await clickSubtab(page, "Choose a sensor to configure", "as7341-", { startsWith: true });
    await sleep(300);
    await rec.capture("Sensors tab — AS7341 spectral sensor", 1500);
  });

  // 6. Settings tab — flip on Grown-up mode *before* Display/LEGO so the rest of the walkthrough
  // shows the full wiring-level detail (and LEGO tab's Word Blocks/Binary maths/hub-program
  // sections, which only render at all when advanced mode is on).
  await tryStep("open Settings tab", async () => {
    await clickByText(page, "button", "Settings");
    await sleep(300);
    await rec.capture("Settings tab — kid mode", 1400);
  });

  await tryStep("turn on grown-up mode", async () => {
    // The Grown-up/Kid mode switch (SettingsForm's first DebugRow) has no visible text on the
    // toggle itself — find the row by its "mode" label, then click the switch inside it.
    const handle = await page.evaluateHandle(() => {
      const rows = Array.from(document.querySelectorAll(".debug-row"));
      const row = rows.find((r) => /mode/i.test(r.textContent || ""));
      return row ? row.querySelector(".toggle") : null;
    });
    const toggle = handle.asElement();
    if (!toggle) throw new Error("advanced-mode toggle not found");
    await toggle.click();
    await toggle.dispose();
    await sleep(300);
    await rec.capture("Settings — grown-up mode on", 1600);
  });

  // 7. Display tab.
  await tryStep("open Display tab", async () => {
    await clickByText(page, "button", "Display");
    await sleep(300);
    await rec.capture("Display tab — onboard display config", 1800);
  });

  // 8. LEGO emitter tab — Quick assign, then (grown-up mode only) Word Blocks, Binary maths
  // (bit-packing explained), and the two generated hub-program modes (SPIKE Python, Pybricks).
  await tryStep("open LEGO tab", async () => {
    await clickByText(page, "button", "LEGO");
    await sleep(300);
    await rec.capture("LEGO tab — Quick assign", 1800);
  });

  await tryStep("scroll to Build it with blocks", async () => {
    // Word Blocks renders open by default — just needs scrolling into view.
    await scrollPanel(page, 420);
    await sleep(250);
    await rec.capture("LEGO tab — Build it with blocks (SPIKE App)", 1800);
  });

  await tryStep("expand Binary maths (bit-packing explained)", async () => {
    await expandDetails(page, "Binary maths");
    await sleep(300);
    await scrollPanel(page, 260);
    await sleep(250);
    await rec.capture("LEGO tab — Binary maths, grown-up mode bit-packing", 1800);
  });

  await tryStep("expand the SPIKE Python hub program", async () => {
    await expandDetails(page, "SPIKE Prime");
    await sleep(300);
    await scrollPanel(page, 260);
    await sleep(250);
    await rec.capture("LEGO tab — SPIKE Prime / Robot Inventor hub program", 1800);
  });

  await tryStep("expand the Pybricks hub program", async () => {
    await expandDetails(page, "Pybricks");
    await sleep(300);
    await scrollPanel(page, 260);
    await sleep(250);
    await rec.capture("LEGO tab — Pybricks hub program", 1800);
    await scrollPanelToTop(page);
    await sleep(200);
  });

  // 8. Dashboard tab — start polling and let live data animate.
  await tryStep("open Dashboard tab", async () => {
    await clickByText(page, "button", "Dashboard");
    await sleep(300);
    await rec.capture("Dashboard tab — before polling", 1000);
  });

  await tryStep("start polling", async () => {
    await clickByText(page, "button", "Start polling");
    await sleep(400);
    await rec.captureBurst("Dashboard — live readings (colour/distance)", 4, 500, 500);
  });

  await tryStep("switch timeline to 2 columns", async () => {
    // The columns <select> has no id/class of its own — find it by its neighbouring "columns"
    // label text instead, same pattern as clickSubtab/clickAddForGuess use for unlabelled controls.
    const handle = await page.evaluateHandle(() => {
      // textContent here runs straight into the <select>'s option text with no separator
      // ("columns123"), so a trailing \b never matches — check the label's own leading text node.
      const labels = Array.from(document.querySelectorAll("label"));
      const label = labels.find((l) => (l.textContent || "").trim().toLowerCase().startsWith("columns"));
      return label ? label.querySelector("select") : null;
    });
    const select = handle.asElement();
    if (!select) throw new Error("columns <select> not found");
    await select.select("2");
    await select.dispose();
    await sleep(400);
    await rec.capture("Dashboard — 2-column timeline layout", 1600);
  });

  await tryStep("scroll down to the knob and gamepad readings", async () => {
    await scrollPanel(page, 500);
    await sleep(300);
    await rec.captureBurst("Dashboard — knob-unit & gamepad live", 5, 500, 550);
    await scrollPanelToTop(page);
    await sleep(200);
  });

  // 9. Gamepad virtual controller modal.
  await tryStep("open virtual controller", async () => {
    await clickSelector(page, 'button[title="Open virtual controller"]');
    await sleep(400);
    await rec.capture("Virtual game controller open", 1600);
  });

  await tryStep("wiggle a virtual stick", async () => {
    const stick = await page.$(".pad-stick, .stick, [class*='stick']");
    if (stick) {
      const box = await stick.boundingBox();
      if (box) {
        const cx = box.x + box.width / 2;
        const cy = box.y + box.height / 2;
        await page.mouse.move(cx, cy);
        await page.mouse.down();
        await page.mouse.move(cx + 18, cy - 14, { steps: 6 });
        await rec.capture("Virtual controller — stick moved", 800);
        await page.mouse.up();
      }
    }
  });

  await tryStep("close virtual controller", async () => {
    await clickSelector(page, 'button[aria-label="Close"]');
    await sleep(300);
  });

  // 10. Settings tab again — debug tools + export/import (Grown-up mode is already on from
  // step 6, so this is just the rest of the tab we hadn't shown yet).
  await tryStep("revisit Settings tab — debug tools", async () => {
    await clickByText(page, "button", "Settings");
    await sleep(300);
    await scrollPanel(page, 260);
    await sleep(250);
    await rec.capture("Settings tab — debug tools + export/import", 1600);
    await scrollPanelToTop(page);
    await sleep(200);
  });

  // 11. Toggle theme (light/dark) for a bonus beat showing both looks.
  await tryStep("toggle theme", async () => {
    await clickSelector(page, ".theme-toggle");
    await sleep(400);
    await rec.capture("Theme toggled", 1600);
    await clickSelector(page, ".theme-toggle");
    await sleep(300);
  });

  // 12. Back to Dashboard for a final live-data hero frame.
  await tryStep("return to Dashboard", async () => {
    await clickByText(page, "button", "Dashboard");
    await sleep(300);
    await rec.captureBurst("Dashboard — final live readings", 4, 500, 700);
  });
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

async function main() {
  await rm(FRAMES_DIR, { recursive: true, force: true });
  await mkdir(FRAMES_DIR, { recursive: true });
  await mkdir(ASSETS_DIR, { recursive: true });

  console.log("🚀 starting Vite dev server…");
  const server = startDevServer();
  try {
    await waitForServer(BASE_URL);
    console.log(`✅ dev server ready at ${BASE_URL}`);

    console.log("🧭 launching headless browser…");
    // Puppeteer 21's pinned Chromium build (121) crashes on launch on newer macOS —
    // fall back to a system-installed Chrome/Chromium if one exists, since it's
    // just being driven over the DevTools protocol either way.
    const systemChromeCandidates = [
      "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
      "/Applications/Chromium.app/Contents/MacOS/Chromium",
    ];
    const { existsSync } = await import("node:fs");
    const executablePath = systemChromeCandidates.find((p) => existsSync(p));
    const browser = await puppeteer.launch({
      headless: "new",
      defaultViewport: VIEWPORT,
      ...(executablePath ? { executablePath } : {}),
    });
    const page = await browser.newPage();
    await page.emulateMediaFeatures([{ name: "prefers-color-scheme", value: THEME }]);
    // Belt-and-braces: the app reads a saved `mc-theme` from localStorage first and only falls
    // back to prefers-color-scheme if nothing's stored yet (see App.tsx) — set it directly so a
    // stale localStorage value from a previous run can't override the theme this run asked for.
    await page.evaluateOnNewDocument((theme) => {
      localStorage.setItem("mc-theme", theme);
    }, THEME);

    console.log(`🎬 recording walkthrough (${THEME} theme)…`);
    const rec = new FrameRecorder(page, FRAMES_DIR);
    try {
      await runWalkthrough(page, rec);
    } finally {
      await rec.writeManifest();
      await browser.close();
    }
    console.log(`\n✅ captured ${rec.frames.length} frames → ${FRAMES_DIR}`);
    if (THEME === "dark") {
      console.log("\nNext steps:");
      console.log("  python3 scripts/assemble-gif.py     # writes docs/assets/demo-walkthrough.gif");
      console.log("  bash scripts/assemble-webm.sh       # writes docs/assets/demo-walkthrough.webm");
    } else {
      console.log("\nLight-theme frames are for print-friendly manual screenshots — copy the ones");
      console.log("you need from scripts/_demo_frames_light/ into docs/assets/screenshots/.");
    }
  } finally {
    server.kill("SIGTERM");
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});

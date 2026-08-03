// GuideTab.tsx — a built-in walkthrough written for kids: short sentences, LEGO analogies, no
// jargon. One expandable card per tab, narrated by Brix (Mascot.tsx). Purely static content —
// no device interaction — so it works even before connecting.
import { useState } from "react";
import type { TabId } from "../App";
import { Mascot } from "./Mascot";
import {
  Search, Puzzle, Monitor, ToyBrick, BarChart3, Settings,
  ChevronUp, ChevronDown, Lightbulb, ArrowRight, FlaskConical,
  type LucideIcon,
} from "lucide-react";

interface GuideSection {
  tab: TabId;
  Icon: LucideIcon;
  colour: string;
  title: string;
  body: string;
  tip: string;
}

// Same icon-per-tab mapping AND colour as App.tsx's TABS array — this guide mirrors the tab bar
// 1:1, so it should look like the tab bar's own explanation, not a separately-illustrated
// document. Each row's icon carries its own destination tab's colour (rather than one flat
// colour for the whole page) precisely so that mirroring holds — a reader should recognise
// "this is the Sensors row" by its blue icon the same way they'd recognise the Sensors tab
// itself.
const SECTIONS: GuideSection[] = [
  {
    tab: "scan",
    Icon: Search,
    colour: "yellow",
    title: "Scan — find your sensors",
    body: "Your board can talk to lots of different sensors, but first it needs to find them — like turning on the lights in a room to see what's inside. Press \"Scan sensors\" and it'll check every wire for something plugged in. There's also a Bluetooth game controller down there — no wire needed, just add it and pair it.",
    tip: "Plug a sensor in, then hit Scan — it should show up in the list! Want a game controller instead? Scroll down to the Bluetooth section and hit \"+ Add\".",
  },
  {
    tab: "sensors",
    Icon: Puzzle,
    colour: "blue",
    title: "Sensors — your robot's senses",
    body: "Sensors are like eyes and fingers for your robot: a distance sensor \"feels\" how far away something is, a colour sensor \"sees\" colours, a knob sensor knows which way you turned it. Here you pick what each one measures and how fast it checks.",
    tip: "Tick \"enabled\" on a sensor, then watch its live number on the Dashboard!",
  },
  {
    tab: "display",
    Icon: Monitor,
    colour: "azure",
    title: "Display — the board's own little screen",
    body: "If your board has a tiny screen built in, this is where you turn it on and choose what it shows — like a name tag, or a list of what every sensor is reading right now.",
    tip: "Turn the screen on, then pick a sensor to \"show\" on it.",
  },
  {
    tab: "lego",
    Icon: ToyBrick,
    colour: "red",
    title: "LEGO — pretend to be a LEGO sensor",
    body: "This is the clever bit: your board can pretend to BE a real LEGO sensor, so a SPIKE Prime or Robot Inventor hub thinks it's talking to one! You choose which sensor readings get sent, and the hub reads them like magic.",
    tip: "Plug the board into your LEGO hub with the cable, then try the \"Colour → RGBI\" button for an instant setup.",
  },
  {
    tab: "dashboard",
    Icon: BarChart3,
    colour: "green",
    title: "Dashboard — watch everything live",
    body: "This is the fun part! Every sensor you've turned on shows its live reading here, with colours, dials, and little charts, updating many times a second — like a dashboard in a car.",
    tip: "Turn a knob or wave your hand over a distance sensor and watch the number change instantly.",
  },
  {
    tab: "settings",
    Icon: Settings,
    colour: "azure",
    title: "Settings — the toolbox",
    body: "Backup your whole setup to a file (so you never lose it), or turn on debug tools if a grown-up is helping fix something tricky.",
    tip: "Export a config before trying something new — you can always bring it back!",
  },
];

export function GuideTab({ onGoTo }: { onGoTo: (tab: TabId) => void }) {
  const [openTab, setOpenTab] = useState<TabId | null>("scan");

  return (
    <section className="card guide-card">
      <div className="card-head">
        <h2>Guide</h2>
      </div>

      <div className="guide-intro">
        <Mascot mood="celebrating" size={84} />
        <div>
          <h3 style={{ margin: "0 0 4px" }}>Hi, I'm Brix! 👋</h3>
          <p className="muted sm" style={{ margin: 0, padding: 0, maxWidth: 480 }}>
            I'll show you around MultiController — click on a tile below to find out what each
            part does. No boring instructions, promise! No board handy? Disconnect and hit{" "}
            <b><FlaskConical size={13} strokeWidth={2.25} className="inline-icon" /> Try demo mode</b> to explore with fake sensors instead.
          </p>
        </div>
      </div>

      <div className="guide-sections">
        {SECTIONS.map((s) => {
          const open = openTab === s.tab;
          return (
            <div className={`guide-section${open ? " open" : ""}`} key={s.tab}>
              <button
                type="button"
                className="guide-section-head"
                onClick={() => setOpenTab(open ? null : s.tab)}
                aria-expanded={open}
              >
                <s.Icon
                  className="guide-section-emoji"
                  style={{ color: `var(--lego-${s.colour})` }}
                  size={19}
                  strokeWidth={2.25}
                  aria-hidden="true"
                />
                <span className="guide-section-title">{s.title}</span>
                <span className="guide-section-chevron" aria-hidden="true">
                  {open ? <ChevronUp size={15} strokeWidth={2.5} /> : <ChevronDown size={15} strokeWidth={2.5} />}
                </span>
              </button>
              {open && (
                <div className="guide-section-body">
                  <p style={{ margin: "0 0 10px" }}>{s.body}</p>
                  <div className="guide-tip">
                    <Lightbulb size={14} strokeWidth={2.25} className="inline-icon" aria-hidden="true" /> <b>Try it:</b> {s.tip}
                  </div>
                  <button className="ghost sm icon-btn-label" style={{ marginTop: 10 }} onClick={() => onGoTo(s.tab)}>
                    Take me there <ArrowRight size={13} strokeWidth={2.25} />
                  </button>
                </div>
              )}
            </div>
          );
        })}
      </div>
    </section>
  );
}

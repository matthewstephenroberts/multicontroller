// Mascot.tsx — "Brix", a small flat LEGO-minifig-style helper bot built from rectangles and
// circles (same technique as App.tsx's LogoBrick), used on the welcome screen, in the Guide
// tab, and in empty states so those moments feel designed rather than accidental. Four moods
// cover every place it's used: happy (default/welcome), thinking (empty states — "nothing here
// yet"), celebrating (Guide tab header / success moments), working (BusyOverlay during a save/
// import — paired with the CSS bob animation there, so the face just needs to read as
// "concentrating" rather than animate on its own).
interface Props {
  mood?: "happy" | "thinking" | "celebrating" | "working";
  size?: number;
  className?: string;
}

export function Mascot({ mood = "happy", size = 96, className }: Props) {
  // Arm angles and eye/mouth shape vary by mood; body/head/legs stay constant so it always
  // reads as the same character.
  const leftArm = mood === "celebrating" ? "-35 30 46" : mood === "thinking" || mood === "working" ? "20 30 46" : "0 30 46";
  const rightArm = mood === "celebrating" ? "35 66 46" : "-8 66 46";
  return (
    <svg width={size} height={size} viewBox="0 0 96 96" aria-hidden="true" className={className}>
      {/* legs */}
      <rect x="34" y="76" width="10" height="16" rx="2" fill="#3a4250" />
      <rect x="52" y="76" width="10" height="16" rx="2" fill="#3a4250" />
      {/* body (LEGO torso brick shape) */}
      <rect x="26" y="46" width="44" height="34" rx="6" fill="#0059ab" />
      <circle cx="48" cy="63" r="6" fill="#ffd500" opacity="0.9" />
      {/* arms */}
      <rect x="26" y="46" width="9" height="26" rx="4" fill="#ffd500" transform={`rotate(${leftArm})`} />
      <rect x="61" y="46" width="9" height="26" rx="4" fill="#ffd500" transform={`rotate(${rightArm})`} />
      {/* head */}
      <rect x="30" y="14" width="36" height="32" rx="10" fill="#ffd500" />
      <circle cx="48" cy="6" r="6" fill="#ffd500" />
      {mood === "working" ? (
        <>
          <path d="M36 30 h8" stroke="#1c2431" strokeWidth="2.4" fill="none" strokeLinecap="round" />
          <path d="M52 30 h8" stroke="#1c2431" strokeWidth="2.4" fill="none" strokeLinecap="round" />
          <path d="M41 39 h14" stroke="#1c2431" strokeWidth="2.2" fill="none" strokeLinecap="round" />
        </>
      ) : mood === "thinking" ? (
        <>
          <circle cx="40" cy="30" r="2.4" fill="#1c2431" />
          <circle cx="56" cy="30" r="2.4" fill="#1c2431" />
          <path d="M40 40 q8 -4 16 0" stroke="#1c2431" strokeWidth="2.2" fill="none" strokeLinecap="round" />
        </>
      ) : mood === "celebrating" ? (
        <>
          <path d="M36 28 q4 -4 8 0" stroke="#1c2431" strokeWidth="2.2" fill="none" strokeLinecap="round" />
          <path d="M52 28 q4 -4 8 0" stroke="#1c2431" strokeWidth="2.2" fill="none" strokeLinecap="round" />
          <path d="M38 36 q10 8 20 0" stroke="#1c2431" strokeWidth="2.4" fill="none" strokeLinecap="round" />
        </>
      ) : (
        <>
          <circle cx="40" cy="30" r="2.4" fill="#1c2431" />
          <circle cx="56" cy="30" r="2.4" fill="#1c2431" />
          <path d="M39 37 q9 6 18 0" stroke="#1c2431" strokeWidth="2.2" fill="none" strokeLinecap="round" />
        </>
      )}
    </svg>
  );
}

/* RRV-10 canvas config for schwung-canvaskit (../schwung-canvaskit).
 * SOURCE for src/canvas.js — regenerate after editing:
 *   node ../schwung-canvaskit/build.mjs src/canvas.config.js src/canvas.js src/module.json
 * Concatenated between the kit prelude (cell constructors in scope) and the
 * kit engine (which reads CONFIG) inside one IIFE. */

/* Continuous params are native 0..100 ints — the module's chain contract. */
KIT_PARAM_MAX = 100;

/* Mode names in front-panel order. Only ROM banks 0-8 hold programs, so this
 * list is exactly the hardware's nine switch positions — no padding.
 * Overlay labels are the readable form; `sq` is the 3-glyph form that fits the
 * framed enum square in a 32px cell. */
const kModeLabels = ["Room 1", "Room 2", "Hall 1", "Hall 2",
                     "Plate 1", "Plate 2", "M-Tap 1", "M-Tap 2", "Gate"];
const kModeSquares = ["RM1", "RM2", "HL1", "HL2",
                      "PL1", "PL2", "MT1", "MT2", "GAT"];

/* A knob-style (arc) cell over an arbitrary integer range. `uni` is hardwired to
 * 0..KIT_PARAM_MAX, and `count` would render the big numeric square instead — we
 * want the arc, over 0..15. PICK sensitivity because the range is short: at the
 * continuous KIT_SENS a 16-step param sweeps in 32 detents, which is twitchy. */
function knobRange(key, label, lo, hi) {
  return { key, label, kind: "unipolar", min: lo, max: hi, step: 1, sens: KIT_PICK_SENS };
}

const CONFIG = {
  name: "RRV-10",

  /* The whole unit is five controls, so it is one page — no bank nesting and
   * nothing to drill into. The SHIFT picker still needs a section entry to
   * point at (the engine indexes CONFIG.sections unguarded), but with a single
   * bank there is nothing for it to switch between. */
  banks: [
    {
      label: "RRV-10",
      knobs: [
        enumc("mode", "Mode", kModeLabels, kModeSquares),
        /* 16 detented switch positions on the hardware, not a continuum — but
         * shown as a knob, since that is how it reads on the panel. */
        knobRange("time", "Time", 0, 15),
        /* Pre-equalizer: 50 is flat, below cuts highs (warmer), above cuts
         * lows (brighter). Bipolar so the arc reads out from centre. */
        bip("pre_eq", "Tone"),
        /* Five cells wrap 4 + 1, so Dry sits alone on the second row. A blank()
         * spacer would force a tidier 3 + 2 — but a spacer consumes its knob
         * slot (engine.js: cellsFor(...)[cc-71], blanks return early), which
         * would leave physical knob 4 dead and push Efct/Dry to knobs 5-6.
         * A live contiguous 1-5 beats a prettier line break. */
        uni("effect_level", "Efct"),
        uni("direct_level", "Dry")
      ]
    }
  ],

  sections: [
    { name: "RRV-10", bank: 0 }
  ],

  icons: ["env"],

  /* Mirrors the plugin's create_instance defaults. Only consumed off-device
   * (previewer / tests) — on device every read is live. */
  defaults: {
    mode: 2,
    time: 8,
    pre_eq: 50,
    effect_level: 40,
    direct_level: 100
  },

  testExports: { kModeLabels, kModeSquares }
};

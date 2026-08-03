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

const CONFIG = {
  name: "RRV-10",

  /* Five editable params — the whole unit. Two banks rather than one so the
   * SHIFT picker has something to pick, and so send levels sit apart from the
   * reverb's own character controls. */
  banks: [
    {
      label: "Reverb",
      knobs: [
        enumc("mode", "Mode", kModeLabels, kModeSquares),
        /* 0..15 detented switch positions on the hardware, not a continuum —
         * `count` renders the big numeric read-out and steps one per detent. */
        count("time", "Time", 0, 15),
        /* Pre-equalizer: 50 is flat, below cuts highs (warmer), above cuts
         * lows (brighter). Bipolar so the arc reads out from centre. */
        bip("pre_eq", "Tone")
      ]
    },
    {
      label: "Mix",
      knobs: [
        uni("effect_level", "Efct"),
        uni("direct_level", "Dry")
      ]
    }
  ],

  sections: [
    { name: "REVERB", bank: 0 },
    { name: "MIX", bank: 1 }
  ],

  icons: ["env", "pan"],

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

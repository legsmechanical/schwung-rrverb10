# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

RRVerb-10 — an emulation of an 80s Micro Rack digital reverb, running the unit's
own BOS-007 gate-array program out of a 16 KB ROM the user supplies. Four
parameters: `mode`, `time`, `pre_eq`, `mix`. `component_type: audio_fx`.

```
src/
  dsp/
    rrverb10.cpp        # plugin wrapper — set_param/get_param, chain_params,
                        #   ui_hierarchy, the resampler
    BossEmu.cpp/.h      # the gate array itself (exact 16-bit integer arithmetic)
  module.json           # metadata + the OTHER copy of ui_hierarchy (see below)
  help.json             # on-device help
  canvas.config.js      # Bank Editor SOURCE — SUPPRESSED, kept for the revert
  canvas.js             # GENERATED — SUPPRESSED, kept for the revert
tests/
  test_rrverb10.cpp     # the DSP: rate, taps, impulse responses
  hierarchy.test.mjs    # the two declarations must AGREE (see below)
```

## ⚠⚠ THE UI IS DECLARED TWICE, and neither copy is authoritative everywhere

This is the thing to know before editing anything user-visible. Which copy the
host reads depends on **where the effect is inserted**:

| surface | `ui_hierarchy` | `chain_params` |
|---|---|---|
| chain slot fx1–4 | **`src/module.json`** first, plugin as fallback | **the plugin** first |
| Master FX bus | **the plugin** (module.json never consulted) | the plugin |
| send / shadow slot | the plugin | the plugin |

So a field edited on one side only is an effect that behaves differently
depending on which slot you drop it in — with nothing logged, and nothing to see
in the diff. `tests/hierarchy.test.mjs` asserts the two agree on every field the
widget choice or the readout depends on; run it after touching either.

⭑ The plugin's `ui_hierarchy` is **string refs only**, so on the FX bus its
`chain_params` is the only source of param metadata. Do not thin it out.

## ⛔ The on-device Bank Editor is SUPPRESSED (2026-09-03)

The host draws its own generated knob grid from `ui_hierarchy` + `chain_params`,
which is what 95 of the 100 modules in the fleet do, so the bespoke canvas
stopped earning the screen.

**Five declarations were removed, and all five matter, differently:**

1. the `type:"canvas"` entry in the plugin's `chain_params` (`rrverb10.cpp`);
2. the `{"key":"editor"}` link in **both** `ui_hierarchy` copies — ⚠ strip only
   the plugin's and module.json's leftover link becomes a **fifth knob on Main
   labelled `BEDITO`**, guessed as a float 0..1;
3. `capabilities.host_canvas_ui` in `module.json` — the flag **dAVEBOx** reads to
   host the canvas *in preference to* the grid. It loads `canvas.js` itself, so
   dropping the chain_params entry alone would NOT have freed the screen;
4. `scripts/build.sh`'s packaging line — **and an explicit `rm -f` beside it**,
   because `dist/` is not cleaned between builds and the previous build's
   `canvas.js` otherwise stays in the tarball;
5. `scripts/install.sh` — **it only ever `scp`s, never deletes**, so a
   `canvas.js` from a pre-suppression install lives on the device forever, and
   dAVEBOx loads it straight off disk regardless of the flag. It now removes it.

`p->editor`, its `set_param` case and its slot in the state blob are deliberately
**left alone**: inert, and they keep the revert small.

⚠ **REVERSIBLE, deliberately.** `canvas.config.js` and `canvas.js` stay in the
tree, unbuilt. Restoring the packaging line and the four declarations puts the
editor back exactly as it was.

### What the generated grid does differently, and why we accepted it

- **`mode`** — ⚠ the grid draws an enum as a two-line **three-character** square,
  and `enumSquareLines` renders both `"M-Tap 1"` and `"M-Tap 2"` as `M / TAP`:
  two modes, one picture. Fixed by declaring `short_options` (the canvas's own
  `kModeSquares` labels). Verified against the host's font tables — that pair
  collides and no other does. Keep `short_options` in step with `options`.
- **`time`** — draws a **big number**, where the canvas drew an arc: an `int`
  spanning ≤ 24 takes that route. ⚠ Do **not** "fix" this by making it a float
  with a fractional step. `rrverb10.cpp`'s `set_param` is `atoi`, so `7.5` stores
  as 7 and the readback snaps — every other detent becomes a no-op, which is far
  worse than a number instead of a dial.
- **`pre_eq`** — loses its centre tick. There is no bipolar arc drawing at all;
  `min < 0` is consulted only to add a `+` sign to a big number. A real loss with
  no fix short of a host feature.
- **`mix`** — reads "30%", from `unit`.

## Building and installing

```bash
./scripts/build.sh                 # cross-compiles via Docker (aarch64)
MOVE_HOST=move.local ./scripts/install.sh [--with-rom]
node tests/hierarchy.test.mjs      # the two declarations agree
```

`install.sh` restarts the host itself — a module deploy is a no-op until it does,
and swapping the effect out and back in is not enough.

⚠ The **ROM is not ours to redistribute** and is deliberately not packaged; the
user supplies it through the host's `assets` mechanism. `--with-rom` uploads your
own local copy.

## The rate is the whole ballgame

The gate array consumes 256 clock cycles per sample from an 8.000 MHz crystal, so
its native rate is **31250 Hz**, and the module resamples 44100 → 31237.5 → 44100
rather than clocking one chip cycle per host sample. That shortcut — which the
reference plugin takes — runs 41% fast: every decay comes out 29% short and the
response shifts up 5.97 semitones. It still sounds like a reverb, which is why it
goes unnoticed. `tests/test_rrverb10.cpp` pins the tap positions and the decay
rather than trusting this. See README.md for the derivation.

⚠ Build with `-O3`, never `-Ofast`: BossEmu is exact integer arithmetic with
deliberate overflow/saturation semantics, and the emulator must not be given
licence to reassociate its accumulator.

## License

LGPL-2.1-or-later. Chip emulation by Sergey V. Mikayev (MUNT); ROM research by
giulioz.

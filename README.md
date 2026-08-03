# schwung-rrv10

Chip-level emulation of a mid-80s Micro Rack digital reverb, as a Schwung audio
FX module. Nine effects — rooms, halls, plates, a multi-tap delay, a reverse and
a gated reverb — run from the unit's own program ROM.

The DSP is [MUNT](https://github.com/munt/munt)'s `BossEmu`, a cycle-accurate
model of the BOSS HG61H20R36F (BOS-007) reverb gate array, driven by a dump of
the unit's 16 KB program ROM. Nothing about the reverb is modelled by ear or
approximated: the ROM *is* the reverb algorithm, and the emulator executes it.

## You must supply the ROM

The program ROM is not ours to redistribute and is **not** included. Supply your
own dump:

| | |
|---|---|
| path | `roms/rrv10.bin` inside the installed module folder |
| size | 16384 bytes (MSM27C128 EPROM) |
| crc32 | `4B1D6D75` |

Easiest route is the web manager at `http://move.local:7700` — the module
declares the file in its `assets` block, so the manager shows an upload slot and
verifies size and CRC for you. Or `./scripts/install.sh --with-rom` if you keep
a copy in this repo's `roms/` (git-ignored).

**Without the ROM the effect still loads and passes audio through unprocessed**,
and reports the missing file rather than failing to start.

## Controls

| Param | Range | Notes |
|---|---|---|
| Mode | 9 modes | Room 1/2, Hall 1/2, Plate 1/2, M-Tap 1 (delay), M-Tap 2 (reverse), Gate |
| Decay/Gate | 0–15 | 16 switch positions, as on the hardware — not a continuum |
| Pre-EQ | 0–100 | Tone into the reverb. 50 flat, below warmer, above brighter. Dry path untouched |
| Mix | 0–100 | 0 dry, 100 wet. Equal-power, so the middle of the sweep holds its level |

The hardware has independent Effect and Direct level knobs; this exposes a single
equal-power blend instead, which suits a chain insert better. Mode indices are the
front panel's order. Only ROM banks 0–8 hold programs
(9–14 are zero-filled), which is one of three independent confirmations that
these nine are the whole unit — the others being the documentation and each
program's measured impulse response.

The on-device **Bank Editor** is built with
[schwung-canvaskit](../schwung-canvaskit); the module also declares
`host_canvas_ui`, so davebox can host the same editor.

## The rate, which is the whole ballgame

The gate array consumes 256 clock cycles per audio sample and the unit clocks it
from an 8.000 MHz crystal, so its native rate is **31250 Hz** — which the
documentation states independently ("Sampling Frequency: 31.25 kHz").

Driving one chip cycle per *host* sample instead — the obvious shortcut, and what
the reference JUCE plugin does — runs the emulation 41% fast: **every decay comes
out 29% short and the whole response shifts up 5.97 semitones.** It still sounds
like a reverb, which is why it goes unnoticed, but it is not this reverb.

So the module resamples 44100 → 31237.5 → 44100. `tests/test_rrv10.cpp` pins
this down rather than trusting it: the multi-tap delay's first echo sits at a
ROM-derived position (`1100·t − 601` chip cycles), and the test asserts it lands
where the hardware puts it and *not* where the naive version would.

Why 31237.5 and not 31250: the exact ratio is 882:625, an 882-phase filter bank.
A reverb carries no pitch reference, so detuning the clock by **0.69 cents**
buys a 17/24 ratio instead. Details and the measured alias/droop table are in
`src/dsp/resampler_poly.h`.

Measured cost of the real `process_block`: 224 ns/frame, 28.7 µs per 128-frame
block — under 1% of a modern desktop core, roughly 3–4% of the device's.
Running the chip at its true (slower) rate removes ~29% of the emulator's work,
which more than pays for the resampler: the correct version benchmarks *cheaper*
than the naive one.

## Build

```bash
./scripts/test.sh       # native unit tests, no device or cross-compiler needed
./scripts/build.sh      # cross-compile for ARM64 via Docker -> dist/
./scripts/install.sh    # deploy to move.local and restart the host
```

Regenerate the canvas after editing `src/canvas.config.js`:

```bash
node ../schwung-canvaskit/build.mjs src/canvas.config.js src/canvas.js src/module.json
node ../schwung-canvaskit/tests/contract.test.mjs src/canvas.js
node ../schwung-canvaskit/preview.mjs src/canvas.js /tmp/preview.png
```

## Credits

- Gate array emulation: Sergey V. Mikayev (MUNT), LGPL-2.1+
- ROM extraction and the original plugin: [giulioz/smol-rack](https://github.com/giulioz/smol-rack)

See `LICENSE` for the full provenance breakdown.

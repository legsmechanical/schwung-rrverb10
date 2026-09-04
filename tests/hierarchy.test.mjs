// TWO DECLARATIONS, ONE MODULE — this test exists to keep them the same.
//
// RRVerb-10 declares its UI twice, and which copy the host reads depends on
// WHERE the effect is inserted:
//
//   chain slot fx1-4   ui_hierarchy: src/module.json FIRST, plugin as fallback
//                      chain_params: the PLUGIN first
//   Master FX bus      both from the PLUGIN (module.json is never consulted)
//   send / shadow slot both from the PLUGIN
//
// So neither copy is authoritative everywhere, and the failure mode when they
// drift is the nastiest kind: the module works, nothing is logged, and it simply
// looks like a DIFFERENT effect depending on which slot you put it in. Coverage
// is not worth asserting here — there are four params on one level — so this
// asserts AGREEMENT instead, plus that the suppressed canvas is suppressed in
// every place it was declared.
//
// It reads the plugin's copy out of the C SOURCE: the .so is aarch64 and cannot
// be run here, and the source is what ships.
//
// Run: node tests/hierarchy.test.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const cpp = readFileSync(join(root, "src", "dsp", "rrverb10.cpp"), "utf8");
const buildSh = readFileSync(join(root, "scripts", "build.sh"), "utf8");
const installSh = readFileSync(join(root, "scripts", "install.sh"), "utf8");
const moduleJson = JSON.parse(readFileSync(join(root, "src", "module.json"), "utf8"));

let failures = 0;
const ok = (c, m) => { if (!c) { failures++; console.error("FAIL " + m); } else console.log("ok   " + m); };

/* ---------------------------------------------------------------- helpers */

/** The C string pieces between two markers, joined and unescaped.
 *
 * Searched FROM the opening marker, not from 0 — the closing markers also occur
 * earlier in the file, and a bare indexOf finds the wrong one and slices empty.
 * (Same extraction pattern as schwung-obxd/tests/hierarchy.test.mjs.) */
function joinedLiteral(from, to) {
  const i = cpp.indexOf(from);
  const j = i < 0 ? -1 : cpp.indexOf(to, i + from.length);
  if (i < 0 || j < 0) throw new Error("markers not found: " + from);
  /* Comments are stripped FIRST. These literals are heavily commented, and a
   * comment explaining a value quotes it — the mode comment names "M-Tap 1"
   * verbatim — so a naive literal scan splices prose into the JSON. */
  const src = cpp.slice(i, j).replace(/\/\*[\s\S]*?\*\//g, " ")
                             .replace(/^\s*\/\/.*$/gm, "");
  const lits = src.match(/"(?:[^"\\]|\\.)*"/g) || [];
  return lits.map((l) => l.slice(1, -1)).join("")
             .replace(/\\"/g, '"').replace(/\\n/g, "");
}

/** First balanced {...} or [...] — the literal is followed by format tails. */
function firstBalanced(s, open) {
  const close = open === "{" ? "}" : "]";
  const start = s.indexOf(open);
  let depth = 0;
  for (let n = start; n < s.length; n++) {
    if (s[n] === open) depth++;
    else if (s[n] === close && --depth === 0) return s.slice(start, n + 1);
  }
  throw new Error("unbalanced " + open);
}

/* `%%` is printf's escape for a literal percent — it reaches the host as `%`. */
const chainParamsSrc = joinedLiteral('if (strcmp(key, "chain_params") == 0)',
                                     'if (strcmp(key, "state") == 0)');
const pluginChain = JSON.parse(firstBalanced(chainParamsSrc, "[").replace(/%%/g, "%"));

const pluginHierarchy = JSON.parse(firstBalanced(
  joinedLiteral('if (strcmp(key, "ui_hierarchy") == 0)', "int len = (int)strlen(h);"), "{"));

const jsonHierarchy = moduleJson.capabilities.ui_hierarchy;
const jsonRoot = jsonHierarchy.levels.root;
const pluginRoot = pluginHierarchy.levels.root;

/* module.json declares its params INLINE in the hierarchy; the plugin's
 * hierarchy is string refs only, so its meta lives in chain_params. Those two
 * are the things that have to agree. */
const jsonParams = (jsonRoot.params || []).filter((p) => p && typeof p === "object");

/* ============================================================== 1 ==
 * THE CANVAS IS SUPPRESSED, in all five places it was declared.
 *
 * Five, and they fail differently. A leftover chain_params entry puts the Bank
 * Editor back on the host's own grid; a leftover hierarchy link leaves a row
 * that opens nothing (or worse — strip the plugin side only and the module.json
 * link becomes a FIFTH knob on Main labelled BEDITO, guessed float 0..1);
 * `host_canvas_ui` is the one dAVEBOx reads to host the canvas IN PREFERENCE to
 * the grid; packaging ships a file nothing declares; and the installer, which
 * only ever COPIES, leaves a canvas.js from an older install on the device
 * forever — which dAVEBOx loads straight off disk regardless of the flag.
 */
{
  ok(!pluginChain.some((p) => p.type === "canvas"),
     "the plugin's chain_params declares no canvas");
  ok(!/canvas_script/.test(cpp),
     "and the C source names no canvas script anywhere");

  ok(moduleJson.capabilities.chain_params === undefined,
     "module.json declares no chain_params — it held only the canvas entry");
  ok(moduleJson.capabilities.host_canvas_ui === undefined,
     "module.json declares no host_canvas_ui — this is the one dAVEBOx reads");

  const links = [...(jsonRoot.params || []), ...(pluginRoot.params || [])]
    .filter((p) => p && typeof p === "object")
    .map((p) => p.key);
  ok(!links.includes("editor"), "neither hierarchy links an `editor` row");

  /* Two separate things, and the second bit TWICE in one session. `dist/` is
   * not cleaned by hand, so dropping the copy line alone leaves the PREVIOUS
   * build's canvas.js sitting in the package — the artifact still ships the
   * editor while every declaration says it is gone. (The same trap then shipped
   * a stale help.json.) So the build must WIPE dist, not remove one file. */
  const copies = buildSh.split("\n").filter(
    (l) => /canvas\.js/.test(l) && !/^\s*#/.test(l) && !/^\s*rm\s/.test(l));
  ok(copies.length === 0,
     "build.sh copies no canvas.js into dist, got " + JSON.stringify(copies));
  ok(/^\s*rm -rf ["']?dist/m.test(buildSh),
     "and WIPES dist before packaging, so nothing from an older build survives");

  ok(/rm -f [^\n]*canvas\.js/.test(installSh),
     "install.sh REMOVES any canvas.js left on the device by an older install");
}

/* ============================================================== 2 ==
 * THE TWO DECLARATIONS AGREE.
 *
 * Same keys, same order, same meta. This is the whole point of the file: a chain
 * slot reads one copy and the Master FX bus reads the other, so a field edited
 * on one side alone is an effect that behaves differently depending on where you
 * put it, with nothing logged and nothing to see in the diff.
 */
{
  const chainKeys = pluginChain.map((p) => p.key);
  const jsonKeys = jsonParams.map((p) => p.key);
  ok(JSON.stringify(chainKeys) === JSON.stringify(jsonKeys),
     "same params in the same order: " + JSON.stringify(chainKeys)
     + " vs " + JSON.stringify(jsonKeys));

  /* Every field the grid's widget choice or readout depends on. `name` picks the
   * label, `type`+`min`+`max`+`step` pick the WIDGET (an int spanning <= 24 draws
   * a big number where a float draws an arc), `options`/`short_options` pick the
   * enum square's text, `unit` and `default` the readout. */
  const FIELDS = ["name", "type", "min", "max", "step", "default",
                  "unit", "options", "short_options"];
  for (const c of pluginChain) {
    const j = jsonParams.find((p) => p.key === c.key);
    if (!j) continue;                       /* reported by the key check above */
    for (const f of FIELDS)
      ok(JSON.stringify(c[f]) === JSON.stringify(j[f]),
         `${c.key}.${f} agrees (${JSON.stringify(c[f])} vs ${JSON.stringify(j[f])})`);
  }

  ok(JSON.stringify(pluginRoot.knobs) === JSON.stringify(jsonRoot.knobs),
     "both hierarchies name the same knob row");
  ok(pluginRoot.name === jsonRoot.name,
     `both call the page ${JSON.stringify(jsonRoot.name)}`);
}

/* ============================================================== 3 ==
 * THE KNOB ROW IS PLAYABLE.
 *
 * Eight physical encoders, so a ninth knob is a cell that cannot be reached; a
 * knob naming a key the DSP does not serve is a live-looking cell that reads and
 * writes nothing. set_param is the list of keys that actually exist.
 */
{
  const served = [...cpp.matchAll(/strcmp\(key,\s*"([a-z0-9_]+)"\)\s*==\s*0/g)]
    .map((m) => m[1]);
  for (const [who, lvl] of [["plugin", pluginRoot], ["module.json", jsonRoot]]) {
    const knobs = lvl.knobs || [];
    ok(knobs.length <= 8, `${who}: ${knobs.length} knobs (max 8)`);
    const orphan = knobs.filter((k) => !served.includes(k));
    ok(orphan.length === 0, `${who}: every knob names a served key, stray `
       + JSON.stringify(orphan));
  }
}

/* ============================================================== 4 ==
 * THE MODE SQUARES ARE DISTINGUISHABLE.
 *
 * The generated grid draws an enum as a two-line square of at most three
 * characters a line, so "M-Tap 1" and "M-Tap 2" BOTH reduce to M / TAP — two
 * different reverb modes rendering as literally the same cell. That is a real
 * defect the canvas never had (it carried these exact three-char labels), and it
 * is invisible in any test that only counts params.
 *
 * Asserted structurally rather than by importing the host's font: nine options
 * need nine short forms, each <= 3 chars, all distinct.
 */
{
  const mode = pluginChain.find((p) => p.key === "mode");
  const short = mode && mode.short_options;
  ok(Array.isArray(short) && short.length === (mode.options || []).length,
     "every mode option has a short form");
  ok((short || []).every((s) => typeof s === "string" && s.length > 0 && s.length <= 3),
     "each fits the 3-char square, got " + JSON.stringify(short));
  ok(new Set(short || []).size === (short || []).length,
     "and no two modes render as the same square, got " + JSON.stringify(short));
}

console.log(failures === 0
  ? `\nALL HIERARCHY CHECKS PASSED (${pluginChain.length} params, 2 declarations)`
  : `\n${failures} FAILED`);
process.exit(failures === 0 ? 0 : 1);

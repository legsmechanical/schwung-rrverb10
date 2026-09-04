#!/usr/bin/env bash
# Build the RRVerb-10 module for Schwung (ARM64).
#
# Uses Docker for cross-compilation automatically. Set CROSS_PREFIX to skip it.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== RRVerb-10 Module Build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (inside Docker, or with a cross-compiler on PATH) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"

echo "=== Building RRVerb-10 Module ==="
echo "Cross prefix: $CROSS_PREFIX"

mkdir -p build dist/rrverb10

# -O3, not -Ofast. BossEmu is exact 16-bit integer arithmetic with deliberate
# signed overflow/saturation semantics; the resampler is ordinary float FIR.
# Neither wants -ffast-math, and the emulator must not be given licence to
# reassociate its accumulator.
echo "Compiling DSP plugin..."
${CROSS_PREFIX}g++ -O3 -shared -fPIC \
    -DNDEBUG \
    src/dsp/rrverb10.cpp \
    src/dsp/BossEmu.cpp \
    -o build/rrverb10.so \
    -Isrc/dsp \
    -lm

# The shared library MUST be named <id>.so — the chain host loads audio FX as
# modules/audio_fx/<id>/<id>.so and never consults module.json's `dsp` field.
echo "Packaging..."
cat src/module.json  > dist/rrverb10/module.json
# dist/ is not cleaned between builds, so simply not copying canvas.js leaves
# the LAST build's copy sitting in the package -- caught doing exactly that on
# the first suppressed build here. Remove it explicitly, or the tarball still
# ships the editor dAVEBOx loads off disk.
rm -f dist/rrverb10/canvas.js

# The on-device Bank Editor is SUPPRESSED -- the host draws its own generated
# knob grid from ui_hierarchy + chain_params. src/canvas.js stays in the tree,
# unpackaged, so restoring this one line (plus host_canvas_ui and the two editor
# declarations) puts the editor back exactly as it was.
# cat src/canvas.js    > dist/rrverb10/canvas.js
[ -f src/help.json ] && cat src/help.json > dist/rrverb10/help.json
[ -f LICENSE ]       && cat LICENSE       > dist/rrverb10/LICENSE
cat build/rrverb10.so   > dist/rrverb10/rrverb10.so
chmod +x dist/rrverb10/rrverb10.so

# NOTE: roms/ is deliberately NOT packaged. The program ROM is not ours to
# redistribute; the user supplies it through the host's `assets` mechanism.
mkdir -p dist/rrverb10/roms
cat > dist/rrverb10/roms/README.txt <<'EOF'
Place your own dump of the unit's 16 KB program ROM here as:

    rrv10.bin      (16384 bytes, crc32 4B1D6D75)

It is not distributed with this module. Until it is present the effect loads
and passes audio through unprocessed, and the module reports the missing file.
EOF

cd dist
tar -czf rrverb10-module.tar.gz rrverb10/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output:  dist/rrverb10/"
echo "Tarball: dist/rrverb10-module.tar.gz"

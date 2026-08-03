#!/usr/bin/env bash
# Build the RRV-10 module for Schwung (ARM64).
#
# Uses Docker for cross-compilation automatically. Set CROSS_PREFIX to skip it.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== RRV-10 Module Build (via Docker) ==="
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

echo "=== Building RRV-10 Module ==="
echo "Cross prefix: $CROSS_PREFIX"

mkdir -p build dist/rrv10

# -O3, not -Ofast. BossEmu is exact 16-bit integer arithmetic with deliberate
# signed overflow/saturation semantics; the resampler is ordinary float FIR.
# Neither wants -ffast-math, and the emulator must not be given licence to
# reassociate its accumulator.
echo "Compiling DSP plugin..."
${CROSS_PREFIX}g++ -O3 -shared -fPIC \
    -DNDEBUG \
    src/dsp/rrv10.cpp \
    src/dsp/BossEmu.cpp \
    -o build/rrv10.so \
    -Isrc/dsp \
    -lm

# The shared library MUST be named <id>.so — the chain host loads audio FX as
# modules/audio_fx/<id>/<id>.so and never consults module.json's `dsp` field.
echo "Packaging..."
cat src/module.json  > dist/rrv10/module.json
cat src/canvas.js    > dist/rrv10/canvas.js
[ -f src/help.json ] && cat src/help.json > dist/rrv10/help.json
[ -f LICENSE ]       && cat LICENSE       > dist/rrv10/LICENSE
cat build/rrv10.so   > dist/rrv10/rrv10.so
chmod +x dist/rrv10/rrv10.so

# NOTE: roms/ is deliberately NOT packaged. The program ROM is not ours to
# redistribute; the user supplies it through the host's `assets` mechanism.
mkdir -p dist/rrv10/roms
cat > dist/rrv10/roms/README.txt <<'EOF'
Place your own dump of the unit's 16 KB program ROM here as:

    rrv10.bin      (16384 bytes, crc32 4B1D6D75)

It is not distributed with this module. Until it is present the effect loads
and passes audio through unprocessed, and the module reports the missing file.
EOF

cd dist
tar -czf rrv10-module.tar.gz rrv10/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output:  dist/rrv10/"
echo "Tarball: dist/rrv10-module.tar.gz"

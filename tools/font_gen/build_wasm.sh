#!/usr/bin/env bash
# Build font_gen.wasm + font_gen.js using Emscripten.
# Prerequisites: Emscripten SDK activated (source emsdk_env.sh before running).
#
# Usage:
#   cd tools/font_gen
#   ./build_wasm.sh
#
# Output: docs/font_gen.js  docs/font_gen.wasm

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build_wasm"
OUT_DIR="$REPO_ROOT/docs"

echo "=== Configuring (emcmake) ==="
emcmake cmake "$SCRIPT_DIR" \
    -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFONT_GEN_WASM=ON

echo "=== Building ==="
emmake cmake --build "$BUILD_DIR" --target font_gen -j "$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"

echo "=== Copying to $OUT_DIR ==="
cp "$BUILD_DIR/font_gen.js"   "$OUT_DIR/"
cp "$BUILD_DIR/font_gen.wasm" "$OUT_DIR/"

echo "Done: docs/font_gen.js + docs/font_gen.wasm"
echo "Bundle sizes:"
ls -lh "$OUT_DIR/font_gen.js" "$OUT_DIR/font_gen.wasm"

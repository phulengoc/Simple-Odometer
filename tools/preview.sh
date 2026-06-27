#!/usr/bin/env bash
# Build and run the host preview harness, then convert the PPM frame to PNG.
# Usage: tools/preview.sh [speed] [gear] [odo] [out.png]
set -euo pipefail
cd "$(dirname "$0")/.."

SPEED="${1:-37}"
GEAR="${2:-3}"
ODO="${3:-4905.5}"
RPM="${4:-6000}"
OUT="${5:-tools/preview.png}"
PPM="tools/preview_out.ppm"

c++ -std=c++17 -O2 -Imain \
    tools/host_preview.cpp main/tach_ring.cpp main/text8x8.c \
    -lm -o tools/host_preview

./tools/host_preview "$SPEED" "$GEAR" "$ODO" "$RPM" "$PPM"

# PPM -> PNG via the font venv's Pillow (host-only convenience).
.fontvenv/bin/python - "$PPM" "$OUT" <<'PY'
import sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[2])
print("wrote", sys.argv[2])
PY

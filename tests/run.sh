#!/bin/bash
# Builds and runs the driver and DSP tests.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/tests"
mkdir -p "$OUT"
# A test build of the driver: no broker, and a hook for the test to hand it the ring.
rm -rf "$OUT/AnalogAudioDevice.driver"
mkdir -p "$OUT/AnalogAudioDevice.driver/Contents/MacOS"
clang -Wall -Wextra -Werror -O2 -DAAD_TESTING -bundle -framework CoreAudio -framework CoreFoundation \
  -o "$OUT/AnalogAudioDevice.driver/Contents/MacOS/AnalogAudioDevice" "$ROOT/driver/AnalogAudioDevice.c"
cp "$ROOT/driver/Info.plist" "$OUT/AnalogAudioDevice.driver/Contents/Info.plist"
clang -Wall -O2 -framework CoreAudio -framework CoreFoundation -o "$OUT/driver_smoke" "$ROOT/tests/driver_smoke.c"
clang -Wall -O2 -o "$OUT/dsp_test" "$ROOT/tests/dsp_test.c" "$ROOT/app/Engine/dsp.c"
echo "== driver"; "$OUT/driver_smoke" "$OUT/AnalogAudioDevice.driver"
echo; echo "== dsp"; "$OUT/dsp_test"

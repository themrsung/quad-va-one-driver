#!/bin/bash
# Builds and signs build/AnalogAudioDevice.driver, build/me.sjun.AnalogAudio.broker and
# build/Analog Audio.app. SIGN_IDENTITY overrides the signing identity.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
IDENTITY="${SIGN_IDENTITY:-Apple Development: themrsung@gmail.com (V55MLK6H33)}"
MIN_MACOS="14.0"
TARGET="arm64-apple-macos$MIN_MACOS"
CFLAGS=(-Wall -Wextra -Werror -O2 -target "$TARGET")

DRIVER="$BUILD/AnalogAudioDevice.driver"
BROKER="$BUILD/me.sjun.AnalogAudio.broker"
APP="$BUILD/Analog Audio.app"

rm -rf "$DRIVER" "$BROKER" "$APP" "$BUILD/obj"
mkdir -p "$DRIVER/Contents/MacOS" "$APP/Contents/MacOS" "$BUILD/obj"

echo "==> Driver"
clang "${CFLAGS[@]}" -bundle -framework CoreAudio -framework CoreFoundation \
  -o "$DRIVER/Contents/MacOS/AnalogAudioDevice" "$ROOT/driver/AnalogAudioDevice.c"
cp "$ROOT/driver/Info.plist" "$DRIVER/Contents/Info.plist"
codesign --force --sign "$IDENTITY" --timestamp=none "$DRIVER"

# The broker and the app check each other's signatures against this team.
TEAM_ID="$(codesign -dv "$DRIVER" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
[ -n "$TEAM_ID" ] && [ "$TEAM_ID" != "not set" ] || { echo "Signing identity has no team ID"; exit 1; }

echo "==> Broker"
clang "${CFLAGS[@]}" -DAAD_TEAM_ID="\"$TEAM_ID\"" -o "$BROKER" "$ROOT/broker/broker.c"
codesign --force --sign "$IDENTITY" --options runtime --timestamp=none --identifier me.sjun.AnalogAudio.broker "$BROKER"

echo "==> App"
for src in engine dsp; do
  clang "${CFLAGS[@]}" -DAAD_TEAM_ID="\"$TEAM_ID\"" -c "$ROOT/app/Engine/$src.c" -o "$BUILD/obj/$src.o"
done
swiftc -O -wmo -swift-version 5 -target "$TARGET" \
  -import-objc-header "$ROOT/app/Engine/Bridge.h" \
  -o "$APP/Contents/MacOS/AnalogAudio" \
  "$ROOT"/app/Sources/*.swift "$BUILD/obj/engine.o" "$BUILD/obj/dsp.o"
cp "$ROOT/app/Info.plist" "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
codesign --force --sign "$IDENTITY" --options runtime --timestamp=none "$APP"

for x in "$DRIVER" "$BROKER" "$APP"; do codesign --verify --strict "$x"; done
echo "==> Signed with: $IDENTITY (team $TEAM_ID)"

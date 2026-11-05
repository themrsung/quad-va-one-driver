#!/bin/bash
# Installs the app, and the driver and broker if they changed (those need sudo; a new driver
# restarts Core Audio). Run ./build.sh first.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
HAL="/Library/Audio/Plug-Ins/HAL"
NEW_DRIVER="$ROOT/build/AnalogAudioDevice.driver"
OLD_DRIVER="$HAL/AnalogAudioDevice.driver"
LABEL="me.sjun.AnalogAudio.broker"
NEW_BROKER="$ROOT/build/$LABEL"
OLD_BROKER="/Library/PrivilegedHelperTools/$LABEL"
PLIST="/Library/LaunchDaemons/$LABEL.plist"

[ -d "$NEW_DRIVER" ] && [ -f "$NEW_BROKER" ] || { echo "Run ./build.sh first."; exit 1; }

cdhash() { [ -e "$1" ] && codesign -dvvv "$1" 2>&1 | sed -n 's/^CDHash=//p'; }

osascript -e 'quit app "Analog Audio"' 2>/dev/null || true
sleep 1

if [ "$(cdhash "$NEW_BROKER")" = "$(cdhash "$OLD_BROKER" || true)" ] && cmp -s "$ROOT/broker/$LABEL.plist" "$PLIST"; then
  echo "Broker unchanged."
else
  echo "Installing the broker…"
  sudo launchctl bootout "system/$LABEL" 2>/dev/null || true
  sudo mkdir -p /Library/PrivilegedHelperTools
  sudo cp "$NEW_BROKER" "$OLD_BROKER"
  sudo chown root:wheel "$OLD_BROKER"
  sudo chmod 755 "$OLD_BROKER"
  sudo cp "$ROOT/broker/$LABEL.plist" "$PLIST"
  sudo chown root:wheel "$PLIST"
  sudo chmod 644 "$PLIST"
  sudo launchctl bootstrap system "$PLIST"
fi

if [ "$(cdhash "$NEW_DRIVER")" = "$(cdhash "$OLD_DRIVER" || true)" ]; then
  echo "Driver unchanged; leaving Core Audio alone."
else
  echo "Installing the driver…"
  sudo rm -rf "$OLD_DRIVER"
  sudo cp -R "$NEW_DRIVER" "$HAL/"
  sudo chown -R root:wheel "$OLD_DRIVER"
  echo "Restarting Core Audio (audio drops out for a moment)…"
  sudo killall coreaudiod
  sleep 3
fi

rm -rf "/Applications/Analog Audio.app"
cp -R "$ROOT/build/Analog Audio.app" /Applications/
open "/Applications/Analog Audio.app"
echo "Installed."

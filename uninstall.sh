#!/bin/bash
# Removes the app, the broker and the driver, then restarts Core Audio.
set -euo pipefail
LABEL="me.sjun.AnalogAudio.broker"
osascript -e 'quit app "Analog Audio"' 2>/dev/null || true
rm -rf "/Applications/Analog Audio.app"
sudo launchctl bootout "system/$LABEL" 2>/dev/null || true
sudo rm -f "/Library/LaunchDaemons/$LABEL.plist" "/Library/PrivilegedHelperTools/$LABEL"
sudo rm -rf "/Library/Audio/Plug-Ins/HAL/AnalogAudioDevice.driver"
sudo killall coreaudiod
echo "Removed. Set the system output back to your amp in Sound settings."

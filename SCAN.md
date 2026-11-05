# System scan, mini-m6 (2026-11-05)

Read-only. Re-run the audio part with `swift scripts/scan-audio.swift [name]`.

## Host

- Mac mini, Apple Silicon (arm64), macOS 27.0.1 (26A434). SIP enabled.
- Xcode in /Applications and the Command Line Tools; Swift 6.4.
- Code signing: one identity, "Apple Development: themrsung@gmail.com (V55MLK6H33)". No
  Developer ID.

## The QUAD (VA-One's USB input)

| | |
|---|---|
| Name / UID | QUAD USB / `AppleUSBAudioEngine:VIA Technologie:QUAD USB:131000:1` |
| Role | Default output and default system output |
| Sample rates | 44.1, 48, 88.2, 96, 176.4, 192 kHz |
| Formats | 24-bit int at 44.1–96 kHz; 16-bit int at every rate (176.4 and 192 kHz are 16-bit only) |
| **Running now** | **48 kHz, 2 ch, 16-bit int**: 24-bit is available at this rate but not selected |
| Controls | Mute (main, settable); **no volume** at any element (see keyboard-troubleshooting's hardware/volume-keys-unmute-only-when-volume-changes) |
| Latency | device 50 frames, safety offset 50; buffer 15–4096 frames |

Other outputs: RØDE NT-USB+ (48 kHz 24-bit, volume and mute), Mac mini Speakers (float, volume and
mute), DELL S2725QC (DisplayPort) and LG HDR 4K (HDMI), both without controls.

## Audio plug-ins installed

`/Library/Audio/Plug-Ins/HAL`: only `ParrotAudioPlugin.driver` (`com.apple.audio.ParrotAudioPlugin`,
Apple's). No third-party virtual devices (no BlackHole, Loopback, SoundSource).

## What this replaces: QUAD Keep-Alive

- `/Applications/QUAD Keep-Alive.app` (`me.sjun.QuadKeepAlive`), running, source in
  `~/src/quad-keep-alive` (menu-bar app, Swift, 1,363 lines).
- Every `intervalSeconds` = 60 s it opens the QUAD's output, plays one `pulseMilliseconds` = 1000 ms
  burst of `toneHz` = 22,000 Hz at `levelDBFS` = −80 dBFS (shaped, with fades), then tears the
  audio stack down. `ditherOnly` = 0 (±1 LSB noise instead of a tone, off). Pauses on system
  sleep; `pauseWhenDisplayAsleep` = 0. Option to skip while audio is playing.
- It takes no power assertions itself. Separately, launch agent `com.user.caffeinate`
  (`caffeinate -s -i`, KeepAlive) keeps the Mac awake; that's not part of the QUAD fix.
- The pulse being a tone, not just an open stream, suggests the amp's auto-standby listens for
  signal, so a passthrough sending digital silence may still let it sleep. To confirm.

Also in `~/Projects/claude-local-default`: `quad-va-one-ir-codes.md` / `.ir` (remote codes, not
read for this).

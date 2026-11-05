# quad-va-one-driver

A virtual output device for the QUAD VA-One's USB input: apps play to it, and it passes the audio
to the QUAD with dynamic range expansion applied, keeping the amp awake without a separate
keep-alive app. Replaces QUAD Keep-Alive (`~/src/quad-keep-alive`). Status: planning; see
[SCAN.md](SCAN.md) for the system it runs on.

## Proposed shape (not built)

Two parts, because a CoreAudio driver can't safely open another audio device from inside
coreaudiod's I/O cycle:

1. **Driver** (`QUAD VA-One.driver`, an AudioServerPlugIn in `/Library/Audio/Plug-Ins/HAL`): a
   virtual 2-channel output, "QUAD VA-One", set as the default output, with a loopback input the
   helper reads, the same pattern as BlackHole. No DSP here.
2. **Helper** (menu-bar app, replacing QUAD Keep-Alive): reads the loopback, applies the
   expansion, and plays to the QUAD through AUHAL at 24-bit, matching the virtual device's sample
   rate so nothing is resampled. Keeps the QUAD's stream open; when the input has been silent for a
   while, it sends the keep-alive pulse (22 kHz, −80 dBFS, 1 s every 60 s) itself.

Alternatives to weigh: a process tap (`AudioHardwareCreateProcessTap`, macOS 14.2+) could capture
system audio without a driver, but the output still needs a device to route to; an aggregate device
doesn't process audio.

## Open questions

- Which expansion: upward (restore dynamics to compressed masters) or downward (push the floor
  down, e.g. as a gentle gate); per band or broadband; attack, release, ratio, threshold.
- Does the VA-One sleep on digital silence with the stream open, or only when the stream closes?
  That decides whether the helper needs the pulse at all.
- Installing the driver needs admin rights (copy to `/Library/Audio/Plug-Ins/HAL`, restart
  coreaudiod), and it's signed with the Apple Development certificate (local use only).

# quad-va-one-driver

A virtual output device, **Analog audio device**, for the QUAD VA-One's USB input: apps play to
it, and the menu-bar app **Analog Audio** passes the audio to the QUAD (or any other output device
you pick), either untouched or made easier on the ears (Comfort), keeping the amp awake without a
separate keep-alive app. Replaces QUAD Keep-Alive (`~/src/quad-keep-alive`). See [SCAN.md](SCAN.md)
for the system it runs on.

Status: installed on mini-m6 and in testing.

## Build and install

```sh
./build.sh       # build/: the driver, the broker and the app, signed
tests/run.sh     # driver (loaded with a fake host), shared memory and DSP tests
./install.sh     # app to /Applications; broker and driver only if they changed (sudo; a new driver restarts coreaudiod)
./uninstall.sh
```

Signing uses the Apple Development identity (override with `SIGN_IDENTITY`), so it's for this
Mac only. No permissions to grant: the app never records audio input, so macOS doesn't show the
microphone as in use. Then quit QUAD Keep-Alive, which this replaces.

## Modes

One global setting for all audio, chosen from the menu-bar icon's menu:

```
✓ Passthrough
  Comfort          ▸   plain
                       ─────
                       music        ▸  k-pop, j-pop, us-pop, global-pop,
                                       modern, classical, baroque, pre-baroque
                       film         ▸  tv, cinema
                       speech       ▸  podcast, news
                       experimental ▸  propaganda, war-cry, dramatic, tragic, super-hdr
```

Picking a profile switches to Comfort with that profile; the active one is checked, and the choice
is kept across restarts.

## Output device

The downstream device is chosen in the same menu (`Output ▸`), listing every output device except
the virtual one itself (that would loop). The QUAD USB is the default; the list is meant for another
QUAD or an analog amp behind any interface, but any output works. The choice is stored by device UID,
so it survives restarts and the device being unplugged and replugged.

- **Format**: the helper opens the chosen device at the virtual device's sample rate if it supports
  it (otherwise it switches the virtual device to a rate both share) and at its deepest integer
  format at that rate (24-bit on the QUAD), so nothing is resampled.
- **Keep-alive pulse**: a per-device toggle, on by default for the QUAD and off for others; plenty
  of devices don't sleep, and the pulse is only there for the amps that do.
- **Unplugged**: if the chosen device disappears, the helper shows it as missing in the menu and
  holds output (apps keep playing into the virtual device, into silence) until it returns, rather
  than falling back to another device unasked.

### Passthrough

Everything reaches the QUAD exactly as it does today, when apps play to the QUAD directly: no
processing, no gain change, no dither, no resampling. The only addition is the keep-alive pulse
(22 kHz, −80 dBFS, 1 s every 60 s, as QUAD Keep-Alive sends it), sent only while the input is
silent, so playing audio stays bit-exact.

### Comfort

The whole mix is processed with the selected profile, then sent to the QUAD; switching crossfades
over 50 ms. The keep-alive pulse is added after the processing, so it works the same as in
passthrough.

Every profile applies the same gain curve, aimed at listening fatigue rather than at removing
treble: a −1.5 dB dip at 3.2 kHz (Q 0.9), where hearing is most sensitive and harshness lives, and
a high shelf easing down to −2.5 dB above about 7 kHz. Lows are untouched; 1 kHz is down about
0.2 dB. **plain** is that curve and nothing else: passthrough plus the curve, no limiter.

The other profiles add a broadband, stereo-linked expander (20 ms RMS detector with per-profile
attack and release): upward above one threshold, to give back dynamics that mastering compressed
away, and downward below another, to push noise floors down. Makeup gain pulls the result back
under a 2 ms lookahead limiter at −1 dBFS. The curve is in `app/Engine/dsp.h`, the profile values
in `app/Sources/Presets.swift`; both are a first pass, to be tuned by ear.

## How it works

Three parts. A CoreAudio driver can't safely open another audio device from inside coreaudiod's
I/O cycle, so the app does the playing; and reading the driver's audio back through an input stream
would light up the microphone indicator, so the two share memory instead:

1. **Driver** (`driver/`, `AnalogAudioDevice.driver`, an AudioServerPlugIn): a virtual 2-channel
   float output, "Analog audio device", at 44.1–192 kHz, output only. Each IO cycle it appends the
   mix to the shared ring. No DSP. Its clock can be nudged through a custom property, `'rAdj'`,
   within ±0.1%.
2. **Broker** (`broker/`, a LaunchDaemon running as `nobody`): owns the ring (`shared/AADShared.h`,
   32768 stereo float frames) and hands the same memory to the driver and the app over XPC. It's
   needed because the driver runs sandboxed inside Apple's Core Audio driver service and can only
   reach mach services it declares (`AudioServerPlugIn_MachServices`). It only answers Apple's
   driver service and our own app, checked by code signature, so nothing else can read system
   audio from it.
3. **App** (`app/`, `Analog Audio.app`, menu bar only): the Swift side handles the menu, settings
   and devices; the audio path is C (`app/Engine/`). Its output IOProc on the chosen device reads
   the ring directly. The two devices run on different clocks, so once a second the app compares
   the ring's fill with its target and steers the virtual device's clock to the output's through
   `'rAdj'`. That's what keeps Passthrough bit-exact: no resampler, no dropped or repeated
   samples. On launch it makes the virtual device the system output; on quit it hands the system
   output back to the chosen device.

## Known caveats

- The output device is kept running, so coreaudiod holds its usual "audio playing" sleep
  assertion all the time (already the case here with `com.user.caffeinate`).
- Apps see no output latency beyond the virtual device's, so A/V sync is off by the ring's fill
  (~20 ms at 48 kHz) plus the output device's own latency.

## Later

Per-app profiles, and per-website ones for Chrome, are deferred. They'd need processing before
CoreAudio mixes the apps together (the driver's per-client `ProcessOutput` hook, or one process tap
per app) and a Chrome extension to report which site is playing.

## Open questions

- Tuning the curve and the profiles by ear; whether some profiles want multiband instead of
  broadband; whether a de-esser (dynamic, 5–10 kHz) helps beyond the static curve.
- Does the VA-One sleep on digital silence with the stream open, or only when the stream closes?
  That decides whether the helper needs the pulse at all.

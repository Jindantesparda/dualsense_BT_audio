# DualSense Music

Plays music on a **DualSense over Bluetooth** using both of its audio outputs at once:

* the **built-in speaker** (Opus audio inside HID reports), and
* the **voice-coil actuators** in the grips, driven with the **full-band audio**
  so you feel the track itself, not just a kick drum.

Two front ends, one engine:

| File | What it is |
|---|---|
| **`DualSenseMusic.exe`** | The GUI player (drag & drop, transport, sliders) |
| **`ds5play.exe`** | Command-line player for scripting and quick tests |
| **`Play on controller.bat`** | Drop an audio file on it, or double-click to open the GUI |

No emulator, SDL or runtime DLLs are needed. Opus is linked into the executables.

---

## Quick start

1. Pair the controller over Bluetooth and make sure Windows shows it as connected.
   The GUI shows a green **Connected** pill when it can see the DualSense.
2. Open **`DualSenseMusic.exe`**, then drag an audio file onto the window (or use
   **Open file...**). Playback starts immediately.
3. **Test** plays a built-in loop: a kick you feel plus a melody you hear, useful
   for checking the setup without a file.

Supported files: **wav, mp3, flac, ogg**.

### GUI controls

* **Play / Pause**, **Stop**, **Test**
* Click or drag the progress bar to seek
* **Loop** to repeat the file
* **System audio** captures whatever your PC is playing and sends that to the
  controller instead of a file. The small chevron next to it picks *which*
  Windows playback device to capture (see *Using the controller for game audio*)
* **Haptics intensity** - actuator gain, 0-2.00x
* **Haptic tone** - 0% = bass only (deep rumble), 100% = full-band audio
  (default 90%, i.e. the track itself)
* **Speaker volume** - the controller's own volume register, 0-100
* **Speaker level** - level of the encoded stream, 0-2.00x
* **Space** toggles play/pause, **Esc** stops
* Click the **Connected** pill to rescan for the controller

Changes to the sliders apply live, including while playing.

---

## Command line

```
ds5play [options] <audio file>
ds5play --test [options]

  -l, --list               list DualSense HID interfaces and exit
  -d, --device N           use interface N from --list
  -t, --test               play a built-in test loop (no file needed)
      --sweep              diagnostic: 40 Hz -> 1 kHz glide on the actuators only
      --system-audio       play the PC's output (WASAPI loopback) instead of a file
      --audio-device N     with --system-audio, capture device N from --list-audio
      --list-audio         list Windows playback devices (with channel counts)
      --loop               repeat the file forever
      --start SECONDS      start at this position
      --duration SECONDS   stop after this many seconds
      --haptics-gain X     actuator level multiplier (default 1.0)
      --tone N             haptic tone, 0 = bass only, 100 = full band (default 90)
      --speaker-gain X     speaker level multiplier (default 1.0)
      --speaker-volume N   controller speaker volume 0..100 (default 100)
      --no-haptics         speaker only
      --no-speaker         haptics only
  -v, --verbose            print timing statistics
  -h, --help               help
```

Examples:

```bat
ds5play.exe "C:\Music\song.mp3"
ds5play.exe "C:\Music\song.flac" --loop
ds5play.exe song.mp3 --tone 100 --haptics-gain 1.6 --speaker-volume 60
ds5play.exe --test -v
```

`-v` prints the report cadence, e.g.

```
stats: 1172 reports (25.00 s), avg write 0.33 ms, max 0.65 ms, max gap 21.65 ms, late slots 0
```

`late slots 0` means every report left exactly on its 21.333 ms boundary.

---

## Using the controller for game audio

Windows does **not** expose the DualSense as a playback device over Bluetooth:
Sony's Bluetooth audio path is proprietary, and only the USB connection appears
as a 4-channel sound card. No application can make the controller show up in a
game's audio settings.

Instead, tick **System audio** in the GUI (or use `--system-audio`). The player
captures whatever your default output device is playing with WASAPI loopback
and forwards it to the controller's speaker and actuators.

Because this is a mirror, the game still plays on the device it selected. To
hear game audio *only* on the controller:

1. Use a virtual audio device (VB-Cable, Voicemeeter) - or an existing one.
2. Set the game's output device to that virtual input.
3. In the player, tick **System audio** and pick that same device from the
   chevron menu (or use `--audio-device N` from `--list-audio`).
4. Press Play.

The cable swallows the sound, the player captures it and the controller becomes
your only speaker. (Muting the normal speakers usually works too, but some
drivers also mute the loopback signal, so the cable is the reliable route.)

Latency through this path is roughly 100-200 ms - capture buffer plus Bluetooth.
That is fine for most games but noticeable in rhythm games.

---

## Haptics

The controller's actuator stream is sampled at **3 kHz**, so it reproduces real
audio up to ~1.2 kHz - `ds5play --sweep` glides a tone from 40 Hz to 1 kHz and
you can follow the pitch in your hands.

The catch is that music is not mixed for haptics: bass is typically 15-20 dB
louder than everything else, so a flat feed feels like a sequence of thumps.
The chain compensates for that:

```
music -> 70 Hz high-pass -> 4th-order anti-alias at 1.2 kHz
      -> +12 dB presence shelf above 250 Hz -> tone blend -> 15:1 decimate -> limiter
```

The sub-bass cut removes the part that only thumps, and the shelf lifts melody,
vocals and texture to roughly the same felt level as the kick. The **tone**
control crossfades back toward bass-only if you want weight instead of detail.

A per-sample peak limiter with an 80 ms release keeps loud passages from
clipping into 8-bit distortion, so quiet detail stays present in the grips
without the loud parts crackling.

---

## Why playback used to stutter (and why it does not now)

Output report 0x39 must leave **every 21.333 ms** (64 haptic samples at 3 kHz).
Windows' `sleep_until` is quantised to the 15.6 ms system timer tick, and
`timeBeginPeriod(1)` had no effect on the tested machine, so roughly every
second report was late and the controller's buffer ran dry - audible as
stuttering.

The engine now waits on a **high-resolution waitable timer**
(`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) and spins the final ~1 ms, which keeps
delivery within a fraction of a millisecond. Measured before/after on the same
machine:

| | late reports | worst gap |
|---|---|---|
| `sleep_until` | 455 / 1172 | 46.6 ms |
| high-resolution timer | **0 / 1172** | **21.7 ms** |

Reports are also prepared by a **separate producer thread** that stays about
200 ms ahead, so decoding, haptics processing and Opus encoding can never delay
a write. The sending thread itself runs at MMCSS *Audio* critical priority, and
the controller is primed with the first three packets so a single late write
cannot drain its buffer.

Measured with every CPU core saturated by a stress test: **0 late reports,
0 skipped slots, worst gap 21.7 ms**.

---

## How it works

Bluetooth output report **0x39** carries one 21.333 ms block:

| Part | Contents |
|---|---|
| Haptics | 128 signed 8-bit samples: 2 channels x 32 frames x 2 blocks, 3 kHz |
| Speaker | two 200-byte Opus frames, 480 samples each, labelled 48 kHz |

The controller consumes 960 Opus samples per block, so the speaker stream is
really clocked at 45 kHz. Audio is decoded at 45 kHz, encoded as 48 kHz, and
decimated 15:1 (45 kHz -> 3 kHz) for the actuators.

Report **0x31** routes audio to the speaker, mutes the headphones, sets the
speaker volume and the +6 dB preamp. Both reports carry a CRC-32 over a
synthetic 0xA2 HIDP header.

The report layout and timing come from KytyPS5's DualSense Bluetooth backend
(`src/libs/dualSenseBluetooth.cpp`), which in turn documents report 0x39 from
[awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) (MIT). Thanks to both.

---

## Troubleshooting

**No controller / red pill**
Pair over Bluetooth: hold **Create + PS** until the light bar flashes, then add
it in *Settings > Bluetooth & devices > Add device*. Click the pill to rescan.
`ds5play --list` shows what Windows exposes.

**"connected over USB"**
This program uses the Bluetooth-only 0x39 report. Unplug the cable and connect
wirelessly. (Over USB the controller exposes a 4-channel audio device instead,
which is a different path and not implemented here.)

**No sound, haptics work**
Raise **Speaker volume** / **Speaker level**. Audio goes to the controller
speaker only while nothing is plugged into its headphone jack.

**Sound works, haptics are weak**
Raise **Haptics intensity**. Very quiet or treble-heavy tracks have less energy
for the actuators; raise **Haptic tone** toward 100% for more texture.

**Haptics feel harsh**
Lower **Haptic tone** toward 0-40% (bass only) or reduce **Haptics intensity**.

**Sound is pitched slightly?**
The 45 kHz figure is what the controller's Opus stream actually runs at; it is
the same compensation the emulator uses. If a future firmware changes it, the
`kSpeakerRate` constant in `src/ds5.h` is the one to adjust.

**Another program is using the controller**
Close it first (the emulator, DSX, Steam Input...). Only one program can drive
the controller's audio at a time.

---

## Rebuilding

`build.bat` builds libopus once, then both executables. It needs g++ (MinGW-w64),
CMake and Ninja (or mingw32-make).

Sources:

* `src/gui.cpp` - Win32 GUI (custom drawn, drag & drop, sliders)
* `src/main.cpp` - command-line front end
* `src/player.cpp/.h` - engine: decoding, haptics, report pacing
* `src/ds5.cpp/.h` - DualSense report building and CRC-32
* `src/hid_win32.cpp/.h` - HID enumeration and output reports
* `src/dsp.h` - haptic filtering, tone blend and limiter

Third-party: `third_party/miniaudio.h` (public domain / MIT-0),
`third_party/opus` (BSD).

SlopAY & MIDIAY
===============

SlopAY is a mostly vibe-coded AY player in C. It includes a Z80 emulator and support for loading AY files, rendering
audio in real time on macOS, or exporting to WAV. It can also print a piano roll of the notes being played.

**Supported formats and platforms:**
- **ZX Spectrum 48K:** beeper-only tracks
- **ZX Spectrum 128K:** AY chip tracks, and hybrid tracks combining AY with beeper
- **Amstrad CPC:** AY chip tracks (with configurable interrupt rates)

The player supports beeper mixing with configurable volume and mix mode.

MIDIAY is a companion tool that provides an interactive shell for controlling the AY chip via MIDI input on macOS.

Note: This is new code largely vibrated into existence to test my AY emulator, so it may be rough around the edges.
The MIDIAY tool is macOS-only due to its use of CoreMIDI/CoreAudio.

Executables
-----------

The project builds two executables:

- `SlopAY` (all platforms; real-time playback on macOS, WAV export everywhere)
- `MIDIAY` (macOS only)

### SlopAY syntax

```text
SlopAY [-V] [-v <percent>] [-b <percent>] [-m <mode>] [-x <mode>] [-P <machine>] [-I <50|300>] [-r <Hz>] [-p] [-s <song>] [-t <seconds>] [-w <file.wav>] [-M <file.mid>] [-B <channel>] <ay_file>
```

- `-v, --volume <percent>`: AY volume (0-100, default 100)
- `-b, --beeper-volume <percent>`, `--beeper <percent>`: beeper volume (0-100, default 22)
- `-m, --beeper-mix <mode>`, `--mix <mode>`: beeper mix mode (`add` or `duck`, default `add`)
- `-x, --stereo-mode <mode>`: stereo mode (`mono`, `abc`, or `acb`, default `abc`)
- `-P, --machine <machine>`: timing profile (`spectrum` or `cpc`, default `spectrum`)
- `-I, --cpc-rate <50|300>`: CPC interrupt-rate override (used only with `-P cpc`, default `50`)
- `-r, --sample-rate <Hz>`: audio sample rate (8000-192000, default 44100)
- `-p, --piano-roll`: print per-frame AY/Beeper notes
- `-s, --song <song>`: 0-based song index from the AY file; if omitted, SlopAY plays songs sequentially starting from the file's first-song index
- `-t, --time <seconds>`: max playback time (`0` uses song length)
- `-w, --wav <file.wav>`: write WAV instead of speaker output (requires finite duration from song length or `-t`); in sequential mode this writes per-song files as `<name>-sN.ext`
- `-M, --midi <file.mid>`: export AY + beeper notes to MIDI (requires finite duration from song length or `-t`); in sequential mode this writes per-song files as `<name>-sN.ext`
- `-B, --midi-beeper-channel <0-15>`: MIDI channel used for beeper notes in `--midi` export (default `3`)
- `-V, --version`: show program version
- `-h, --help`: show command help

Examples:

Intended use: Play the full AY file in default sequential mode (first song, then the next).
```text
SlopAY ProjectAY/Spectrum/Demos/example.ay
```

Intended use: Export every song in sequence to WAV files without overwriting previous songs.
```text
SlopAY -t 60 -w out.wav ProjectAY/Spectrum/Games/example.ay
```
Expected output files:
```text
out-s0.wav
out-s1.wav
...
```

Intended use: Export one selected song to a single WAV file.
```text
SlopAY -s 1 -t 60 -w out.wav ProjectAY/Spectrum/Games/example.ay
```

Intended use: Export every song in sequence to per-song MIDI files.
```text
SlopAY -t 90 -M out.mid ProjectAY/Spectrum/Demos/example.ay
```
Expected output files:
```text
out-s0.mid
out-s1.mid
...
```

Intended use: Export one selected song to MIDI with a custom beeper channel.
```text
SlopAY -s 0 -t 90 -M out.mid -B 10 ProjectAY/Spectrum/Demos/example.ay
```

Intended use: Inspect note activity frame-by-frame with the piano roll.
```text
SlopAY -p -s 0 -t 5 ProjectAY/Spectrum/Demos/example.ay
```
Example piano roll output:
```text
[PR 000123] A=C5     B=E4     C=---    BEEP=---
[PR 000124] A=C5     B=E4     C=G3     BEEP=A4
[PR 000125] A=NOISE  B=---    C=---    BEEP=---
```

Intended use: Compare stereo layouts and platform timing profiles.
```text
SlopAY -x acb -t 30 -w out-acb.wav ProjectAY/Spectrum/Games/example.ay
SlopAY -P cpc -t 30 -w out-cpc.wav ProjectAY/CPC/Games/example.ay
SlopAY -P cpc -I 50 -t 30 -w out-cpc-50hz.wav ProjectAY/CPC/Games/example.ay
SlopAY -r 48000 -t 60 -w out-48khz.wav ProjectAY/Spectrum/Games/example.ay
```

### MIDIAY syntax

```text
MIDIAY
```

`MIDIAY` starts an interactive shell with CoreMIDI/CoreAudio input and AY control commands.

REPL command syntax:

- `<reg> <value>`: write AY register directly (reg `0-15`, value `0-255`)
- `A`..`G`: hold a note
- `.`: stop all notes
- `s`: set envelope shape
- `p`: set envelope period
- `v <0-127>`: set channel volume (MIDI scale)
- `m <0-100>`: set master volume percent
- `r`: cycle reverb delay
- `t`: cycle stereo mode (`mono` -> `abc` -> `acb`)
- `q`: quit

Links
-----

https://worldofspectrum.org/projectay/gdmusic.htm

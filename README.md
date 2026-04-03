SlopAY & MIDIAY
===============

SlopAY is a mostly vibe-coded AY player in C. It includes a Z80 emulator and support for loading AY files, rendering
audio in real time on macOS, or exporting to WAV. It can also print a piano roll of the notes being played. It supports
beeper mixing with configurable volume and mix mode.

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
SlopAY [-v <percent>] [-b <percent>] [-m <mode>] [-p] [-s <song>] [-t <seconds>] [-w <file.wav>] [-M <file.mid>] [-B <channel>] <ay_file>
```

- `-v, --volume <percent>`: AY volume (0-100, default 100)
- `-b, --beeper-volume <percent>`, `--beeper <percent>`: beeper volume (0-100, default 22)
- `-m, --beeper-mix <mode>`, `--mix <mode>`: beeper mix mode (`add` or `duck`, default `add`)
- `-p, --piano-roll`: print per-frame AY/Beeper notes
- `-s, --song <song>`: 0-based song index from the AY file
- `-t, --time <seconds>`: max playback time (`0` uses song length)
- `-w, --wav <file.wav>`: write WAV instead of speaker output (requires finite duration from song length or `-t`)
- `-M, --midi <file.mid>`: export AY + beeper notes to MIDI (requires finite duration from song length or `-t`)
- `-B, --midi-beeper-channel <0-15>`: MIDI channel used for beeper notes in `--midi` export (default `3`)
- `-h, --help`: show command help

Examples:

```text
SlopAY ProjectAY/Spectrum/Demos/example.ay
SlopAY -s 1 -t 60 -w out.wav ProjectAY/Spectrum/Games/example.ay
SlopAY -s 0 -t 90 -M out.mid -B 10 ProjectAY/Spectrum/Demos/example.ay
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
- `r`: cycle reverb delay
- `t`: toggle stereo
- `q`: quit

Links
-----

https://worldofspectrum.org/projectay/gdmusic.htm

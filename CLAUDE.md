# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project snapshot

SlopAY is a C codebase that loads `.ay` music files, emulates a Z80 CPU running the original player code, and
renders AY-3-8912 chip audio either in real time (macOS Core Audio) or to WAV/MIDI. `MIDIAY` is a companion
interactive MIDI shell for controlling the AY chip live (macOS only). See `README.md` for full CLI usage/flags and
`AGENTS.md` for a deeper architecture walkthrough.

## Build

Standard out-of-tree CMake/Ninja build; a `cmake-build-debug` directory is already configured.

```sh
cmake --build cmake-build-debug --target SlopAY   # main player
cmake --build cmake-build-debug --target MIDIAY   # macOS-only interactive MIDI shell
cmake --build cmake-build-debug                   # everything, including tests
```

If build files are missing or `CMakeLists.txt` changes, reconfigure first: `cmake -S . -B cmake-build-debug`.

- Project targets **C99, no higher** — do not use C11 features (`_Atomic`, `<stdatomic.h>`, etc). For state shared
  between the main thread and the macOS audio callback, use `volatile sig_atomic_t` instead.
- Built with `-Wall -Wextra -pedantic`; keep new code warning-clean.
- `SlopAY` and `MIDIAY` only get macOS Core Audio/CoreMIDI sources and frameworks linked when `APPLE` is true
  (`SLOPAY_HAVE_MACOS_AUDIO`); on other POSIX platforms `SlopAY` still builds but runs headless unless writing WAV.

## Tests

```sh
ctest --test-dir cmake-build-debug              # all tests
ctest --test-dir cmake-build-debug -R <name>    # single test, e.g. slopay-chip-test
```

Tests: `slopay-chip-test` (`tests/slopay-chip-test.c`), `slopay-loader-test` (`tests/slopay-loader-test.c`), plus
smoke tests that just run `SlopAY --version` / `--help` and pattern-match the output.

There's no automated audio-correctness test; regression checks are manual, listening to files under
`ProjectAY/*/Demos` and `ProjectAY/*/Games` (a local corpus of demo/game `.ay` files), typically using:
- `-p` to print a per-frame piano-roll view for timing/debug
- `-w out.wav -t <sec>` for a deterministic offline render to compare

## Architecture

- **Loader layer** — `slop-ay/slopay-loader.c` / `.h` parses `.ay` files (big-endian fields, signed relative
  offsets per `ay-format.md`, block terminator at `address == 0`) into `slopay_loader_file_t` /
  `slopay_loader_song_t`.
- **Playback core** — `slop-ay/slopay.c` builds Player v3 memory (`slopay_build_player_v3_memory`), loads song
  blocks (`slopay_load_song_blocks`), seeds Z80 registers, then drives execution via `slopay_run_z80`.
- **CPU/audio bridge** — Z80 port I/O callbacks in `slop-ay/slopay.c` map `OUT` traffic to AY register
  select/data writes and beeper state; this is the key cross-component contract between the emulator and the
  sound chip.
- **Sound generation** — `lib/slopay-chip.c` / `.h` is the AY-3-8912 emulator: register-driven synthesis
  (`slopay_chip_write_register`, `slopay_chip_get_sample`), fixed-point timing, stereo mixing, per-channel mute
  (`slopay_chip_enable_channel`). Tone/noise/envelope generators always run, even when a channel is muted —
  muting only gates the mixed output, since stopping a generator would corrupt phase/LFSR continuity.
- **Z80 emulator** — `slop-ay/slopz80.c` / `.h`. Missing-opcode coverage gaps surface at runtime via
  `slopz80_missing_opcode_snapshot` / `slopay_dump_missing_opcodes`.
- **Output drivers** — `lib/slopay-target-macos.c` (real-time Core Audio callback), `slop-ay/slopay-target-wave.c`
  (offline WAV encode) and `slop-ay/slopay-target-midi.c` (MIDI export) all reuse the same `render_audio` callback
  shape; a new output target should follow the same `init`/`start`/`stop`/`cleanup` lifecycle.
- **Live playback control** — during real-time playback (not WAV/MIDI export), raw terminal input
  (`slopay_term_raw_enter`/`exit` in `slop-ay/slopay.c`, `<termios.h>`) lets `1`/`2`/`3`/`4` toggle AY channels
  A/B/C and the beeper, and `+`/`-`/`0` adjust/reset playback speed — only active when stdin is a TTY. Playback
  speed scales `samples_per_frame` (real-time cadence between Z80 interrupt frames), not the Z80/AY clocks, so
  tempo changes without affecting pitch.
- **`midi-ay/`** — `MIDIAY`'s interactive shell (`midiay.c`) plus `effects/` (reverb, echo), `notes/` (chords,
  arpeggiator) and `oscillators/` (polyBLEP) helpers, all macOS-only.

### Timing rules

Player timing is Spectrum-like and mostly hard-coded per machine profile in `slop-ay/slopay.c`: Z80 3,494,400 Hz,
AY 1,773,450 Hz, 50 Hz frame interrupt cadence by default (Amstrad CPC support allows a configurable interrupt
rate), 44.1 kHz audio by default. Infinite songs have `song_length == 0`; WAV/MIDI export requires a finite
duration (explicit `-t` or a nonzero song length).

## Code conventions

- Declare variables separately from use, grouped at the top of scope, ordered by use (also a global preference —
  see user-level `CLAUDE.md`).
- Favour explicit, local parsing helpers (e.g. `read_be16`, `read_be16_signed`, `rel_ptr` in
  `slop-ay/slopay-loader.c`) over hidden endian/pointer magic.
- Port-decoding logic is mask-based (`slopay_is_register_port`, `slopay_is_data_port`) and should stay
  centralized rather than duplicated.
- This project favours simple structs over deep abstraction — preserve directness unless a change genuinely
  crosses module boundaries.

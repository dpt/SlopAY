# AGENTS.md

## Project Snapshot
- `SlopAY` is a C codebase for loading `.ay` music files, emulating a Z80 player, and rendering AY-3-8912 audio either in real time (macOS Core Audio) or to WAV.
- Main runtime flow lives in `slop-ay/slopay.c`; reusable cross-target pieces are in `lib/`.
- `ProjectAY/` is the local corpus of demo/game `.ay` files for manual regression checks.

## Architecture You Need First
- **Loader layer:** `slop-ay/slopay_loader.c` parses AY files (big-endian fields + signed relative offsets) into `slopay_loader_file_t` and `slopay_loader_song_t` (`lib/slopay_loader.h`).
- **Playback core:** `slop-ay/slopay.c` builds Player v3 memory (`slopay_build_player_v3_memory`), loads song blocks (`slopay_load_song_blocks`), seeds Z80 regs, then drives execution.
- **CPU/audio bridge:** Z80 port callbacks in `slop-ay/slopay.c` map OUT traffic to AY register select/data writes and beeper state; this is the key cross-component contract.
- **Sound generation:** `lib/slopay_chip.c` exposes register-driven synthesis (`slopay_chip_write_register`, `slopay_chip_get_sample`) with fixed-point timing and stereo mixing.
- **Output drivers:** `lib/slopay_target_macos.c` handles real-time Core Audio callback output; `slop-ay/slopay_target_wave.c` reuses the same render callback for offline WAV encoding.

## Critical Data/Timing Rules
- AY format parsing follows `ay-format.md`: Motorola order (big-endian), 16-bit signed relative pointers, block terminator at `address == 0`.
- Player timing is Spectrum-like and hard-coded in `slop-ay/slopay.c`: Z80 3,494,400 Hz, AY 1,773,450 Hz, 50 Hz frame interrupt cadence, 44.1 kHz audio.
- Infinite songs are represented as `song_length == 0`; WAV export requires a finite duration (`-t` or nonzero length).

## Build + Run Reality (Current Repo State)
- `CMakeLists.txt` defines two targets on macOS: `SlopAY` (main player CLI) and `MIDIAY` (interactive MIDI shell).
- `SlopAY` uses loader + Z80 + AY chip + WAV target modules and adds the macOS target module only when `APPLE` is true.
- `MIDIAY` is macOS-only in CMake and links `CoreMIDI` plus audio frameworks with `polyblep`/`reverb` helpers.

## Developer Workflows Used In This Codebase
- Use `slop-ay/slopay.c` CLI flags for diagnostics and reproducibility:
  - `-p` prints per-frame note/pitch view (good for timing/debug)
  - `-w out.wav` uses deterministic offline render path
  - `-t <sec>` bounds runtime (required for unknown-length songs in WAV mode)
- Regression checks are usually manual against files under `ProjectAY/*/Demos` and `ProjectAY/*/Games`.
- Z80 opcode coverage gaps are surfaced at runtime via `slopay_z80_missing_opcode_snapshot` and printed summaries in `slopay_dump_missing_opcodes`.

## Code Patterns and Conventions
- Keep parsing helpers explicit and local (`read_be16`, `read_be16_signed`, `rel_ptr` in `slop-ay/slopay_loader.c`); avoid hidden endian/pointer magic.
- New audio outputs should follow existing driver pattern: callback signature compatible with `render_audio` and lifecycle `init/start/stop/cleanup`.
- Port-decoding logic is mask-based (`slopay_is_register_port`, `slopay_is_data_port`) and should stay centralized to avoid behavioral drift.
- This project favors C99 + simple structs over deep abstraction; preserve directness unless a change truly crosses module boundaries.

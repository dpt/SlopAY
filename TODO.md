# TODO

## High priority

- [ ] **Verify and tune unipolar + DC-block output path (`lib/slopay-chip.c`)**
  - Confirm sampled speech remains clear across known problem tracks (for example `ChaseHQ.ay` tracks 6+).
  - A/B compare tonal balance against previous output to check for unintended bass loss or level shift.
  - Review whether `AY_DC_BLOCK_R` should stay fixed at `0.995f` or be made configurable.

- [ ] **Reconcile current AY chip core after rapid iteration (`lib/slopay-chip.c`)**
  - Re-check that intended fixes are all present together (register masking, noise bit semantics, noise clocking, unipolar output, DC filter).
  - Remove or update stale comments that no longer match behaviour.

- [ ] **Decide final analogue model strategy**
  - Either keep the current simple DC-block filter, or add an explicit optional output stage model.
  - Document the chosen approach in `README.md` and `AGENTS.md`.

## Medium priority

- [ ] **Address strict warning backlog (`-Wall -Wextra -pedantic`)**
  - Fix reported `unused parameter` warnings in `slop-ay/slopay-z80.c`.
  - Work through additional warnings surfaced by stricter flags and keep builds warning-clean.

- [ ] **Add coverage checks for sequential export naming (`slop-ay/slopay.c`)**
  - Confirm per-song naming for WAV and MIDI in sequential mode: `<name>-sN.ext`.
  - Verify no accidental overwrite when running without `-s`.
  - Verify explicit `-s` mode still writes a single file exactly as requested.

- [ ] **README consistency pass after recent behaviour changes (`README.md`)**
  - Confirm options and examples match current CLI output exactly (`-V`, sequential playback default, per-song export naming).
  - Keep wording in British English.

## Lower priority

- [ ] **Version parity for `MIDIAY`**
  - Add `--version` output to `midi-ay/midiay.c` for parity with `SlopAY`.

- [ ] **Optional quality-of-life flags**
  - Consider `--no-autonext` to restore single-song default behaviour when needed.
  - Consider optional `--trace-outs` / debug gating instead of code-level instrumentation edits.

## Validation checklist for release candidate

- [ ] Build both targets in Debug and Release configurations.
- [ ] Manual listening pass on representative Spectrum and CPC files (AY-only, beeper-only, hybrid, sampled speech).
- [ ] WAV export sanity check (single-song and sequential multi-song).
- [ ] MIDI export sanity check (single-song and sequential multi-song).
- [ ] Confirm no regressions in piano roll output formatting.

## Future Features

- [ ] Atari ST/YAMAHA YM2149 support.
  - This page talks about the differences from the YM https://maidavale.org/blog/ay-ym-differences/
- [ ] BBC Micro/Texas Instruments SN76489 support.


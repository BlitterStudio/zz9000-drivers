# ZZCapture

`ZZCapture` diagnoses native Amiga video capture on the ZZ9000. `info` reports
board and capture state. `observe` records four frozen row-timing records
without changing phase control. The C28-only commands test or adjust the
28 MHz sampling phase against a generated AGA test image.

Use `observe` when investigating row movement, alternating horizontal origins,
wrong pixels, or capture-clock instability. Use the phase commands only with
the matched A4000 C28 diagnostic firmware and bitstream. A successful command
reports the state of that machine and source mode; it does not qualify other
machines or HDMI output.

## Install and collect an initial report

Install `ZZCapture`, `BOOT.bin`, and the bitstream from one matching package,
then cold-boot before running the tool. A C28 firmware mismatch is a reason to
stop and check the complete package. Do not bypass the capability check.

For an initial report that does not change phase control, save both commands:

```text
ZZCapture info >RAM:zzcapture-info.txt
ZZCapture observe >RAM:zzcapture-observe.txt
```

Include the complete files, the firmware and bitstream variant, and a
description of the visible fault when reporting a problem. `info` reads board,
firmware, clock, phase, and last-snapshot state. It does not write registers.
`observe` arms one new snapshot, reads frozen row metadata, and does not open a
test screen, score pixels, or commit a phase. It writes only the snapshot arm
and address registers needed to acquire that metadata.

## Commands

Run `Stack 32768` in the same Shell before `phase`, `check`, `startup`, or
`calibrate`. Those commands reject a process stack below 32 KiB before screen
setup or a phase-register write. `info` and `observe` do not need the larger
stack.

```text
Stack 32768
ZZCapture info
ZZCapture observe
ZZCapture phase -140
ZZCapture check pal
ZZCapture check pal lace
ZZCapture startup ntsc
ZZCapture startup ntsc reverse
ZZCapture calibrate pal
ZZCapture calibrate ntsc
ZZCapture calibrate pal lace
ZZCapture calibrate ntsc lace
```

| Command | Effect | Requirements and exit status |
| --- | --- | --- |
| `info` | Prints the board address and Zorro version, firmware, capabilities, clock flags and counts, phase flags, and the last frozen snapshot status. | A ZZ9000 board. Returns 0 after the report. |
| `observe` | Takes one metadata-only snapshot and prints four `Timing observe` records. | Capture capability `0x56510106` (E7M) or `0x56510206` (C28), metadata capability `0x564d010c`, and field-diagnostic capability `0x56440110`. Returns 20 when the matched observation interface or a fresh frame is unavailable. |
| `phase N` | Applies phase `N` and waits for acknowledgement. Nothing is saved. | C28 diagnostic firmware, Zorro III, a ready idle controller, and a qualified 28 MHz clock. `N` is an integer from -896 through 895. Returns 0 on acknowledgement, 10 for syntax, or 20 for a hardware failure. |
| `check pal\|ntsc [lace]` | Tests the current phase on the native pattern. It restores and verifies the entry phase before closing the screen. Nothing is saved. | The C28 requirements above and the selected native monitor. Returns 0 only when the phase has no wrong or changing pixels and restoration succeeds; 10 is syntax and 20 is a failed or incomplete check. |
| `startup pal\|ntsc [reverse]` | Measures the entry phase and nearby candidates for three minutes on one progressive test screen, then restores the entry phase. Nothing is saved. | The C28 requirements above. `reverse` visits the plus candidate first. Returns 0 when the timed experiment completes and restoration succeeds, even if rows report pixel errors; 10 is syntax and 20 is an incomplete run, timing fault, or restoration failure. |
| `calibrate pal\|ntsc [lace]` | Sweeps the C28 phase circle, refines a clean interval, retests its midpoint, and leaves that midpoint live on success. It prints a `videocap_c28_phase` candidate but never writes `ZZ9000.CFG`. | The C28 requirements above and the selected native monitor. Returns 0 on a verified interval, 10 for syntax, or 20 for a calibration or hardware failure. |

`phase`, `check`, `startup`, and `calibrate` require the A4000 C28 diagnostic
firmware and a Zorro III board. They require a capture clock with frequency,
lock, ready, and C28 flags set, with no fault, and an idle ready phase
controller. Busy capture, a capture-engine error, an acknowledgement timeout,
or a source change ends the command instead of selecting phase zero.

## Native-screen requirements

`check`, `startup`, and `calibrate` create an 8-plane, 1280-pixel SuperHires
AGA screen with the requested PAL or NTSC standard. The display mode must have
an 8-bit red, green, and blue palette; it cannot be RTG, HAM, dual-playfield,
Extra Half-Brite, or scandoubled. Install the PAL or NTSC monitor when the
requested standard is not the native default.

The screen needs about 320 KiB of Chip RAM for progressive PAL or 640 KiB for
PAL interlace, plus screen and window overhead. It uses the A4000 28 MHz
video-slot connection and AmigaOS 3 graphics and Intuition libraries. The tool
refuses an unavailable mode, an unsuitable replacement screen, and a screen
with less than eight bitplanes.

The test screen remains in front during measurement. Do not switch screens,
move it behind another screen, change the video setup, or run another capture
or configuration tool. Press Escape or Ctrl-C to cancel. On cancellation,
capture or clock failure, the tool attempts a bounded, acknowledged restoration
of the entry phase. If restoration fails, the report marks the phase as unknown;
cold-boot before continuing.

## Observation output

Each `Timing observe` record is paired with one of the four captured rows. It
prints three raw metadata words and these decoded values:

| Group | Fields |
| --- | --- |
| Line identity | `raw_y`, `history`, `grid_seen`, `pair`, `grid`, `hsync_low`, `shortlines` |
| Edge timing | `timestamp`, `interval` |
| Sampler state | `sample_x`, `phase_x`, `grid_pair_first`, `full_width`, `sample_mode`, `fullrate`, `csync_vsync`, `rgb_mode` |

`raw_y` identifies the captured source row. `timestamp` and `interval` are
capture-clock values for accepted HSYNC edges. `sample_x` and `phase_x` are
counter values just before the next HSYNC resets the row origin. The field
diagnostic capability, build, and variant establish the loaded bitstream
identity. `clock` and `counts` are separate reads rather than an atomic trace;
in `counts`, the low 16 bits are C28 edges per millisecond and the high 16 bits
are E7M edges per millisecond.

Alternating intervals or pre-reset counters beside faulty rows can indicate an
HSYNC or line-origin path. Stable timing with alternating, otherwise intact
pixel origins can indicate RGB movement relative to a stable capture origin.
A missing history bit, a `raw_y` mismatch, or metadata that changes separately
from the frozen snapshot can indicate an instrumentation or clock-domain
crossing issue. These observations do not establish the final physical cause.

## Phase and pattern checks

The C28 phase circle has 1792 fine steps, from -896 through +895. One step is
about 20 ps of the 28 MHz clock period. These units differ from legacy
`videocap_phase` units and must not be converted. The E7M phase domain is
separate: it has 448 steps and a signed range of -255 through +255.

The test pattern repeats 256 palette colours across every row. It changes all
24 RGB bits repeatedly. Each captured line must match the expected sequence at
one unknown horizontal origin and advance one SuperHires pixel per sample. A
flat picture, duplicated or omitted pixels, wrong RGB values, or lost low
colour bits fails the check. The four-line sample does not establish vertical
placement, the rest of the framebuffer, output-clock behaviour, the HDMI link,
or monitor behaviour.

Each capture contains 1,024 raw RGB samples: 256 adjacent samples on each of
four consecutive source rows. The source region is `crop_h + 128` through
`crop_h + 383` and `crop_v + 64` through `crop_v + 67`. The tool checks capture
geometry, PAL or NTSC state, interlace state, phase, and clock state while it
reads. Arm acknowledgement and the field sequence reject a stale comparison.

For a measurement position, the tool reapplies the phase, discards two
post-acknowledgement captures, then scores the remaining captures. A sweep
uses ten temporal comparisons per field parity. `check` and the final
calibration retest use 50. Interlace compares matching parities separately and
requires both to pass. Every wrong or changing pixel counts; there is no masked
colour bit, error band, or tolerated error threshold.

When the same interlace parity repeats while the other still needs comparisons,
the tool calls `WaitTOF()` before rearming. Each required field has a
no-progress limit of 66 captures for a sweep or refinement position and 306
for a longer check. Progressive measurements retain those 66 and 306 capture
limits. In interlace, the total bounds are 726 captures for a sweep or
refinement position and 15,606 for a longer check. The two discarded transition
captures are outside these limits.

`calibrate` measures 64 positions, 28 fine steps apart. It selects the widest
circular run of at least three clean positions; equal widths select the midpoint
nearest the entry phase. It binary-searches both neighbouring bad/good brackets,
then retests the refined midpoint. A phase wrap at +895/-896 is normal. If all
64 positions pass, it restores the entry phase and reports no measured boundary
instead of selecting a value. A failed pattern can also mean that the capture
crop misses the test area, the screen is not reaching the input, or another
part of the test path failed. Use Automatic framing and a known working cable
before attributing a failed sweep to phase.

## Failure evidence

`check` and `startup` retain the first scored snapshot with wrong or changing
pixels, plus the preceding scored snapshot of the same parity when available.
The reference can contain errors and its strict score is printed. Incomplete or
invalid reads are not retained as pixel evidence. `calibrate` does not retain a
packet because its sweep deliberately visits bad phases.

A packet includes the snapshot status and geometry, one `Row` and `Timing` line
per source row, a context record, and `RAW failed` data. It can also include
`RAW reference`. The raw section contains all 1,024 words in capture order,
eight hexadecimal words per line; offsets `0000` through `1016` cover four
groups of 256 words. Keep the report through `End failure evidence v2.` for
offline analysis. The packet adds about 12–25 KiB of output, so redirect it to
a file and copy it to disk before power-off:

```text
ZZCapture check ntsc >RAM:c28-check-ntsc.txt
```

`Row` reports the best horizontal pattern `origin` modulo 256, the count of
equally good `ties`, and `residual_pixels`. A unique origin also reports
`residual_bits` and `bit_mask`; ambiguous alignment prints `origin=-1` and
`unknown` bit metrics. Diagnostic alignment does not change the strict
pass/fail scores. Different row origins with zero residual errors describe
intact colour sequences at different horizontal positions. Residual errors show
that a horizontal shift alone does not explain the row.

## Startup experiment

`startup` starts its timer before screen setup and runs for 180,000 ms. It
measures the entry phase as a baseline, then alternates baseline minus 28,
baseline, baseline plus 28, baseline until the duration has elapsed. `reverse`
changes the candidate order. The candidate phases wrap at +895/-896. The run
can finish after three minutes because a started candidate is followed by a
baseline measurement before the duration check.

Each `Startup` row reports the start and end milliseconds, phase, role,
complete state, raw pixel-error counts, clock and phase records, framing, and
field coverage. The times use `timer.device` E-clock time and are not time
since power-on. Clock status, edge counts, and phase status are sampled before
and after each measurement; they are separate reads. A frozen or backwards
clock, a changed E-clock frequency, a one-hour elapsed limit, and a limit of
200 candidate/baseline pairs terminate faulty runs.

Compare cold boots with opposite candidate orders, then a warm control. Keep
the firmware, saved phase, output profile, cable, crop, and filter unchanged.
Record launch delay and visible behaviour, and save every complete log before
a new run or power-off. A candidate that improves before its following baseline
supports a phase effect. A baseline that also improves with elapsed time leaves
startup settling as a possible explanation; neither observation proves a
thermal cause or justifies saving a phase.

## Hardware qualification

1. Back up the boot image and configuration. Install the matched C28 package,
   cold-boot, and record the firmware and bitstream build, card revision,
   video adapter and cable, Amiga model, crystal standard, `videocap_phase`,
   and `videocap_c28_phase`.
2. Select full-detail scandoubler output with scanlines disabled and Automatic
   framing. Apply a staged profile by cold-boot, close ZZTop and other
diagnostic tools, and keep the cable, crop, filter, and output profile fixed.
3. Start from ambient temperature. Record elapsed time since power-on, then
   collect `info` and the applicable progressive and interlace `calibrate`
   reports. Use `ntsc` for an NTSC run. The native screen still opens when
   stdout is redirected. Apply the same recorded entry phase with
   `ZZCapture phase N` before directly comparing repeated runs.
4. After at least 30 minutes of normal operation, record ambient temperature
   and elapsed time and repeat the measurements to separate warm logs. Test
   PAL-crystal and NTSC-crystal machines independently when both are affected.
5. Compare clean intervals and margins. Choose a value only when a common
   phase lies comfortably inside every required cold, warm, and source-mode
   interval. Keep reports for investigation when no common interval exists or
   a longer check fails.
6. At the candidate, inspect native lores, hires, and SuperHires in progressive
   and interlace. Include fine checkerboards, one-pixel text, saturated RGB
   transitions, and static screens. Check full-detail output for shimmer,
   wrong colours, pixel loss, row movement, alternating-field defects, and
   vertical placement. Check filtered output separately for pairing and detail
   handling.
7. Save only a qualified C28 value through the matching stack:

   ```text
   videocap_c28_phase = N
   ```

   Replace `N` with the verified phase. Do not copy a legacy phase value into
   this key. Power-cycle, run `ZZCapture info` to check readback, repeat the
   relevant image checks, and retain the value and reports with that machine.
   Recalibrate after a change to its capture-clock design, card, adapter, or
   video cable.

## Protocol allocation and compatibility

| Item | Value or rule |
| --- | --- |
| Capture capability register | `0x1240` |
| E7M observation capability | `0x56510106` |
| C28 observation and phase-control capability | `0x56510206` |
| Row metadata capability register and value | `0x1274`, `0x564d010c` |
| Field-diagnostic registers | `0x1200` capability, `0x1204` build, `0x1208` variant; capability `0x56440110` |
| Snapshot registers | status `0x1260`, arm `0x1264` with token `0xca1c`, pixel address `0x126a`, pixel data `0x126c`, geometry `0x1270` |
| Metadata registers | address `0x1278`, data `0x127c` |
| Phase-control registers | target `0x1246`, commit `0x1248` with token `0xf05a`, status `0x124c` |
| Clock registers | status `0x1250`, edge counts `0x1254` |

`observe` accepts the matched E7M or C28 capture protocol and needs the
metadata and field-diagnostic interfaces. The field-diagnostic ABI is also
present on Denise variants. C28 phase writes are exclusive to the C28 protocol.
Do not convert E7M and C28 phase values.

## Building and host checks

Build with the repository wrapper. It uses a local `m68k-amigaos-gcc` when
available, otherwise `tools/amiga-docker.sh`:

```sh
cd ZZCapture
AMIGA_IMAGE=sacredbanana/amiga-compiler:m68k-amigaos ./build.sh
```

The repository host suite includes the C99 calibration checks and the
`ZZCAPTURE_TEST_IO` lifecycle test:

```sh
make -C common/tests test
```

The calibration helper can also be compiled alone on a native C99 compiler:

```sh
cc -std=c99 -Wall -Wextra -Werror -Iinclude \
   tests/capture_calibration_test.c -o /tmp/capture_calibration_test
/tmp/capture_calibration_test
```

Host tests cover the phase and interval-selection helpers, the shared capture
client, and a mocked lifecycle. They do not replace physical capture, AmigaOS
display, or startup testing.

## Status and history

The current program identifies itself as version `0.9` dated 23 September
2026. `observe` is the non-phase-changing diagnostic path. `phase`, `check`,
`startup`, and `calibrate` remain A4000 C28 diagnostics and require hardware
qualification before a configuration value is saved.

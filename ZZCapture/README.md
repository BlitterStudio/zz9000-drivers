# ZZCapture

`ZZCapture` measures native-video sampling against a controlled AGA test
image. It is an **experimental A4000 diagnostic tool**, requiring the matched
opt-in C28 firmware/bitstream with capability `0x56510206`. It does not support
the legacy E7M phase interface, A3000/A2000, or Denise adapters. A successful
run is measurement evidence for that machine and source mode; it is not
qualification of other machines or a guarantee of artifact-free HDMI output.

The Amiga must have AmigaOS 3, AGA, the 28 MHz video-slot connection to ZZ9000,
and enough free Chip RAM for an eight-plane native screen (about 320 KiB for
PAL progressive, 640 KiB for PAL interlace, plus screen/window overhead).
Install the PAL or NTSC monitor when testing a standard different from the
native default. The tool checks the requested mode and its full 24-bit
palette, and refuses an RTG replacement or a lower-depth screen.

## Commands

Run `Stack 32768` in the same Shell before `phase`, `check`, `startup` or `calibrate`. These
commands check the process stack and refuse to open the test screen or
write registers if it is smaller than 32 KiB. `info` remains read-only and
does not require the larger stack.

```text
Stack 32768
ZZCapture info
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

`info` reads board, firmware, clock and phase state without writing registers.
`phase N` applies a temporary phase from -896 through +895 and waits for exact
acknowledgement. These units describe the C28 clock: 1792 fine steps span a
complete pixel period, roughly 20 ps per step. They are different from the
legacy `videocap_phase` units.

`check pal|ntsc [lace]` tests the current phase with the same native pattern
and 50 comparisons per field parity, without a sweep or a new phase choice.
It restores and checks the entry phase before closing the screen. Use it to
separate field-collection problems from the deliberately bad phases visited
by calibration, or to repeat a suspicious position with `phase N` then `check`.
It never saves settings. Version 0.5 works with the original C28 BOOT images;
the startup diagnostic requires no firmware change.

`startup pal|ntsc [reverse]` measures startup behavior on one continuous
progressive screen for three minutes. The baseline is the phase read on entry.
The sequence is baseline, baseline minus 28, baseline, baseline plus 28,
baseline, repeated until the duration has elapsed. `reverse` visits the plus
candidate first. Phases wrap at +895/-896. These are nearby experimental
positions, not recommended settings; for entry -58 they are -86 and -30.

Every row reapplies its phase, discards two captures, then scores 51 snapshots
and 50 temporal comparisons. Wrong or changing pixels are recorded without
stopping the experiment. Capture, clock, mode, framing, timer or cancellation
failures stop it. A started candidate is followed by a baseline check before
the duration is tested, so the run can finish slightly after three minutes.
The entry phase is restored and acknowledged before the screen closes on
both completion and failure. Nothing is selected or saved.

Rows include elapsed start/end milliseconds, phase and role, raw pixel-error
counts, complete/incomplete status, framing, and field coverage. Clock status,
edge counts and phase status are sampled before and after each measurement;
these separate register reads are not an atomic trace. In `counts`, the low
16 bits are C28 edges and the high 16 bits are E7M edges per 1 ms. Elapsed time
uses `timer.device`'s E-clock, starting before screen setup. It is **not time
since power-on**; record the launch delay separately. These checks do not
observe every video frame, and phase reapplication is part of the experiment.

For startup only, exit status 0 means the timed experiment completed and
restored the entry phase, even if rows contain pixel errors. It does not mean
all phases passed. Syntax errors return 10; interrupted or incomplete runs,
unusable timing, or restoration failures return 20. The timer and all screen
resources are released on failure. A frozen/backwards clock, changed E-clock
frequency, one-hour elapsed bound, and hard limit of 200 candidate/baseline
pairs keep faulty timing from extending the run indefinitely.

Compare cold boots with opposite candidate orders, then a warm control. Keep
the firmware, saved phase, output profile, cable and crop unchanged. Start as
soon after boot as practical; do not run calibration first. Save each complete
log before another test or power-off, and record time since power-on and
visible behavior. If a candidate improves before the following baseline,
that supports a phase effect; if the baseline also improves with elapsed time,
startup settling remains possible. Neither result alone establishes a thermal
cause or justifies persisting a phase.

`calibrate` opens its own native 1280-pixel SuperHires, 256-color screen. PAL
or NTSC is explicit, including when launched from an RTG Workbench. It keeps
the source image still and hides the mouse pointer. Do not switch screens,
move the test screen, or run another capture/configuration tool during the
measurement. Press Escape or Ctrl-C to cancel. The tool restores the entry
phase on cancellation, capture/clock errors, or failure to find a verified
clean interval. Restoration is bounded and checked; if it fails, the report
explicitly says the phase is unknown and a cold boot is required.

On success the chosen phase remains live. **The tool never writes
`ZZ9000.CFG`.** It prints the candidate `videocap_c28_phase` setting, the
original phase restoration command, the clean interval, and its tested
margin. Exit status is 0 on success, 10 for command syntax, and 20 for a
hardware/calibration failure (see the startup-specific completion meaning above).
Save the complete report, not just its chosen
phase.

## What is measured

The FPGA supplies a frozen snapshot of 1024 raw RGB samples before any
filtering: 256 adjacent capture pixels on each of four consecutive source
lines. Its source coordinates are `crop_h + 128` through `crop_h + 383`, and
`crop_v + 64` through `crop_v + 67`. The tool checks the geometry, PAL/NTSC
state, interlace state, phase and clock throughout each read. The snapshot
arm acknowledgement and field sequence prevent comparing a stale frame
with itself.

The screen repeats a 256-color pattern horizontally. All 24 RGB bits change
repeatedly across its period. A snapshot must contain the exact expected
color sequence at one unknown horizontal origin; each sample must advance
one SuperHires pixel. A flat frame, duplicated/omitted pixels, wrong RGB
values, or lost low color bits fails even if the image is perfectly still.
Success therefore cannot come from simply smoothing or holding bad pixels.
All four inspected lines carry the same horizontal pattern; this test does
not establish vertical row placement or exclude stable duplicated rows.

For each of 64 phases spaced 28 fine steps apart, the tool waits for the
applied phase, discards two complete captures, then collects at least ten
temporal comparisons per field parity. In interlace it compares matching
parities separately and requires both to pass. Every wrong or changing pixel
counts; there is no tolerated bad band, error threshold or masked color bit.

The host can repeatedly skip an even number of fields while polling and
reading snapshots. The tool waits for the next vertical blank with
`WaitTOF()` before rearming when a parity repeats and the other still needs
comparisons. The previous DOS-duration wait could round to two fields and
leave the same parity starvation in place. Version 0.4 permits more snapshots
when field delivery is unbalanced: up to 132 per sweep/refinement position
or 612 for the longer check. It also stops if a field still needing comparisons
has not appeared for 66 or 306 snapshots, respectively. A completed field
cannot keep extending that wait. Progressive limits stay at 66/306 snapshots.
These limits exclude the two discarded transition captures.
Every captured sample remains scored, including samples from a field whose
comparison count is already sufficient. Both parities must reach the full
required count; a missing/stuck parity or excessively sparse delivery still
fails within the bounds. The final or
failed measurement reports per-parity sample/comparison counts, field sequence
range and increments, and cadence waits. These distinguish a skipped-field
cadence from a parity signal that does not alternate on odd sequence steps;
they do not themselves establish which physical field is odd or even.

It selects the widest circular run of at least three clean positions.
Equal-width runs prefer the midpoint nearest the entry phase. It then
refines both boundaries by binary search within the neighboring 28-step
bad/good brackets, using the same ten comparisons per parity for every
tested position. Each refined clean endpoint is one fine step from a tested
bad position. The final midpoint is calculated from those refined endpoints
and retested with at least 50 comparisons per parity. A wrap from +895 to
-896 is normal. The reported margin extends only to the tested clean
endpoints. Boundary refinement assumes the selected local quiet interval
is contiguous; repeated cold/warm runs remain necessary to measure its
repeatability. If all 64 positions pass, it reports that no boundary was
measured and restores the original phase, because that result needs
investigation rather than automatic saving.

A failure to match the pattern can also mean the capture crop misses the
test area, the screen is not reaching the video input, or the test path has
an unrelated defect. Use the normal Automatic framing and a known working
video cable before attributing a failed sweep solely to clock phase.

## Reproducible hardware qualification

1. Back up the current boot image and configuration. Install the matched
   experimental C28 package using the documented firmware update procedure,
   then power-cycle. Record the exact firmware/bitstream build, ZZ9000 card
   revision, video adapter/cable, Amiga model and crystal standard. Record
   the existing `videocap_phase` and `videocap_c28_phase` separately.
2. Select full-detail scandoubler output for visual checks, with scanlines
   disabled and Automatic framing. Apply any staged profile by cold boot
   before measuring. Close ZZTop and other diagnostic tools. Do not change
   the cable, crop, filter or output profile between compared runs.
3. Start with the machine cooled to ambient temperature. Power it on and
   record elapsed time since power-on for each run. Save initial telemetry:

   ```text
   Stack 32768
   ZZCapture info >RAM:c28-cold-info.txt
   ZZCapture calibrate pal >RAM:c28-cold-pal.txt
   ZZCapture calibrate pal lace >RAM:c28-cold-pal-lace.txt
   ```

   Use `ntsc` for an NTSC run. The native test screen still appears when
   stdout is redirected. Test PAL and NTSC separately when both are used.
   Each sweep changes the live phase, so the next report records the entry
   phase left by the previous run. For directly comparable repeated runs,
   apply the same recorded entry phase first with `ZZCapture phase N`.
4. Leave the computer operating normally for at least 30 minutes. Record
   ambient temperature and elapsed time, then repeat the same commands to
   warm-named log files. Repeat cold/warm tests on affected PAL-crystal and
   NTSC-crystal machines: changing a screen standard does not replace
   testing the other physical clock crystal.
5. Compare clean intervals and their margins across runs. A common phase
   must lie comfortably inside every required cold/warm and source-mode
   interval. If there is no common interval, or a run fails the longer
   check, do not choose the lowest-error phase or average failing results;
   keep the reports for investigation.
6. At a common candidate, inspect native lores, hires and SuperHires in
   progressive and interlace. Include fine checkerboards, one-pixel text,
   saturated RGB transitions and static screens. Check full-detail output
   for shimmer, wrong colors, pixel loss, row movement, alternating-field
   defects and vertical placement. Check filtered output separately for
   correct pairing and unchanged detail handling. A four-line raw snapshot
   cannot establish correctness of the remaining framebuffer, the output
   clock, HDMI link or monitor.
7. Only after qualification, persist the common value using the matched
   stack's configuration support:

   ```text
   videocap_c28_phase = N
   ```

   Replace `N` with the verified value; never copy a legacy phase value into
   this key. Power-cycle and run `ZZCapture info` to verify readback, then
   repeat the relevant image checks. Keep the qualified value and reports
   with that physical machine. Recalibrate after changes to its capture
   clock design, card, adapter or video cable.

## Building and host checks

The component follows the repository build wrapper and defaults to the
user's AmigaOS 3 image:

```sh
cd ZZCapture
AMIGA_IMAGE=sacredbanana/amiga-compiler:m68k-amigaos ./build.sh
```

Run both the helper and actual utility lifecycle checks with the existing
host suite:

```sh
make -C common/tests test
```

For the pure measurement and selection checks alone on a native C99 compiler:

```sh
cc -std=c99 -Wall -Wextra -Werror -Iinclude \
   tests/capture_calibration_test.c -o /tmp/capture_calibration_test
/tmp/capture_calibration_test
```

The tests exercise signed 16-bit target words across all 1792 phases,
12-bit status decoding, circular/wrapping/tied/narrow
and full-circle intervals, boundary refinement through the signed wrap,
all horizontal origins, flat/wrong/repeated/
skipped/shifted pixels, and transitions on every RGB bit. The lifecycle
harness compiles `ZZCapture.c` with `ZZCAPTURE_TEST_IO` and mocks only the
registers and AmigaOS calls. It exercises the actual polling, native bitmap
and palette construction, complete PAL and NTSC interlace sweeps, boundary
refinement, cancellation, timeout, partial screen setup, resource cleanup,
launch-stack guard, checked restoration and success reports. It does not
substitute for physical capture or real AmigaOS display qualification.
The cadence regressions drive fields independently of requests, with separate
PAL/NTSC field periods, a fixed 50 Hz minimum-duration DOS wait and varied
snapshot read costs. They retain version 0.2's two/four-field polling case.
A synthetic delivery sequence reproduces the 0.3 report's 56/10 snapshots,
55/9 comparisons and 2..4 sequence steps at its 66-snapshot cutoff, then
verifies completion with the extended budget. That report does not include
the exact field trace or read latencies, so the fixture establishes the
collection-accounting failure, not its physical timing cause.
Tests cover sequence wrap, stuck/stalled fields, the hard total limit despite
occasional progress, complete calibration, and current-phase checks. Late
pixel errors, Ctrl-C/Escape and restoration failures are tested beyond the
old limit. Hardware retesting on the affected machine remains necessary.
Startup regressions exercise both orders and standards, signed phase wrap,
fixed screen lifetime, complete comparison counts, early pixel errors followed
by recovery, and continued collection at bad candidates. They also cover
E-clock low-word wrap, frozen/backwards/changed/slow/jumped timing, partial
timer/screen setup, cancellation during a candidate, capture/framing/clock
faults, checked restoration and launch guards. Real AmigaOS timer/display
operation and startup behavior still require hardware testing.

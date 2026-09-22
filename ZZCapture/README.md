# ZZCapture

`ZZCapture` observes native-video row timing and, on A4000/C28 builds, measures
sampling against a controlled AGA test image. The `observe` command accepts
the matched E7M (`0x56510106`) or C28 (`0x56510206`) capture protocol plus row
metadata capability `0x564d010c`; it supports Zorro III, Zorro II and Denise
adapter builds without touching phase control. Phase-changing and strict-pixel
commands remain experimental A4000 diagnostics. A successful run is evidence
for that machine and source mode; it is not qualification of other machines or
a guarantee of artifact-free HDMI output.

The Amiga must have AmigaOS 3, AGA, the 28 MHz video-slot connection to ZZ9000,
and enough free Chip RAM for an eight-plane native screen (about 320 KiB for
PAL progressive, 640 KiB for PAL interlace, plus screen/window overhead).
Install the PAL or NTSC monitor when testing a standard different from the
native default. The tool checks the requested mode and its full 24-bit
palette, and refuses an RTG replacement or a lower-depth screen.

## Commands

Run `Stack 32768` in the same Shell before `phase`, `check`, `startup` or `calibrate`. These
commands check the process stack and refuse to open the test screen or
write registers if it is smaller than 32 KiB. `info` and `observe` do not
require the larger stack.

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

`info` reads board, firmware, clock and phase state without writing registers.
`observe` arms one fresh snapshot and prints its four atomically frozen timing
rows, build/variant identity, geometry and clock context. It does not open a
test screen, score pixels, require C28 readiness, or write the C28 phase target
or commit registers. Snapshot arm/address writes are required to acquire and
read the metadata. Redirect the complete output to a file when comparing E7M
behavior across A3000, A2000 and supported Denise-adapter machines.

`phase N` applies a temporary phase from -896 through +895 and waits for exact
acknowledgement. These units describe the C28 clock: 1792 fine steps span a
complete pixel period, roughly 20 ps per step. They are different from the
legacy `videocap_phase` units.

`check pal|ntsc [lace]` tests the current phase with the same native pattern
and 50 comparisons per field parity, without a sweep or a new phase choice.
It restores and checks the entry phase before closing the screen. Use it to
separate field-collection problems from the deliberately bad phases visited
by calibration, or to repeat a suspicious position with `phase N` then `check`.
It never saves settings. Version 0.9 requires the matched diagnostic BOOT
image; it refuses older C28 images before writing any calibration register.

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

### First-failure evidence (0.8)

`check` and `startup` retain the first fully read, scored snapshot with wrong
or changing pixels. If a previous scored snapshot of the same field parity
exists at that measurement's phase, it is retained too. This reference may
also contain errors; its pattern score is printed explicitly. A failure on
the first snapshot has no temporal reference. Incomplete or invalid reads
are not retained as pixel evidence. `calibrate` does not emit this packet:
its sweep deliberately visits bad phases.

The tool copies at most two snapshots, including their row timing, into about
8 KiB of static memory. It makes no additional captures. At the first scored
failure it reads one bounded context record: metadata capability, atomic field
diagnostic capability/build/variant IDs, clock status/counts and phase status.
There are no report-time hardware reads. Analysis and raw output occur after
measurement, phase restoration attempt, screen cleanup and timer cleanup,
including on cancellation or restoration failure. Only one packet is retained,
even if later phases also fail. Startup `measurement=N`
identifies the existing `Startup row=N` and its timestamp/clock records;
`sample` is the scored snapshot number within that measurement, excluding
the two discarded captures. Snapshot status contains sequence and field
metadata; geometry contains the captured crop. Each row has a wrapping 16-bit
capture-clock timestamp, but there is no per-snapshot wall-clock timestamp.
The retained context record is sampled immediately after the failure and is
not atomically frozen with the pixel snapshot.

Each `Row` line reports the best horizontal pattern origin modulo 256,
the number of equally good origins (`ties`), and remaining mismatched
pixels. When the origin is unique, it also reports the total differing
bits and their OR mask against that pattern. If several origins tie,
`origin=-1` and bit metrics are `unknown`; a flat or severely damaged row
must not acquire a fabricated alignment. Origins are inferred from all
exact palette matches, so a damaged first pixel cannot dictate the result.

Different row origins with zero residual errors describe intact colour
sequences at different horizontal positions. Residual errors indicate that
a horizontal shift alone does not explain the row. Neither result proves
a physical cause such as HSYNC jitter or RGB setup/hold failure. The strict
four-row pattern and temporal scores remain unchanged: diagnostic alignment
never turns a failed check into a pass. In particular, damage to the first
pixel can make the strict comparator report 1024 wrong pixels, whereas
the diagnostic may identify only one damaged pixel.

Each `Timing` line is frozen with the corresponding 256-pixel row and prints
all three raw metadata words plus decoded fields. `raw_y` ties timing to the
captured source row. `timestamp` and `interval` report accepted HSYNC edges in
capture-clock units. `sample_x` and `phase_x` are the counters immediately
before that edge reset the next row's origin. `hsync_low`, `shortlines`,
`grid`, `pair`, `grid_pair_first` and the active sampler configuration provide
the remaining line-decoder and pixel-grid state.

For the observed alternating-origin failure, alternating line intervals or
pre-reset counters alongside the bad rows support an HSYNC/line-origin path.
Stable intervals and counters with alternating intact pixel origins instead
support coherent RGB movement relative to a stable capture origin. A missing
history bit, `raw_y` mismatch or metadata that changes independently of the
frozen snapshot indicates an instrumentation/CDC problem. These are causal
discriminators, not by themselves proof of the final physical mechanism.

`RAW failed` and optional `RAW reference` lines preserve every 32-bit word
in capture order, eight hexadecimal words per line. Offsets run from 0000
to 1016; four consecutive groups of 256 words are the four source rows.
Keep the entire log through `End failure evidence v2.` for offline analysis.
The packet adds about 12-25 KiB of output after the test. Redirect output to
a file, for example `ZZCapture check ntsc >RAM:c28-check-ntsc-v08.txt`, and
copy it to disk before powering off. A clean run prints `Failure evidence:
none`; a hardware abort without a scored pixel failure may also do so.

### Pattern and phase measurements

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
leave the same parity starvation in place. Version 0.7 lets a progressing
minority field reach its quota even when it supplies fewer than one in twelve
snapshots. Each needed field retains its no-progress limit of 66 captures
for a sweep/refinement position, or 306 for a longer check. It must arrive
within that window or collection stops; a completed field cannot extend
the other field's deadline.

The total interlace bound is the no-progress limit multiplied by the required
samples per field (comparisons plus one): 726 captures for a sweep/refinement
position and 15,606 for a longer check. These bounds allow every required
sample to arrive at the last permitted capture without failing early.
Balanced capture finishes as soon as both quotas are met. Severe but continuing
imbalance can make a test take much longer; Escape or Ctrl-C cancels and
restores the entry phase. Progressive limits stay at 66/306 snapshots.
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
Observe regressions cover E7M and C28 identities, Zorro II and III hosts,
metadata-only reads without the large-stack gate or phase commits, mismatched
protocol rejection, and a bounded no-frame timeout.
The cadence regressions drive fields independently of requests, with separate
PAL/NTSC field periods, a fixed 50 Hz minimum-duration DOS wait and varied
snapshot read costs. They retain version 0.2's two/four-field polling case.
A synthetic delivery sequence reproduces the 0.3 report's 56/10 snapshots,
55/9 comparisons and 2..4 sequence steps at its 66-snapshot cutoff, then
verifies completion with the extended budget. That report does not include
the exact field trace or read latencies, so the fixture establishes the
collection-accounting failure, not its physical timing cause.
Tests cover sequence wrap, stuck/stalled fields, finite completion with rare
progress, complete calibration, and current-phase checks. Periodic fixtures
matching 4/62 and 6/60 initial snapshot totals exercise both parity orientations
and both quotas. A sample on the no-progress deadline completes; one arriving
after it fails. Late pixel errors, Ctrl-C/Escape and restoration failures are
tested beyond the former 132/612-capture limits. These are synthetic delivery
patterns, not recorded hardware traces; affected-machine retesting remains
necessary.
Startup regressions exercise both orders and standards, signed phase wrap,
fixed screen lifetime, complete comparison counts, early pixel errors followed
by recovery, and continued collection at bad candidates. They also cover
E-clock low-word wrap, frozen/backwards/changed/slow/jumped timing, partial
timer/screen setup, cancellation during a candidate, capture/framing/clock
faults, checked restoration and launch guards. Real AmigaOS timer/display
operation and startup behavior still require hardware testing.

First-failure tests verify exact raw-word round trips, retention through
later clean captures, same-parity references, missing references, invalid
read rejection, cancellation and failed restoration. They also verify all
12 frozen timing words per snapshot, decoded failure/reference output, and
one-time retention of build/variant/clock/phase context. Row analysis covers
all 256 origins, corruption of each of 32 word bits, equally good offsets,
flat/invalid rows and multiple residual bits. Shifted rows still fail the
strict checks. The existing startup timestamps and capture counts are
unchanged in the host model; physical copy/printing costs need an Amiga
smoke check.

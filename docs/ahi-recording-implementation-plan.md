# ZZ9000AX AHI recording implementation plan

> **IMPLEMENTED AND HARDWARE ACCEPTED; RETAINED AS HISTORY.** Firmware #57 and
> drivers #48 merged on 2026-07-14. The production `0xa204` candidate completed
> the R17 hardware gate on 2026-07-29. Remaining final installer/release smoke
> is distinct from recording implementation and production hardware
> acceptance.

This plan implements the behavior in
[`ahi-recording-spec.md`](ahi-recording-spec.md) as two coordinated changes:
one pull request in `zz9000-firmware` and one in `zz9000-drivers`.

## 1. Firmware capture producer

Files:

- `ZZ9000_proto.sdk/ZZ9000OS/src/zz_regs.h`
- `ZZ9000_proto.sdk/ZZ9000OS/src/ax.h`
- `ZZ9000_proto.sdk/ZZ9000OS/src/ax.c`
- `ZZ9000_proto.sdk/ZZ9000OS/src/main.c`
- new pure capture-conversion source/header and host unit test under
  `test/audio/`
- `docs/ahi-recording-protocol.md`

Work:

1. Name register `0xF6` as the receive-status register and add the control and
   status masks.
2. Split playback and recording interrupt enables while retaining the legacy
   playback ownership query used by SDK audio playback.
3. Enable receive IOC interrupts and initialize receive publication state each
   time the formatter is reconfigured.
4. On receive completion, invalidate the DMA period, convert/resample it in
   place, flush the published bytes, update status, and conditionally signal
   the Amiga.
5. Expose the status through the register-read path and accept recording bit 1
   through the existing audio-config write path.
6. Unit-test conversion at 48, 44.1 and 8 kHz plus sequence packing/wrap using
   the host compiler.

Validation:

- `make -C test/audio test`
- the existing firmware host-test suites
- `./build_firmware.sh clean && ./build_firmware.sh`
- `./build_bootimage.sh` using the committed FPGA bitstream
- verify no `.v`, `.tcl` or `.bit` file changes

## 2. AHI capture consumer

Files:

- `include/zz9000_hw.h`
- `include/zz9000_ax.h`
- `ahi/driver/zz9000ax-ahi.h`
- `ahi/driver/zz9000ax-ahi.c`
- `ahi/README.md`
- `tools/tests/test_repo_tooling.py`

Work:

1. Add the shared register, control-mask and receive-status definitions.
2. Capability-gate AHI recording using codec presence plus the new firmware
   status bit.
3. Allocate a persistent recording callback buffer and configure the receive
   ring after the transmit ring in reserved card memory.
4. Track play and record states independently and write their combined mask to
   the firmware.
5. Gate playback service on the firmware transmit sequence, then drain all new
   receive sequences in chronological order. This keeps capture-only shared
   interrupts from advancing the playback ring.
6. Copy each published period into the persistent buffer and invoke
   `ahiac_SamplerFunc` with an `AHIRecordMessage` describing `AHIST_S16S`.
7. Advertise one fixed-gain `RCA In`, recording limits and full-duplex support;
   bump the driver revision and document the firmware/hardware requirement.
8. Add source invariants for capability gating, independent start/stop and the
   sampler callback contract.

Validation:

- `python3 -m unittest tools/tests/test_repo_tooling.py`
- `make rtg-tests`
- `make quality`
- `make ahi` using `sacredbanana/amiga-compiler:m68k-amigaos`
- inspect the final diff for generated binaries

## 3. Hardware acceptance (completed 2026-07-29)

The R17 run exercised the matched `0xa204` firmware, default-Z3 bitstream,
`zz9000ax.audio`, and AudioMode on a real Amiga/ZZ9000AX system. Playback-only,
second-owner rejection, left-only, right-only, stereo and silence capture,
true one-control full duplex, all six advertised recording rates, repeated
teardown/reopen, and final playback passed without noise or unexpected
behavior. The duplex report recorded exactly 240,000 frames with
`result=PASS`, no callback type errors, no overflow frames, and no generated
playback-tone leakage into the RCA recording.

This completed the production recording hardware gate. The exact R17 evidence
remains the accepted hardware baseline; the later post-review firmware build
and final installed matched release still require combined release smoke.
Rebuilt non-default bitstream variants remain hardware-untested.

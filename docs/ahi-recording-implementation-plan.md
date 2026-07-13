# ZZ9000AX AHI recording implementation plan

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
5. Let the interrupt worker continue playback service and also drain all new
   receive sequences in chronological order.
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

## 3. Hardware acceptance and pull requests

1. Install the firmware build on a test card without replacing its bitstream.
2. Install the matching `zz9000ax.audio` and AudioMode file.
3. Set both ZZ9000AX auxiliary jumpers to `IN` and verify AHI Record at 48 kHz,
   44.1 kHz and one lower rate.
4. Exercise playback-only, record-only and full-duplex start/stop sequences.
5. Open linked draft pull requests. The driver PR closes issue 47; the firmware
   PR references the issue and the driver PR.

Bench validation remains required before either pull request is marked ready,
because the local build can validate the protocol and binary but cannot inject
physical RCA input into the card.


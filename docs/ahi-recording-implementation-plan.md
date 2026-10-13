# ZZ9000AX AHI recording implementation plan

## Status

| State | Scope | Current implementation or record |
| --- | --- | --- |
| Implemented | Firmware protocol and capture producer | `ZZ9000_proto.sdk/ZZ9000OS/src/zz_regs.h` assigns `REG_ZZ_AUDIO_RX_STATUS` to `0xF6` and `REG_ZZ_AUDIO_TX_STATUS` to `0xF8`. `audio_capture.h`, `audio_capture.c`, `ax.c`, and `main.c` implement receive-ring publication, conversion, register access, and receive-buffer retargeting. |
| Implemented | AHI consumer | `include/zz9000_hw.h` and `include/zz9000_ax.h` define the shared interface. `ahi/driver/zz9000ax-ahi.c` gates recording on codec and receive capability, manages the RX ring and callback buffer, and supports independent playback and recording state on one `AHIAudioCtrl`. |
| Implemented | Test coverage | Firmware capture and conversion tests are built by `test/audio/Makefile`; driver source invariants and the one-control duplex utility are covered by `tools/tests/test_repo_tooling.py` and `ahi/duplextest/ZZAXDuplexTest.c`. |
| Partial | Release qualification | The R17 hardware gate accepted the matched `0xa204` firmware, default-Z3 bitstream, driver, and AudioMode on 2026-07-29. The recorded final matched installer/release smoke remains pending, and rebuilt non-default bitstream variants remain hardware-untested. This qualification record cannot be verified from source alone. |

The original in-place conversion step was dropped. `audio_capture.c` copies each receive period to CPU-side scratch buffers before conversion, then writes S16BE output back to the DMA period. The FIR reads input samples that early in-place output would overwrite.

## Purpose

This plan added stereo RCA recording to `zz9000ax.audio` while retaining playback with older firmware. A matching FPGA bitstream, ARM firmware, and AHI driver are required for recording. Firmware without the receive capability remains playback-only.

The interface and AHI-facing behavior are specified in [`ahi-recording-spec.md`](ahi-recording-spec.md). The firmware register protocol is recorded in the firmware repository's `docs/ahi-recording-protocol.md`.

## Design record

### Firmware capture producer

The ADAU1761 publishes RCA input through TDM8 slots 0 and 1. The FPGA converts those slots to the I2S stream used by the existing S2MM formatter, DDR receive ring, and receive interrupt. On receive completion, firmware:

1. invalidates the completed 3840-byte DMA period;
2. converts 960 S16LE stereo frames at 48 kHz to the requested output rate;
3. writes S16BE stereo frames to the same period and flushes the published byte range;
4. updates the receive status; and
5. raises the shared Amiga audio interrupt when recording is enabled.

The converter accepts 160, 240, 480, 640, 882, and 960 output frames per 20 ms period for 8, 12, 24, 32, 44.1, and 48 kHz. Other valid frame counts select the 960-frame endian-swap path. Converter state resets at recording start, on a changed frame count, or when the receive ring changes. Repeated writes of the same frame count retain state so full-duplex playback does not restart capture conversion.

### Register and ring protocol

| Decision | Reason |
| --- | --- |
| `ZZ_REG_AUDIO_CONFIG` at `0xF4` uses write bit 0 for playback interrupts and bit 1 for recording interrupts. Its read bit 0 remains codec presence, while read bit 1 advertises packed transmit status. | Legacy drivers write only zero or one, so their playback control values remain valid. |
| `ZZ_REG_AUDIO_RX_STATUS` at `0xF6` publishes `CAPABLE`, period, and a modulo-4096 sequence. `ZZ_REG_AUDIO_TX_STATUS` at `0xF8` uses the same packed layout for playback. | The status sequence is the publication boundary. It lets the driver distinguish capture-only wake-ups from a playback completion on the shared interrupt. |
| The receive ring has eight 3840-byte periods and the driver consumes at most seven pending periods. | The eighth period may be the formatter's active write target. On overrun, older data is discarded and recording continues. |
| The receive ring follows the transmit ring by `0x8000` bytes in the final 64 KiB card reservation. | The rings occupy 30,720 bytes each, leaving 2,048 bytes for separation and end padding. |

The AHI driver samples the receive sequence at recording start, drains later periods in chronological order, copies each published period into its persistent callback buffer, and calls `ahiac_SamplerFunc` with `AHIRecordMessage` type `AHIST_S16S`. Only the converted frame count is defined in a period.

### AHI behavior

Recording is advertised only when the codec-present bit and receive `CAPABLE` bit are both set. On capable firmware, the driver advertises one fixed-gain input, `RCA In`, a maximum of 960 recording frames, and full-duplex support.

Playback and recording start and stop independently through `AHISF_PLAY` and `AHISF_RECORD`. Full duplex means both directions on one allocated `AHIAudioCtrl`; it does not permit two low-level clients to share the card. A second `AHIsub_AllocAudio` fails before it changes shared registers, buffers, or interrupt servers.

The driver gates playback service on the transmit sequence. A capture-only interrupt therefore cannot advance the playback ring. Firmware and driver retain the legacy playback path when the transmit-status capability is absent.

## Recorded acceptance

The completed R17 hardware run used the matched `0xa204` firmware, default-Z3 bitstream, `zz9000ax.audio`, and AudioMode on a ZZ9000AX-equipped Amiga. It recorded successful playback-only, record-only, and one-control full-duplex operation; left-only, right-only, stereo, and silence capture; all six advertised recording rates; repeated teardown and reopen; and final playback. `ZZAXDuplexTest` recorded 240,000 frames with `result=PASS`, no callback type errors, no overflow frames, and no generated playback tone in the RCA recording.

This acceptance applies only to that matched set. It does not qualify later firmware builds, final installer payloads, or rebuilt non-default bitstreams.

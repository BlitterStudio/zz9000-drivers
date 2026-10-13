# ZZ9000AX AHI recording specification

`zz9000ax.audio` records stereo audio from the ZZ9000AX RCA inputs when the
running firmware advertises the capture protocol. It delivers one 20 ms buffer
at the selected AHI mix rate, supports 8, 12, 24, 32, 44.1, and 48 kHz, and can
record while the same `AHIAudioCtrl` plays.

Recording requires matching FPGA, firmware, and AHI-driver releases. On
firmware without the capture capability, the driver continues to provide
playback and does not advertise recording.

## AHI interface

The driver exposes recording only when `ZZ_REG_AUDIO_CONFIG` reports that the
codec is present and `ZZ_REG_AUDIO_RX_STATUS` reports `CAPABLE`. When those
checks pass, it provides the following AHI interface.

| AHI property | Value |
| --- | --- |
| Allocation flag | `AHISF_CANRECORD` |
| Recording and duplex attributes | `AHIDB_Record = TRUE`, `AHIDB_FullDuplex = TRUE` |
| Input | One input, index 0, named `RCA In` |
| Recording buffer limit | 960 sample frames (`AHIDB_MaxRecordSamples`) |
| Input gain | Fixed at unity (`0x10000`) |
| Monitor volume | Fixed at zero |
| Callback format | `AHIST_S16S`, signed 16-bit stereo frames in Amiga big-endian byte order |

The driver sets `ahiac_BuffSamples` to `ahiac_MixFreq / 50`. For every
published period, it copies exactly that many frames to its bounce buffer and
calls `ahiac_SamplerFunc` with an `AHIRecordMessage`. The buffer remains valid
until the following recording callback.

Start and stop the two directions independently with `AHISF_PLAY` and
`AHISF_RECORD`. For full duplex, pass both flags to `AHIsub_Start` on one
allocated `AHIAudioCtrl`. The low-level AHI interface permits one hardware
owner, so a second `AHIsub_AllocAudio` fails before it changes shared registers,
buffers, or interrupt servers.

## Capture protocol

The driver and firmware share an eight-period receive ring. A period always
occupies 3840 bytes: 960 stereo S16LE frames at the 48 kHz capture rate. Once a
period completes, firmware converts it to the requested output frame count,
rewrites the resulting stereo frames as S16BE at the start of that period,
flushes the converted bytes, and publishes its status. Bytes after the
converted data are undefined.

The driver derives the number of completed periods from the receive sequence,
then processes them in chronological order. Seven completed periods are
recoverable. The eighth ring slot is the active DMA target, so delayed service
that spans more than seven periods drops the older data and continues from the
newest seven. It does not generate replacement silence.

`ZZ_REG_AUDIO_SCALE` (`0x74`) selects the requested output frames per period.
The AHI driver writes `ahiac_BuffSamples` before it enables recording.

| Frames per period | Rate |
| ---: | ---: |
| 160 | 8 kHz |
| 240 | 12 kHz |
| 480 | 24 kHz |
| 640 | 32 kHz |
| 882 | 44.1 kHz |
| 960 | 48 kHz |

Firmware retains converter filter state between periods. It resets that state
when the frame count changes, recording starts, or the receive ring changes;
writing the existing frame count again does not reset it. A value of zero or a
value above 960 selects 960 frames. Values from 1 through 959 that are absent
from the table also produce 960 frames, with byte-order conversion only.

### Receive-ring assignment

Select the receive ring through `ZZ_REG_AUDIO_PARAM` and `ZZ_REG_AUDIO_VAL`:

1. Write `AP_RX_BUF_OFFS_HI` with the high half of the framebuffer-relative
   offset.
2. Write `AP_RX_BUF_OFFS_LO` with the low half.

The low-half write installs the pending CPU pointer and schedules I2S and audio
formatter reinitialization. Until that completes, firmware acknowledges receive
interrupts without conversion, sequence advancement, or an Amiga audio
interrupt. It then starts publishing to the new ring while retaining the
receive sequence.

The AHI driver places its receive ring 32768 bytes after its transmit ring. Each
ring occupies 30720 bytes, leaving 2048 bytes in the 64 KiB audio reservation.
On Zorro II, the transmit-ring base can come from the negotiated aperture
layout; the receive-ring offset remains 32768 bytes after it.

## Registers and interrupts

The driver headers use `ZZ_REG_*` names. The firmware protocol uses the same
registers as `REG_ZZ_*`.

| Register | Access | Bits | Meaning |
| --- | --- | --- | --- |
| `ZZ_REG_AUDIO_CONFIG` (`0xF4`) | Read | 0 | Codec present. |
| | Read | 1 | Packed transmit-period status available at `ZZ_REG_AUDIO_TX_STATUS`. |
| | Write | 0 | Enable playback-period interrupts. |
| | Write | 1 | Enable capture-ready interrupts. |
| `ZZ_REG_AUDIO_RX_STATUS` (`0xF6`) | Read | 15 | Capture protocol available. |
| | Read | 14:12 | Newest completed receive-period index, 0 through 7. |
| | Read | 11:0 | Completion sequence, modulo 4096. |
| `ZZ_REG_AUDIO_TX_STATUS` (`0xF8`) | Read | 15 | Packed transmit-period status available. |
| | Read | 14:12 | Most recently completed transmit-period index, 0 through 7. |
| | Read | 11:0 | Completion sequence, modulo 4096. |

Writes to `ZZ_REG_AUDIO_CONFIG` are masked to bits 0 and 1. Existing software
that writes zero or one therefore retains its playback control values. Reads
have separate semantics: bit 0 remains codec presence, and bit 1 indicates
that packed transmit status is available.

The sequence value is the publication boundary. A driver samples the receive
sequence when recording starts and reads a period only after the sequence
changes. Firmware increments it modulo 4096 after the converted bytes are
visible to the Amiga. Playback and recording share `AMIGA_INTERRUPT_AUDIO`;
events may coalesce, so the driver checks transmit and receive status separately.
A capture-only event does not advance playback.

Recording remains available when the SDK audio fabric owns playback. Firmware
clears a requested playback bit in that case and retains the recording bit.

The driver uses packed transmit status only when both the configuration read bit
and transmit-status `CAPABLE` bit are set. Otherwise it retains the legacy
sequential playback cursor. This preserves compatibility with older firmware,
whose 16-bit transmit value can change without being a packed status word.

## Compatibility history

| Earlier interface | Current handling |
| --- | --- |
| `ZZ_REG_AUDIO_CONFIG` was a zero-or-one playback control. | Zero and one remain valid write masks. |
| Firmware had no receive-status capability. | The driver does not advertise recording. |
| Playback used a sequential refill cursor. | New drivers use capability-gated packed transmit status. |

## Qualification record

The following result was recorded on 2026-07-29 for the production `0xa204`
candidate, R17 firmware, and default-Z3 bitstream on a physical ZZ9000AX
Amiga system. It applies to those matched artifacts.

| Area | Recorded result |
| --- | --- |
| RCA capture | Channel isolation, stereo capture, and silence capture passed. |
| Rates | All six advertised recording rates passed. |
| Lifecycle | Repeated teardown and reopen, final playback, and one-`AHIAudioCtrl` full duplex passed without recorded noise or unexpected behavior. |
| Tooling | Host tests and both repository CI pipelines passed. |
| Other bitstreams | Rebuilt non-default variants were build and report validated; this record contains no hardware qualification for them. |

`ZZAXDuplexTest` checks simultaneous generated playback and RCA capture. A
passing run reports `result=PASS`, keeps the normal RCA analogue pass-through
audible, and writes a WAVE file containing the RCA input rather than the
generated playback tone.

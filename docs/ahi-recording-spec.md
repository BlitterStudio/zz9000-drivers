# ZZ9000AX AHI recording specification

## Scope

Add stereo recording from the ZZ9000AX RCA inputs to `zz9000ax.audio`
without changing the FPGA bitstream. The existing I2S receiver, S2MM audio
formatter, DDR ring and receive interrupt are already present in the shipped
hardware design. This work adds the missing ARM-firmware producer protocol and
AHI-driver consumer.

Recording must remain compatible with:

- old firmware, where the driver continues to provide playback and does not
  advertise recording;
- old drivers, whose `ZZ_REG_AUDIO_CONFIG` writes contain only playback bit 0;
- simultaneous AHI playback and recording;
- every sample rate currently advertised by the `ZZ9000AX` AudioMode.

No Verilog, Vivado project or committed bitstream changes are required.

## AHI-facing behavior

When supported by the running firmware, `zz9000ax.audio` shall:

- return `AHISF_CANRECORD` from `AHIsub_AllocAudio`;
- report `AHIDB_Record = TRUE` and `AHIDB_FullDuplex = TRUE`;
- expose one stereo input named `RCA In`;
- report a maximum recording buffer of 960 sample frames;
- accept `AHISF_PLAY` and `AHISF_RECORD` independently in `AHIsub_Start` and
  `AHIsub_Stop`;
- call `ahiac_SamplerFunc` once for each completed 20 ms capture period, using
  an `AHIRecordMessage` with `ahirm_Type = AHIST_S16S`;
- provide exactly `ahiac_BuffSamples` signed 16-bit stereo frames at
  `ahiac_MixFreq`, in native Amiga big-endian byte order;
- keep the callback buffer valid until the next recording callback.

Input gain is fixed at unity. Input selection has a single valid index, zero.
The driver does not claim an independently adjustable monitor path.

This follows the behavior of the tested UAESND implementation: recording is
started independently of playback, capture data is delivered as signed
16-bit stereo, and the sampler hook receives complete buffers rather than
access to a live DMA area.

## Firmware register protocol

All values below are 16-bit Amiga-visible register values.

### `ZZ_REG_AUDIO_CONFIG` (`0xF4`)

Reads retain their existing meaning: bit 0 reports whether the ZZ9000AX codec
was detected.

Writes become a control mask:

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `ZZ_AX_AUDIO_CONFIG_PLAY` | Enable legacy/AHI playback-period interrupts. |
| 1 | `ZZ_AX_AUDIO_CONFIG_RECORD` | Enable capture-ready interrupts. |

All other bits are reserved and ignored. A legacy write of zero or one
therefore keeps its original behavior.

### `ZZ_REG_AUDIO_RX_STATUS` (`0xF6`)

This new read-only register publishes the completed capture period:

| Bits | Name | Meaning |
| --- | --- | --- |
| 15 | `CAPABLE` | Firmware implements this capture protocol. |
| 14:12 | `PERIOD` | Index, 0 through 7, of the newest completed period. |
| 11:0 | `SEQUENCE` | Completion counter, incremented modulo 4096. |

Firmware without recording support returns zero for the previously unused
register. The driver must require both `CAPABLE` and the existing codec-present
bit before advertising recording.

### `ZZ_REG_AUDIO_TX_STATUS` (`0xF8`)

This read-only 16-bit sequence increments modulo 65536 after every transmit
period completes and before firmware asserts the shared audio interrupt. The
driver samples it when playback starts and advances the player/mixer only when
the value changes. The register is required whenever `CAPABLE` is set in the
receive status; legacy firmware remains on the original playback-only path.

The sequence value is the publication boundary. Firmware shall not change it
until the period named by `PERIOD` has been converted, flushed from the ARM
caches and made visible to the Zorro/ACP side. The Amiga shall sample the
status at recording start and consume only later sequence values.

The shared audio interrupt bit remains `AMIGA_INTERRUPT_AUDIO`. Playback and
recording events may coalesce; after acknowledging it, the driver uses the
transmit sequence to decide whether to service playback and the receive
sequence to discover every still-resident capture period. A capture-only
wake-up must never advance the playback buffer.

## Capture ring and sample format

The driver selects the receive ring through the existing
`AP_RX_BUF_OFFS_HI/LO` audio parameters. The offset is relative to the ARM
framebuffer base, exactly like the transmit-ring parameters.

The receive ring has eight periods at a fixed 3840-byte stride. The formatter
writes each period as 960 native 48 kHz stereo frames in little-endian signed
16-bit format. On every receive completion, firmware shall:

1. invalidate the completed 3840-byte period from the ARM caches;
2. resample its 960 frames to the requested `ZZ_REG_AUDIO_SCALE` frame count;
3. write signed stereo samples in big-endian order at the start of the same
   period;
4. flush `frame_count * 4` bytes and issue the required memory barrier;
5. publish the period and incremented sequence in `ZZ_REG_AUDIO_RX_STATUS`;
6. assert the shared Amiga audio interrupt when recording is enabled.

`ZZ_REG_AUDIO_SCALE` is the number of sample frames per 20 ms period and is
shared with playback. The AHI driver writes `ahiac_BuffSamples`, which is
`ahiac_MixFreq / 50`; valid recording values are 1 through 960. Firmware uses
960 if the register contains an invalid value.

Only the first `frame_count * 4` bytes of a published period are defined. The
remaining bytes retain DMA or prior-period data and must not be copied by the
driver.

The driver places the receive ring 32768 bytes after the existing transmit
ring inside the final 64 KiB reserved at the top of board memory. Each ring is
30720 bytes, leaving 2048 bytes of separation/end padding.

## Resampling

The codec and S2MM formatter continue to run at 48 kHz. Firmware performs a
deterministic per-period linear downsample to every rate advertised by the
AudioMode: 8, 12, 24, 32, 44.1 and 48 kHz. Period boundaries are exact at
20 ms, so no fractional state is shared between periods.

Resampling must use integer arithmetic in the receive ISR. It must not reuse
the playback resampler state. In-place conversion is permitted because every
supported output contains no more frames than the 960-frame source period.

## Overrun behavior

The driver calculates the unsigned modulo-4096 distance from its last consumed
sequence. Although the ring contains eight periods, only seven completed
periods are immutable because the formatter is already writing the eighth.
If more than seven periods completed, older data has been overwritten; the
driver drops it and delivers only the newest seven periods in chronological
order. Recording then continues without restarting the formatter.

No synthetic silence is required for dropped capture periods. A future
diagnostic counter may expose overruns, but it is not part of this protocol.

## Acceptance criteria

- Recording is absent, but playback still works, with firmware lacking the
  `CAPABLE` bit.
- AHI Record records both RCA channels through `zz9000ax.audio` at all six
  AudioMode rates.
- Playback-only, record-only and full-duplex sessions start and stop without
  changing the other direction's state.
- Callback messages use `AHIST_S16S`, the selected mix rate and one complete
  20 ms buffer.
- Repeated opens, starts, stops and closes do not leak memory or leave the
  shared audio interrupt enabled.
- Firmware unit tests cover conversion, status packing and sequence wrap.
- The firmware-only build, AHI Docker build and repository host checks pass.

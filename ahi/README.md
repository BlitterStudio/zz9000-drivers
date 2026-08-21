<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000AX audio stack — `zz9000ax.audio` and `mhizz9000.library`

The ZZ9000AX daughterboard adds a hardware MP3 decoder, a stereo DAC,
and a Paula pass-through mixer to the ZZ9000. AmigaOS-side support is
split across two drivers that share the same FPGA card:

| Component           | Folder        | Artifact               | What it drives |
|---------------------|---------------|------------------------|----------------|
| AHI subdriver       | `ahi/driver/` | `zz9000ax.audio`       | Generic PCM via AHI (MODs, samples, emulators, ScummVM, Eagleplayer, …). |
| MHI library         | `mhi/`        | `mhizz9000.library`    | Hardware MP3 decoding via MHI-aware players (AmigaAmp, etc.). |

Only one of the two can own the card at a time. The driver that wins
attaches an interrupt server with a well-known name (`ZZ9000AX` for
AHI, `mhizz9000` for MHI); the other side notices the name on the
shared interrupt list and refuses to allocate.

Build and install locations are covered in the [main README](../README.md).

## AHI recording

`zz9000ax.audio` supports stereo recording from the ZZ9000AX RCA inputs when
used with firmware that exposes the AX receive-status register. Firmware
without that capability continues to provide playback, but the driver does
not advertise AHI recording.

Set both ZZ9000AX auxiliary jumpers to **IN** before recording. AHI sees one
fixed-gain input named `RCA In`; samples are delivered as signed 16-bit stereo
at the selected AudioMode rate. Playback and recording may be started and
stopped independently, including full-duplex operation.

The production capture path uses a TDM8 slot-0/1 bridge ahead of the existing
I2S receive formatter, DMA ring, and receive interrupt. It therefore requires
a matching FPGA bitstream, ARM firmware, and `zz9000ax.audio` build. The
register and buffer contract is documented in
[`docs/ahi-recording-spec.md`](../docs/ahi-recording-spec.md).

For the production full-duplex hardware gate, build and run
[`ZZAXDuplexTest`](duplextest/README.md). It starts playback and recording
together on one low-level AHI control, unlike two-client tests involving AHI
Record and a separate player.

## Hardware revisions

Early ZZ9000AX **Revision 1** boards carry an opamp at **U4** on the
Paula pass-through path that over-amplifies Paula's line-in before
the mixer. The practical effect is that raw Paula drowns out AHI and
MHI playback even with the mixer set symmetric — MP3 / MOD sound
noticeably quieter than the same Amiga's chip-audio through the
ZZ9000AX. MNT resolved this in subsequent revisions by **removing
U4**.

If you're on a new-revision board (U4 already absent), everything
balances correctly at the default mixer setting and you don't need
to touch anything.

If you have an unfixed R1, you have two choices:

1. **Desolder U4** — MNT's own fix, produces the cleanest result and
   matches current hardware. Recommended if you're comfortable with
   SMD rework.
2. **Set the operator baseline** — with matched firmware (see
   [Firmware-authoritative control plane](#firmware-authoritative-control-plane)
   below), shift the Paula/AX balance from ZZTop's Audio window. The
   baseline is enforced by the firmware's gain-staging ceiling instead
   of relying on a hand-picked register value.

The former `ENV:ZZ9K_MIX_LEVELS` register override was removed; see
[Runtime tunables](#runtime-tunables-env-variables) below.

## Firmware-authoritative control plane

Both drivers are clients of the firmware's audio control plane. When
zz9k.library is present **and** the running firmware advertises the
audio-control capability, allocating the device submits the owner's
neutral source trim through the control-plane mailbox; the firmware
combines it with the operator baseline under the active scene and owns
every master-chain write (LPF, mixer volume, EQ). Neither driver
stamps DSP state at allocate, Play start, or release anymore, so a
dialed-in scene survives apps opening and closing the device.

The capability is deliberately unadvertised until qualified, so a
matched pair is required for any control-plane behavior:

- **New driver + old firmware** — no control surface, no trims: the
  driver detects the absent capability and falls back to stamp-free
  legacy playback. Audio works; the Paula/AX balance is whatever the
  firmware's own state leaves it at (on an unfixed early R1 that can
  mean loud Paula — see [Hardware revisions](#hardware-revisions)).
  If `ENV:ZZ9K_MIX_LEVELS` is still set, each driver prints one
  load-time line on the debug channel (Sashimi/serial) saying the
  variable is ignored and that the balance remedy needs the matched
  firmware.
- **Old driver + new firmware** — the old driver's DSP register stamps
  (LPF at allocate/Play, mixer volume) are rejected by the firmware's
  scene-authority gate and playback continues unaffected. The early-R1
  balance remedy is unavailable until the drivers are updated and an
  operator baseline is set.

## Runtime tunables (ENV variables)

The variable below is optional. Unset it to get the default behavior.
Its value is read on each `AllocAudio` (AHI) or `AllocDecoder` (MHI)
call, so changing it takes effect the next time an app opens the
device — no reboot required.

`ENV:ZZ9K_MIX_LEVELS` *(AHI + MHI)* and `ENV:ZZ9000AX-NOLPF`
*(AHI only)* were **removed**. Balance intent now flows through the
firmware control plane's operator baseline and LPF intent through the
active scene (see
[Firmware-authoritative control plane](#firmware-authoritative-control-plane)).
Neither driver reads either variable; if `ZZ9K_MIX_LEVELS` is still
set, the drivers print a one-line load-time notice on the debug
channel (Sashimi/serial) that it is ignored. Delete the stale
variables (`Unsetenv ZZ9K_MIX_LEVELS`, `Unsetenv ZZ9000AX-NOLPF`,
plus their `ENVARC:` copies if you used `setenv SAVE`) when updating.

### `ENV:ZZ9K_INT2` *(AHI + MHI)*

If this variable exists, both drivers attach their interrupt server
to **INT2** (`INTB_PORTS`) instead of the default **INT6**
(`INTB_EXTER`). Useful if something else on your system is
monopolising INT6 (some 68060 accelerator boards, certain SCSI
controllers, poorly-behaved networking hardware). The two drivers
**must** agree — AHI and MHI coordinate on the same interrupt line,
so this setting has to be consistent for both or they won't see each
other's ISR and the mutual-exclusion check will break. Setting the
variable configures both drivers uniformly; don't partially enable
it.

```
setenv ZZ9K_INT2 1
```

With firmware 2.3+ the preferred home for this option is `int2 = on`
in the SD card's `ZZ9000.CFG` (editable from ZZTop's Settings window),
which configures AHI, MHI and ZZ9000Net uniformly. An existing
`ENV:ZZ9K_INT2` variable still takes precedence over the config file,
so remove the ENV variable when migrating.

## Troubleshooting

| Symptom                                               | Likely cause / fix |
|-------------------------------------------------------|---------------------|
| Paula much louder than MP3/MOD through the card       | Early R1 (U4 opamp). Desolder U4, or — on matched firmware — set the operator baseline in ZZTop's Audio window. |
| Muffled / dull AHI output at low sample rates         | LPF is scene-owned. Raise the scene LPF cutoff in ZZTop's Audio window (the old `ENV:ZZ9000AX-NOLPF` bypass was removed). |
| "Can't allocate! Hardware already used by MHI/AHI."   | The other driver owns the card. Close whatever MHI/AHI app is running first. |
| Audio device fails to open on specific accelerators   | INT6 conflict. `setenv ZZ9K_INT2 1` to move both drivers to INT2. |
| Short random burst before playback on first app open  | Fixed in recent commits (driver now silences the DAC at allocate time). Update to the latest `zz9000ax.audio`. |
| AHI application has no recording option               | Update both the ARM firmware and `zz9000ax.audio`; old firmware is intentionally detected as playback-only. |
| Recording is silent                                   | Move both ZZ9000AX auxiliary jumpers to `IN` and select `RCA In`. |

## References

- MNT community forum, **"ZZ9000AX mixing levels register"** —
  <https://community.mnt.re/t/zz9000ax-mixing-levels-register/1011>
  (documents the undocumented `AP_DSP_SET_VOLUMES` parameter the
  removed `ZZ9K_MIX_LEVELS` override used to write).
- AHI developer documentation — <https://aminet.net/package/dev/misc/ahidev>
- MHI SDK — shipped with the MHI-aware player's source; the public
  interface definitions this library implements live in `mhi/mhilib.h`
  and `mhi/mhizz9000.h`.

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

On firmware that predates the audio fabric, only one of the two can
own the card at a time: the driver that wins attaches an interrupt
server with a well-known name (`ZZ9000AX` for AHI, `mhizz9000` for
MHI); the other side notices the name on the shared interrupt list
and refuses to allocate.

On matched firmware advertising the audio fabric and its rate flag,
AHI playback instead runs as a **fabric lease producer**: the driver
mixes at the selected AHI rate into a granted card-side ring and the
firmware compositor converts and mixes it beside every other producer
— so AHI sound effects, modules, and emulators play **at the same
time** as MHI/ZZPlay music. The legacy register path remains the
byte-identical fallback for non-fabric stacks, and AHI's own
low-level exclusivity (one `AllocAudio` owner) is unchanged on both
paths.

Build and install locations are covered in the [main README](../README.md).

## Mixing channels

`zz9000ax.audio` 4.29 advertises up to **8 mixing channels** per mode
(`AHIDB_MaxChannels` was 1 before, which is why the channel selector in
AHI Prefs was greyed out). The driver is an `AHISF_MIXING` subdriver:
AHI mixes all allocated channels in software on the Amiga's CPU — linear
interpolation, 32-bit accumulation — and the card still receives a
single stereo PCM stream, exactly as before. Nothing in the hardware
transport, the fabric lease, the legacy register path, or MHI coexistence
changes with the channel count; the ceiling is a host CPU budget, not a
hardware limit.

Practical guidance:

- The actual count is chosen per mode in AHI Prefs; CPU cost accrues only
  for channels that are actively playing (silent channels take AHI's
  cheap silence path).
- 68030: 2–4 channels at 44.1/48 kHz. 68040: 4–8. 68060 / Apollo core:
  8. (The community two-channel variant of the stock 4.19 driver —
  Xanxi/Fitzsteve, same one-line change — was validated with ZZQuake on
  a 68030/25.)
- Overcommitting shows up as AHI CPU-limiter period skips — audible
  stutter — not as a driver failure. If 8 proves comfortable on your
  hardware, raising the ceiling further is a one-line change
  (`ZZ_AX_AHI_MAX_CHANNELS` in `driver/zz9000ax-ahi.c`).

UAE's AHI is not a yardstick here: its m68k-side driver advertises 8
channels while the actual mixing runs on the host CPU, so emulated
channel counts are effectively free.

## AHI timer pairing

Version 4.28 calls `PostTimer` after every executed `PreTimer`, including
when AHI's CPU limiter skips mixing. Both fabric and legacy playback use
this pairing. Earlier versions left the previous exit timestamp stale on
a skip, which could incorrectly suppress subsequent periods. The CPU
limit, 50-Hz pacing, startup runway and buffer bounds are unchanged.

The target regression in `driver/timer_pair_test.c` includes the real
driver. It checks recovery of complete, ordered PCM after an overload
skip in both paths, and balanced timing without delivery for oversized
legacy periods. Scheduling, hook dispatch and coherent RAM are controlled
by the fixture; this does not qualify real interrupt, DMA or cache timing.

After the normal AHI build has staged the SDK headers, run from the repo
root (the executable can also run on AmigaOS):

```sh
tools/amiga-docker.sh ahi/driver m68k-amigaos-gcc \
  timer_pair_test.c asmfuncs.s -m68020 -O3 \
  -I../../include -Izz9k-headers -ffunction-sections -fdata-sections \
  -Wl,--gc-sections -Wall -Wextra -Werror -Wno-unused-parameter \
  -o timer_pair_test -ldebug -lamiga -noixemul
vamos --cpu=68020 --ram-size=8192 --stack-size=64 ahi/driver/timer_pair_test
```

The emulator verification used `amitools==0.8.1` and `machine68k==0.3.0`.
All three cases must pass. Hardware still needs a separate playback and
recording check; a corrected timing pair does not diagnose a weak analog
return.

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

Newer revisions omit U4, but that alone does not qualify every card's
default gain as distortion-free. With no saved audio settings, current
firmware uses the one-card-informed Paula/AX ceiling fallback 48/80
and parity baseline 36/72 (Level 132/160). Saved settings take
precedence, including older louder pairs; check ZZTop before testing.

If you have an unfixed R1:

1. **Desolder U4** — MNT's analog-path fix, if you're comfortable with
   SMD rework. A digital mixer or limiter cannot undo analog distortion
   already produced before its input.
2. **Measure and set each clean ceiling and baseline** — matched
   firmware exposes these controls in ZZTop's Audio window. The
   qualified R1 card measured Paula 48 / AX 80. With the permanent
   post-mix limiter the boundary for that calibrated pair is 160;
   firmware caps each leg at its own ceiling first. The older
   three-quarter-of-AX boundary (60 for 48/80) is pre-limiter history.
   Save the card's own measurements, not the example pair.

The former `ENV:ZZ9K_MIX_LEVELS` register override was removed; see
[Runtime tunables](#runtime-tunables-env-variables) below.

## Firmware-authoritative control plane

Both drivers are clients of the firmware's audio control plane. When
zz9k.library is present **and** the running firmware advertises the
audio-control capability, allocating the device submits the owner's
neutral source trim — the reserved keep-baseline word, "no trim from
this owner" — through the control-plane mailbox, and the same word is
resubmitted at release. The firmware owns every master-chain write
(LPF, mixer volume, EQ): neither driver stamps DSP state at allocate,
Play start, or release, so a dialed-in scene survives apps opening and
closing the device.

The MHI app mixer API (`MHISetParam` for volume, panning, prefactor,
and the EQ bands) is legacy-only against pre-control-plane firmware:
those parameters map straight onto the master chain the scene module
owns, so on control-plane firmware the call reports the documented
not-supported status and the chain is left alone — use scenes
(ZZTop's Audio window) instead.
The calibrated matched firmware advertises the capability after its
hardware gate. Clients still require a matched pair and retain these
mixed-version fallbacks:

- **New driver + old firmware** — no control surface, no trims: the
  driver detects the absent capability and falls back to legacy
  playback with the old anti-alias stamps (the AHI LPF at half the
  mix rate, the MHI 20000 Hz LPF at Play start), so old firmware
  behaves as before. Audio works; the Paula/AX balance is whatever
  the firmware's own state leaves it at (on an unfixed early R1 that
  can mean loud Paula — see
  [Hardware revisions](#hardware-revisions)). The balance remedy
  still needs the matched firmware. If `ENV:ZZ9K_MIX_LEVELS` is
  still set, each driver prints one load-time line on the debug
  channel (Sashimi/serial) saying the variable is ignored and that
  the balance remedy needs the matched firmware.
- **Old driver + new firmware** — the old driver's DSP register
  stamps (LPF at allocate/Play, mixer volume) are rejected by the
  firmware's scene-authority gate and playback continues, but the
  legacy anti-alias LPF tracking no longer happens: the cutoff stays
  where the active scene put it until the drivers are updated.
  Early-R1 baseline/calibration is unavailable until the matched
  drivers expose the control surface.

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
| Muffled / dull AHI output at low sample rates         | On control-plane firmware the LPF is scene-owned: raise the scene LPF cutoff in ZZTop's Audio window (the old `ENV:ZZ9000AX-NOLPF` bypass was removed; on pre-control-plane firmware the legacy half-rate LPF stamp applies as before). |
| ZZTop's Audio button is greyed out                    | The firmware does not advertise the audio-control capability (pre-verification builds), or the AX daughterboard is absent. Update to the matched firmware release that advertises it; playback itself is unaffected. |
| ZZTop says "Audio: control state unavailable - retry" | The firmware did not return a valid audio boundary. Reopen Audio once the control service responds; no audio settings were changed. |
| ZZTop says "Calibration awaiting firmware state..." | Audio edits and Save pause while the firmware confirms the new ceiling pair; the one-second Audio-window timer retries the read. If it remains pending, close and reopen Audio to read the live state. |
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

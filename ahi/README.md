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
2. **Use the driver-side compensation override** — see
   `ENV:ZZ9K_MIX_LEVELS` below. Boosts AHI/MHI and cuts the Paula
   pass-through to pull them back toward parity without rework.

## Runtime tunables (ENV variables)

All three of the variables below are optional. Unset them to get the
default behavior. Values are read on each `AllocAudio` (AHI) or
`AllocDecoder` (MHI) call, so changing a value takes effect the next
time an app opens the device — no reboot required.

### `ENV:ZZ9K_MIX_LEVELS` *(AHI + MHI)*

Overrides the AX's output mixer register (`AP_DSP_SET_VOLUMES`,
parameter 10). Value is a 1-4 digit hex string packing two bytes:

| Byte      | Range    | Meaning                                       |
|-----------|----------|-----------------------------------------------|
| High byte | `0x00`–`0xFF` | ZZ9000AX output level (AHI PCM / MHI MP3). |
| Low byte  | `0x00`–`0xFF` | Paula line-in pass-through level.          |

`0x` prefix, leading whitespace and trailing newlines are tolerated.
On any parse failure the driver falls back to its compiled default
so a stale or mangled file can never brick audio.

**Default: `0x8080`** — the symmetric baseline MNT documents as the
safe maximum for summed output. Matches every fixed-hardware board.
*(Summing both channels above ~`0x100` starts saturating the DAC —
keep `high + low ≤ 0x100` if you change the ratio.)*

Examples (usable with standard AmigaShell `setenv`):

```
; Default — symmetric, fixed hardware or desoldered U4.
setenv ZZ9K_MIX_LEVELS 8080

; Early R1 compensation: boost AHI/MHI ~1.5×, cut Paula ~0.5×.
setenv ZZ9K_MIX_LEVELS C040

; AHI/MHI only, Paula muted.
setenv ZZ9K_MIX_LEVELS FF00

; Paula-only listening (rare, but supported).
setenv ZZ9K_MIX_LEVELS 0080
```

To persist across reboots, use `setenv SAVE` (or edit `ENVARC:`).

### `ENV:ZZ9000AX-NOLPF` *(AHI only)*

If this variable **exists** (any content, any value), the driver's
auto-tuned low-pass filter is bypassed and pinned to `23900 Hz`. The
default behavior sets the LPF cutoff to half the AHI mix frequency,
which is correct for signal integrity at lower sample rates but can
feel slightly dull on content that's already oversampled. Set this
variable only if you know you want flat-response output at every
mix frequency.

```
setenv ZZ9000AX-NOLPF 1
```

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
| Paula much louder than MP3/MOD through the card       | Early R1 (U4 opamp). Desolder U4, or `setenv ZZ9K_MIX_LEVELS C040`. |
| Muffled / dull AHI output at low sample rates         | Expected with auto-LPF on. Set `ENV:ZZ9000AX-NOLPF` to pin the filter at 23.9 kHz. |
| "Can't allocate! Hardware already used by MHI/AHI."   | The other driver owns the card. Close whatever MHI/AHI app is running first. |
| Audio device fails to open on specific accelerators   | INT6 conflict. `setenv ZZ9K_INT2 1` to move both drivers to INT2. |
| Short random burst before playback on first app open  | Fixed in recent commits (driver now silences the DAC at allocate time). Update to the latest `zz9000ax.audio`. |

## References

- MNT community forum, **"ZZ9000AX mixing levels register"** —
  <https://community.mnt.re/t/zz9000ax-mixing-levels-register/1011>
  (documents the undocumented `AP_DSP_SET_VOLUMES` parameter that
  `ZZ9K_MIX_LEVELS` writes).
- AHI developer documentation — <https://aminet.net/package/dev/misc/ahidev>
- MHI SDK — shipped with the MHI-aware player's source; the public
  interface definitions this library implements live in `mhi/mhilib.h`
  and `mhi/mhizz9000.h`.

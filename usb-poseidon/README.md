<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000 USB — Poseidon hardware driver

`zzusbhw.device` connects the MNT ZZ9000 EHCI host controller to the
[commercial Poseidon 4.5 USB stack](https://www.platon42.de/en/poseidon/)
on AmigaOS. Poseidon and its class files are external prerequisites; this
package does not replace or modify them.

## Implemented and qualified scope

| Path | Implementation | Current evidence |
|---|---|---|
| Control and bulk; mass storage | Supported | Existing issue-23 baseline reports a known-good storage device; matched 2.9/2.2 hardware requalification is pending |
| Persistent interrupt; HID and Ethernet status | Supported with event-driven endpoint reuse | Host lifecycle/cadence models pass; Sun HID and AX88772C issue-23 hardware gates are pending |
| Simple ISO IN/OUT | Supported at explicit or ASAP frames | Firmware/driver batch and retirement models pass; commercial-Poseidon hardware gate is pending |
| Realtime ISO IN/OUT; USB Audio | Poseidon add/remove/start/stop handlers implemented | UAC1 capture/playback hardware formats, cadence, and soak are unqualified |
| MIDIStreaming | Descriptor-defined bulk IN/OUT; independent of ISO advertisement | Concurrent audio/MIDI hardware gate is pending |
| Hot-plug, abort, timeout, reset | Generation/epoch fenced with exact-once driver retirement | Host race/error models pass; 100-cycle hardware gate is pending |
| Speed/topology | High speed; split full speed behind a high-speed hub | Direct low speed is deliberately unadvertised; single-TT/multi-TT hardware rows are pending |

Earlier hardware measurements for a high-speed mass-storage stick were about
4 MB/s write and 3 MB/s read on a Zorro III-capable Amiga. Treat those as a
historical baseline, not a result for this matched release. See
[`docs/usb-qualification-matrix.md`](../docs/usb-qualification-matrix.md) for
the exact evidence and gaps behind every claim.

## Where the driver lives

Release packages ship `zzusbhw.device` inside the Commodore Installer
drawer. The installer copies it to
`DEVS:USBHardware/zzusbhw.device`; for a manual install, copy
`usb-poseidon/zzusbhw.device` to `DEVS:USBHardware/` yourself and then
register it with Poseidon.

The ARM-side USB proxy is part of the firmware. Firmware 2.9 and
`zzusbhw.device` 2.2 are the matched pair for persistent interrupt and ISO.
The driver negotiates protocol capabilities at startup. With older firmware it
falls back to the constrained legacy transport and does not advertise simple
or realtime ISO.

Some firmware images may expose a ROM copy of the hardware driver, but the
filesystem copy in `DEVS:USBHardware/` is the release package's explicit
installation path. Confirm with AmigaOS `version ... FILE` that 2.2 is the copy
Poseidon opens.

## Registering the driver with Poseidon

After `zzusbhw.device` is in `DEVS:USBHardware/`, Poseidon may pick it
up during a hardware scan. If it does not appear, add it from
Poseidon's Trident GUI:

1. Launch **Trident** (Poseidon's configuration GUI — usually in
   `SYS:Prefs/Poseidon`).
2. Open the **Hardware** page.
3. Click **Add** and fill in:
   - **Driver name**: `zzusbhw.device`
   - **Unit**: `0`
   - Everything else can be left at the defaults.
4. **Save**, then reboot (or restart Poseidon) so the stack binds the
   new entry.

The ZZ9000 USB ports should now show up as Poseidon USB hardware.

## Superseded driver

Older ZZ9000 releases shipped a separate `ZZ9000USBStorage.device`
that spoke only the mass-storage subset. It is obsolete and **should
be removed** — `zzusbhw.device` via Poseidon replaces it and covers
every endpoint type implemented by the matched firmware and driver. The
installer prompts to delete `Devs:ZZ9000USBStorage.device` when it finds one.

## Troubleshooting

| Symptom | Check |
|---|---|
| Driver exposes no ISO capability | Confirm firmware 2.9 and `zzusbhw.device` 2.2, then run the matched `ZZDiag`; legacy fallback intentionally hides ISO |
| Device not detected | Capture `ZZDiag`, Poseidon's `PsdErrorlog`, Trident binding output, VID:PID, speed, hub address/port, and TT type |
| Transfer stops or returns an error | Record the last successful action and the driver/firmware epochs, counters, queue state, schedule bits, and recent events printed by `ZZDiag` |
| Audio product string ends in `?` | Capture raw string-descriptor bytes plus control requested/actual lengths; do not replace Poseidon files or patch displayed text |
| Direct low-speed device is absent | Expected for this release; test low/full speed only behind a qualified high-speed hub |

For anything unexpected, please attach the output of Poseidon's
**debug log** (enable via Trident → Main → Debug) when reporting.

## Reporting issues

- GitHub: <https://github.com/BlitterStudio/zz9000-drivers/issues>
- Community: <https://community.mnt.re> — `#mnt-amiga` on
  `irc.libera.chat` — <zz9000@mntre.com>

## Building from source (developers)

```sh
cd usb-poseidon
./build.sh          # uses the m68k-amigaos Docker toolchain
```

Produces `zzusbhw.device`. For a normal Amiga test, copy it to
`DEVS:USBHardware/` and point Poseidon at it via the Trident steps
above. For a release, CI places the driver in the installer drawer so
the Commodore Installer can copy it to `DEVS:USBHardware/`.

## License

GPL-3.0-or-later. See `LICENSE`.

<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000 USB Poseidon hardware driver

`zzusbhw.device` lets the [Poseidon 4.5 USB stack](https://www.platon42.de/en/poseidon/)
use the MNT ZZ9000 EHCI host controller on AmigaOS. Install Poseidon and its
class files first. This package supplies only the hardware driver and does not
replace or modify Poseidon.

## Install and register the driver

Release packages place `zzusbhw.device` in the Commodore Installer drawer. The
installer copies it to `DEVS:USBHardware/zzusbhw.device` and installs this
manual. For a manual installation, copy `usb-poseidon/zzusbhw.device` to
`DEVS:USBHardware/`.

If Poseidon does not find the driver during a hardware scan, add it in Trident:

1. Start **Trident**, usually from `SYS:Prefs/Poseidon`.
2. Open **Hardware** and select **Add**.
3. Set the driver name to `zzusbhw.device` and the unit to `0`. Leave the other
   fields at their defaults.
4. Save the entry, then reboot or restart Poseidon.

The release installation path is the filesystem copy in `DEVS:USBHardware/`.
Some firmware images may also expose a ROM copy of the driver.

## Driver and firmware compatibility

The recorded matched pair is firmware 2.9 and `zzusbhw.device` 2.2. The driver
uses `QUERY_CAPS` to negotiate version 2 of the USB proxy protocol at startup.
Firmware version 2 provides request IDs, controller epochs, diagnostics,
persistent interrupt queues, ISO batches, and a maintenance mailbox.

With firmware that does not accept version 2 negotiation, the driver uses the
legacy transport and does not advertise ISO. Simple ISO requires
`ZZUSB_CAP_ISO_SIMPLE`. Realtime ISO also requires
`ZZUSB_CAP_ISO_REALTIME`, the driver callback path, and QuickIO support.

The firmware repository's `docs/usb-proxy-protocol.md` documents the protocol
layout, command IDs, capability bits, and transfer limits.

## Implementation and qualification status

| Area | Driver behavior | Recorded physical evidence |
|---|---|---|
| Control, bulk, and mass storage | Control and bulk transfer paths are implemented. | A prior issue-23 report recorded one high-speed storage device as working; the matched pair has not been rerun on hardware. |
| Persistent interrupt | The driver uses persistent endpoint reuse and event-driven completion. | Host lifecycle and cadence models passed. Sun HID and AX88772C hardware checks remain unrun. |
| Simple ISO IN/OUT | Supports explicit and ASAP frame scheduling. | Firmware and driver batch and retirement models passed. No commercial-Poseidon hardware run is recorded. |
| Realtime ISO IN/OUT and USB Audio | Implements Poseidon add, remove, start, and stop handlers. | UAC1 capture and playback formats, cadence, and soak have not been qualified. |
| `MIDIStreaming` | Uses descriptor-defined bulk IN/OUT independently of ISO advertisement. | Concurrent audio and MIDI hardware testing has not been recorded. |
| Hot-plug, abort, timeout, and reset | Uses generation and epoch fencing with exact-once request retirement. | Host race and error models passed. The 100-cycle hardware check remains unrun. |
| Speed and topology | Supports high speed and split full speed behind a high-speed hub. | Single-TT and multi-TT hub runs have not been recorded. |

The qualification record contains the test boundaries and outstanding device
checks: [`docs/usb-qualification-matrix.md`](../docs/usb-qualification-matrix.md).
The earlier high-speed storage measurement of about 4 MB/s write and 3 MB/s
read on a Zorro III-capable Amiga is historical only; it is not a result for
the matched release.

## Limits and replacement

Direct low-speed devices are deliberately hidden from Poseidon because the
Zynq/ChipIdea root port does not complete direct low-speed endpoint-zero
transfers. Test low-speed devices only behind a qualified high-speed hub.

Remove `Devs:ZZ9000USBStorage.device` if it is present. It is the older
storage-only driver, and the installer prompts to delete it after installing
`zzusbhw.device`.

## Troubleshooting

| Symptom | Collect or check |
|---|---|
| No ISO capability | Check for firmware 2.9 and `zzusbhw.device` 2.2, then run the matching `ZZDiag`. Legacy fallback hides ISO. |
| Device is not detected | Collect `ZZDiag`, Poseidon's `PsdErrorlog`, Trident binding output, VID:PID, speed, hub address and port, and TT type. |
| A transfer stops or returns an error | Record the last successful action, driver and firmware epochs, counters, queue state, schedule bits, and recent `ZZDiag` events. |
| An audio product string ends in `?` | Collect the raw string-descriptor bytes and the control requested and actual lengths. Do not replace Poseidon files or patch displayed text. |
| A direct low-speed device is absent | This is expected. Test it through a qualified high-speed hub. |

For other failures, enable Trident **Main** → **Debug** and attach Poseidon's
debug log to the report.

## Reporting issues

- GitHub: <https://github.com/BlitterStudio/zz9000-drivers/issues>
- Community: <https://community.mnt.re>, `#mnt-amiga` on `irc.libera.chat`, or
  <zz9000@mntre.com>

## Build from source

```sh
cd usb-poseidon
./build.sh
```

`build.sh` uses `m68k-amigaos-gcc` when it is available. Otherwise, it invokes
the repository's `tools/amiga-docker.sh` wrapper. The build produces
`zzusbhw.device`.

## License

GPL-3.0-or-later. See `LICENSE`.

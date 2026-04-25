<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000 USB — Poseidon hardware driver

`zzusbhw.device` is the USB hardware driver that lets the MNT ZZ9000's
on-board EHCI (USB 2.0 High-Speed) host controller plug into the
[Poseidon USB stack](https://www.platon42.de/en/poseidon/) running on
AmigaOS. Once bound, Poseidon's class drivers for control, bulk, and
interrupt devices see ZZ9000 USB ports as regular USB hardware.

## What it supports

| Device class                     | Status         |
|----------------------------------|----------------|
| HID — mice, keyboards            | Works          |
| USB mass storage (FAT/FFS/RDB)   | Works          |
| USB audio class                  | Not yet (firmware ISO proxy missing) |
| USB Ethernet (ASIX AX88772, …)   | Works          |
| Hot-plug / re-enumeration        | Works          |
| Low-speed + full-speed + high-speed | Works (dynamic ULPI XCVR) |

A wired HS mass-storage stick will typically copy at roughly
**4 MB/s write / 3 MB/s read** on a Zorro III-capable Amiga. That's
the range to expect — it's bounded by the Zorro/AXI path, not by the
EHCI itself.

## Where the driver lives

Release packages ship `zzusbhw.device` inside the Commodore Installer
drawer. The installer copies it to
`DEVS:USBHardware/zzusbhw.device`; for a manual install, copy
`usb-poseidon/zzusbhw.device` to `DEVS:USBHardware/` yourself and then
register it with Poseidon.

The matching ZZ9000 firmware still provides the ARM-side USB stack and
mailbox protocol, so keep the board firmware current:
<https://github.com/BlitterStudio/zz9000-firmware>.

Some firmware images may also expose a ROM copy of the hardware driver,
but the filesystem copy in `DEVS:USBHardware/` is the release package's
explicit installation path and can be updated with the driver package.

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
every class. The installer prompts to delete
`Devs:ZZ9000USBStorage.device` if it finds one.

## Troubleshooting

| Symptom                                  | Check |
|------------------------------------------|-------|
| Device not detected                      | Confirm ZZ9000 firmware is current (see firmware repo). Re-scan from Trident. |
| Mass-storage mounts but transfers stall  | Try a different cable / hub. Very long passive cables can drop HS signalling. |
| HS mouse / keyboard feels laggy          | Wireless HS receivers poll on their own schedule; this is normal behaviour on any host. |
| Poseidon's Info window is blank          | Cosmetic only — the driver reports vendor/product correctly, but Poseidon caches the display separately. No functional impact. |

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

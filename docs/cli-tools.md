<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000 command-line tools

The standard installer places `ZZFwUpdate`, `ZZNetStats`, `ZZDiag`,
`ZZNetReady`, `ZZScanlines`, `ZZGamma`, and `ZZRun` in `C:`. They update
files on the card, inspect the network and board, gate network startup, set
scanlines and output gamma, and start trusted ARM apps.
`ZZCapture` is an optional diagnostic with its own `ZZCapture.guide`.
The SDK tools, when installed, are documented in `sdk.guide`.

| Tool | What it does |
| --- | --- |
| `ZZFwUpdate` | Installs a firmware release bundle or copies a local file to the card, and restores a saved backup. |
| `ZZNetStats` | Reads SANA-II and firmware receive counters from `ZZ9000Net.device`. |
| `ZZDiag` | Prints board, aperture, USB, video-capture, SD, audio, Ethernet, and scanline diagnostics. |
| `ZZNetReady` | Returns an AmigaDOS status for a boot script that waits for Ethernet negotiation. |
| `ZZScanlines` | Changes the active scandoubler scanline mode and parity. |
| `ZZGamma` | Sets the output gamma filter live, or prints the current gamma. |
| `ZZRun` | Loads one trusted ARM ELF through `zz9k.library`; requires the Zorro III app-runtime firmware. |

## ZZFwUpdate: firmware updates

`ZZFwUpdate` transfers a local file to the ZZ9000 microSD card without
removing it. Give it an unpacked firmware release drawer or its `BOOT.bin` to
install the bundle. If a sibling `ZZ9K/` drawer contains valid `NAME.ZZM`
files, it transfers those optional modules first and `BOOT.bin` last:

```text
ZZFwUpdate RAM:zz9000-firmware-v2.9.0-zorro3
ZZFwUpdate RAM:zz9000-firmware-v2.9.0-zorro3/BOOT.bin
```

`ZZ9000.CFG` from a release is never transferred. Firmware that does not
advertise `ZZ_FW_CAP_FWUP_MODULE_DIR` receives `BOOT.bin` only. Reboot, then
run the same command again to install the optional modules.

The existing single-file form derives the card filename from the final
component of the source path. Give a second argument to choose a different
filename:

```text
ZZFwUpdate SYS:Storage/zz9000-fw.bin BOOT.bin
```

`ZZFwUpdate` accepts these forms:

| Form | Result |
| --- | --- |
| `ZZFwUpdate <release-drawer-or-BOOT.bin>` | Installs sibling `ZZ9K/*.ZZM` modules first when present, then copies `BOOT.bin` to the root. |
| `ZZFwUpdate <source-file> [dest-name]` | Copies `source-file` to `0:/<dest-name>`. Without `dest-name`, uses the source basename. |
| `ZZFwUpdate RESTORE [name] [-y\|FORCE]` | Promotes the saved backup for `name` to the active file. `name` defaults to `BOOT.bin`; `-y` and `FORCE` skip confirmation. |

`dest-name` and the restore name are either an existing root filename of 1–64
characters from `A-Z`, `a-z`, `0-9`, `.`, `_`, and `-`, or a module target
exactly `ZZ9K/NAME.ZZM`, with an uppercase 1–8 character `NAME` made from
`A-Z`, `0-9`, and `_`. A leading slash, a volume prefix, other directories,
and deeper paths are rejected by the CLI.

During a transfer, it prints the card address, source and destination, then
byte and percentage progress. On success it prints the bytes written and,
when measurable, elapsed time and throughput. If a transfer fails after it
starts, the client aborts the temporary card file.

### Backups and rollback

Replacing an existing file preserves the previous file under a derived backup
name. For example, replacing `BOOT.bin` saves the previous image as
`BOOT.bak`. Restoring `BOOT.bin` promotes `BOOT.bak`:

```text
ZZFwUpdate RESTORE
```

The command asks for confirmation unless `-y` or `FORCE` is present. It moves
the file being replaced to an internal discard path, which firmware attempts
to clear at the next boot. A restore therefore needs a running firmware with FWUP RESTORE
support and a saved backup. It can recover a booting firmware that misbehaves;
if the card no longer boots, restore the card on another computer.

ZZTop's Firmware page uses the same FWUP protocol for file install and
restore.

## ZZ9000Net.device: networking

`ZZ9000Net.device` in `Devs:Networks/` is the SANA-II Ethernet driver. It
programs the firmware multicast hash filter. The driver adopts the firmware
MAC address, including a `mac` value from `ZZ9000.CFG`; `ENV:ZZ9K_MAC`
overrides that address. `ENV:ZZ9K_INT2`, or `int2 = on` in `ZZ9000.CFG`,
selects INT2 instead of the default INT6.

The installer also copies a DHCP Roadshow NetInterface template to
`Devs:NetInterfaces/ZZ9000Net`. Edit that template if the network requires
different settings.

### ZZNetStats

`ZZNetStats` opens `ZZ9000Net.device` and prints the SANA-II global counters,
firmware receive status, and any driver special counters. The default device
is `Networks/ZZ9000Net.device`, unit `0`.

```text
ZZNetStats
ZZNetStats DEVICE=Networks/ZZ9000Net.device UNIT=0
ZZNetStats Networks/ZZ9000Net.device 0
```

The accepted arguments are case-sensitive:

| Argument | Meaning |
| --- | --- |
| `DEVICE=<name>` | Device name to open. |
| `UNIT=<n>` | Device unit to open. |
| `<name> [<unit>]` | Positional alternatives for device name and unit. |
| `MONITOR` | Keeps the device open and prints one compact sample until Ctrl-C. |
| `SECS=<n>` | Monitor interval in seconds. The default is `2`; values below `1` become `1`. |

Normal output includes `PacketsReceived`, `PacketsSent`, `BadData`,
`Overruns`, `UnknownTypesReceived`, `Reconfigurations`, and the last-start
time. It also prints `FirmwareRXReady`, `FirmwareRXReserved`,
`FirmwareRXBackpress`, `FirmwareRXDropped`, and `FirmwareRXPauseSent` when
the board is present, plus special statistics supplied by the driver.

Use `MONITOR` before reproducing a network stall and redirect the output to a
local file:

```text
ZZNetStats MONITOR >RAM:netmon.log
```

Each monitor line reports receive and transmit totals, `RxEmptySlot`, the SMB
ACK probe counters, and `FwRdy`. `P445In`, `SrvAck`, `SrvAckUpd`, `P445Out`,
`TxSeq`, and `TxSeqMax` describe only TCP traffic on port 445. `SECS` has no
effect unless `MONITOR` is present. A `?` argument prints the usage text.

## ZZDiag: board diagnostics

`ZZDiag` reads the card registers and prints a diagnostic report. Use it
without arguments for one sample:

```text
ZZDiag
ZZDiag 3 50
```

| Form | Result |
| --- | --- |
| `ZZDiag [samples] [delay_ticks] [capture.ppm]` | Prints `samples` reports, with `delay_ticks` AmigaDOS ticks between reports. Defaults are `1` and `50`. A sample count below `1` becomes `1`; a negative delay becomes `0`. |
| `ZZDiag usb-driver` | Prints diagnostics exported by `zzusbhw.device` only. It does not require a board lookup. |

A normal report identifies the card and Zorro version, reports the negotiated
Zorro II aperture layout where applicable, and prints firmware, USB, SD,
audio, Ethernet, scanline, RTG, and VideoCap registers. When available, it
also prints firmware and `zzusbhw.device` USB diagnostic snapshots and their
recent events.

Give a third positional argument to create a native-capture PPM file:

```text
ZZDiag 1 50 RAM:capture.ppm
```

The capture path requires a Zorro III board. After the optional delay,
`ZZDiag` collects VideoCap probe comparisons when the firmware exposes them,
then writes a binary P6 PPM image at `1280x320`. It prints `CapturePPM` with
the path and written size, or an error if the file cannot be created or
written.

## ZZNetReady: boot-time link check

`ZZNetReady` is for `S:User-Startup`. It checks that a ZZ9000 is present and,
when supported by the firmware, waits for Ethernet negotiation. It does not
need a TCP/IP stack.

```text
ZZNetReady
ZZNetReady TIMEOUT=10 QUIET
```

| Argument | Meaning |
| --- | --- |
| `TIMEOUT=<seconds>` | Maximum time to wait for negotiation. The default is `5`. |
| `QUIET` | Suppresses the status message. |

It returns `0` (`OK`) when the link is ready. Firmware without
`ETH_CONFIG_CAP_LINK_STATE` also returns `0`, preserving the prior
always-start behavior. It returns `5` (`WARN`) if no card is found, the link
is not ready before the timeout, or AmigaDOS cannot parse the arguments.
`WARN` keeps `S:User-Startup` running under its usual `FAILAT 10` setting.

```text
ZZNetReady QUIET
If WARN
  ; no card or no link - skip the stack this boot
Else
  Run SYS:Network/... ; or your stack's start script
EndIf
```

`ETH_CONFIG_CAP_LINK_STATE` reports that PHY auto-negotiation completed and
the Ethernet MAC initialized since power-on. It is not cleared when the cable
is later removed.

## ZZScanlines: scanlines

`ZZScanlines` changes the active scanline setting of the scandoubler.

```text
ZZScanlines 0
ZZScanlines 1 0
ZZScanlines 2 1
ZZScanlines 3 0
```

| Argument | Values |
| --- | --- |
| `mode` | `0` off, `1` classic, `2` soft, or `3` gradient. |
| `parity` | Optional: `0` for odd dark lines, the default; `1` for even dark lines. |

The command applies the setting immediately and prints the card address and
the selected state. Scanlines operate in AGA scandoubled modes and RTG modes
below 350 lines; they are disabled in interlaced and high-resolution RTG
modes.

`ZZScanlines` does not save settings. To retain them across a power cycle,
write `scanline_mode` and `scanline_parity` to `ZZ9000.CFG`; ZZTop's Save
command writes those keys. The firmware accepts mode values `0`–`3` and
parity values `0`–`1`.

## ZZGamma: output gamma

`ZZGamma` sets the video formatter's output gamma live, or prints the
current setting when called without arguments.

```text
ZZGamma
ZZGamma 1.8
ZZGamma 0.75
ZZGamma OFF
```

| Argument | Values |
| --- | --- |
| `value` | Gamma `0.20`–`5.00` with one or two decimals (`1.8`, `0.75`, `2`). Values above `1.00` brighten, values below `1.00` darken. |
| `OFF` | Returns to `1.00` (identity curve, filter off). |

The command prints the value it reads back, for example `Gamma 1.80`.
When the firmware or the loaded bitstream has no gamma LUT it prints an
explanation and exits with `WARN` (5).

`ZZGamma` changes the live setting only. To keep a value across a power
cycle, write `gamma = 1.80` to `ZZ9000.CFG`, or set it with the **Gamma**
slider on ZZTop's Display page and press Save.

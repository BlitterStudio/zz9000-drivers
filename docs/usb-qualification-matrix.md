# ZZ9000 USB qualification matrix

This matrix qualifies only matched ZZ9000 firmware and `zzusbhw.device`.
Commercial Poseidon 4.5 and its installed classes are an immutable external
compatibility baseline; they are not built, packaged, replaced, or patched.
Automated evidence is not promoted into a physical-device claim.

## Matched build identity

| Field | Evidence captured 31 August 2026 |
|---|---|
| Build host | Windows 11 Pro 10.0.26200, AMD Ryzen 7 9800X3D x64, Docker Desktop |
| Source branches | `usb-host-reliability` in `zz9000-firmware` and `zz9000-drivers` |
| Firmware | 2.9; Arm GNU Toolchain 13.2.Rel1; `ZZ9000OS.elf` SHA-256 `3ae18a153b913b95df7737fba6f47131d125e9b8ac620501a3c26533b46d8d3c` |
| Default boot image | `BOOT-sdk-docker.bin` SHA-256 `ddf41c4ea750f062686cd2032ab18c49b12d1e063d6161fa0291d55d717ca996` |
| Zorro III release archive | SHA-256 `d5da172745db4745136370636b5a8907088f35d688e30f53dfe68d1c7c07464e` |
| Driver | `zzusbhw.device` 2.2; `m68k-amigaos-gcc` 6.5.0b; SHA-256 `56a3c3ef94ac1fade39c3b06b38839bfdb4f0b2549b5f8b0e2f97c21ea61d703` |
| Diagnostics | `ZZDiag` 1.12; SHA-256 `51625e885c37ba97978116e444d7196ed447e276bca3a67ca540758420679bc3` |
| Poseidon reference | Commercial Poseidon 4.5 runtime required. Read-only source reference `bc56025c33345739963e65ce8253f747be9ab6bd` remained clean. Installed Amiga files were unavailable for checksum capture. |

## Automated gates

| Gate | Result | Evidence boundary |
|---|---|---|
| Firmware USB proxy/EHCI host models | Pass | `make -C test/usb test`: protocol, identity/recovery, EHCI lifecycle, diagnostics, periodic scheduling, shared IRQ, iTD/siTD, and ISO batch tests |
| Driver host models | Pass | `make -C usb-poseidon/tests test`: legacy ownership, exact-once engine, error mapping, diagnostics, interrupt delivery, ISO wire/planner/lifecycle |
| Firmware/driver mirror | Pass | Four Python contract tests plus direct checker; command, status, capability, diagnostics, and ISO constants match |
| Firmware artifact | Pass | Current and legacy-bitstream ELFs linked; default `BOOT-sdk-docker.bin` generated from the committed bitstream |
| Firmware release profiles | Pass for assembly only | Seven archives generated with `--require-all`: Zorro III, Zorro III no-fast-RAM, Zorro II, Zorro II 2 MiB, A500, A500 2 MiB, A500+ |
| Amiga artifacts | Pass for cross-build only | `zzusbhw.device` 2.2 and `ZZDiag` 1.12 linked in `sacredbanana/amiga-compiler:m68k-amigaos` |
| Driver release package | Pass for assembly only | Full release input check passed and the installer ZIP assembled with fresh driver, diagnostics, USB README, and this matrix |

These gates validate code, wire compatibility, artifact construction, and
bounded lifecycle models. They do not validate electrical behavior, Zorro bus
timing, commercial-Poseidon callbacks, a particular USB descriptor set, or
sustained traffic on an Amiga.

## Issue 23 device gates

| Device | Historical observation | Matched 2.9/2.2 result | Missing physical evidence |
|---|---|---|---|
| Known-good mass storage | Reported working, including through an unpowered USB 3 hub in USB 2 fallback mode | Not run | Exact model/VID:PID, descriptors, speed, topology, throughput, hotplug count |
| Sun Type 6 keyboard | Direct attachment reported to hard-lock or reboot | Not run | Exact model/VID:PID, descriptors, direct and hub result, last successful action, `ZZDiag` |
| Sun Type 6 mouse | Direct attachment reported to hard-lock or reboot | Not run | Exact model/VID:PID, descriptors, direct and hub result, last successful action, `ZZDiag` |
| ASUS ASIX AX88772C adapter | Hard-lock or reboot reported | Not run | VID:PID, descriptors, direct and hub result, traffic action at failure, `ZZDiag` |
| USB MIDI device | Soft crash reported; MIDIStreaming classification under Audio is expected | Not run | Model/VID:PID, bulk endpoint descriptors, binding, concurrent audio/MIDI result |
| USB audio input device | Product string reported with a trailing `?`; streaming previously unavailable | Not run | Model/VID:PID, raw string bytes, control requested/actual lengths, UAC1 formats, capture/playback result |

Stop after the first useful reproduction of a hard-lock-prone device. Do not
repeat uncontrolled crash loops before the matched diagnostics are installed.

## Transfer and topology claims

| Axis | Implemented/modelled | Physical claim |
|---|---|---|
| Control and bulk | Exact request ownership, bounded actual length, precise errors | No matched hardware run |
| Interrupt | Persistent endpoint, interval/split model, event-driven reap | No HID, hub, or AX88772C run |
| Simple ISO | High-speed and split full-speed IN/OUT, explicit/ASAP frame, per-packet status | No commercial-Poseidon run |
| Realtime ISO | Add/remove/start/stop handlers, four-batch replenishment, stop/reset fencing | No UAC1 run |
| MIDI | Remains on descriptor-defined bulk endpoints | No concurrent MIDI/audio run |
| High speed | Implemented | No matched root-port run |
| Full speed behind high-speed hub | Single-TT/multi-TT split scheduling modelled | No physical hub run |
| Direct low speed | Deliberately unadvertised | Unsupported in this release |

Simple and realtime ISO flags are exposed only after the driver receives the
complete firmware 2.9 protocol-v2 capability set and initializes its callback
support. This capability advertisement is an implementation fact, not an
audio-device qualification claim.

## Profile and version-pair coverage

| Configuration | Automated result | Physical result |
|---|---|---|
| New firmware 2.9 + new driver 2.2 | Full contract, host models, build, and package pass | Not run |
| New driver + legacy firmware | Legacy fallback and hidden ISO capabilities modelled | Not run |
| Legacy driver + new firmware | Append-only v1 command compatibility modelled | Not run |
| Zorro III | Archive assembled | A4000 run unavailable |
| Zorro III without fast RAM | Archive assembled | Hardware unavailable |
| Zorro II | Archive assembled | Hardware unavailable |
| Zorro II 2 MiB | Archive assembled | Hardware unavailable |
| A500-family profiles | Three archives assembled | Hardware unavailable |

## Required hardware qualification

No real Amiga or target USB device was available in this work session.
Therefore none of the following release gates has passed:

- 100 attach/enumerate/use/detach cycles for every issue-23 device.
- 24-hour keyboard, mouse, AX88772C, mass-storage concurrency.
- 60-minute capture and playback for every advertised UAC1 format while MIDI,
  HID, and storage remain active.
- Error injection and disconnect during enumeration, queued, in-flight,
  completion, stream-stop, and reset states.
- Single-TT and multi-TT hub runs with address, port, think time, and speed
  recorded.
- New/new and both mixed-version pairings on the same Poseidon 4.5 install.
- Zorro III first, then no-fast-RAM, Zorro II, 2 MiB, and A500-family physical
  profiles.
- Before/after checksums of installed `poseidon.library`, class files, and
  configuration.

Until those rows are recorded, describe the release as host-modelled,
cross-built, and packaged—not hardware-qualified.

## Per-run evidence packet

Record host model, CPU/accelerator, Kickstart, Workbench, firmware/driver
versions and checksums, Poseidon/library/class versions and checksums, device
and hub VID:PID, topology and power, negotiated speed, TT type/think time,
attach timestamp, last successful action, `PsdErrorlog`, Trident device and
binding output, raw device/configuration/string descriptors, and correlated
`ZZDiag` output. Retain raw logs outside the source tree; commit only concise
results and explicit gaps here.

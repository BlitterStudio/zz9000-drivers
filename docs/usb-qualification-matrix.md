# ZZ9000 USB qualification matrix

The evidence recorded on 31 August 2026 qualifies firmware 2.9 and
`zzusbhw.device` 2.2 through host models, cross-builds, and package assembly.
It does not qualify the pair on Amiga hardware or with target USB devices.

Commercial Poseidon 4.5 and its installed classes are external prerequisites.
This repository does not build, package, replace, or patch them. Treat
automated evidence as code and artifact evidence only.

## Current qualification status

| Area | Status | Recorded evidence or gap |
|---|---|---|
| Firmware and driver pair | Host-modelled, cross-built, and packaged | Matched 2.9/2.2 automated gates passed. |
| Hardware and USB devices | Not qualified | No Amiga or target USB device was available for the recorded work session. |
| Commercial Poseidon 4.5 | External baseline | The source reference at `bc56025c33345739963e65ce8253f747be9ab6bd` was clean. Installed Amiga files were unavailable for checksum capture. |
| Direct low-speed devices | Unsupported in this release | The driver deliberately does not advertise them. |

## Recorded build evidence

| Field | Evidence captured 31 August 2026 |
|---|---|
| Build host | Windows 11 Pro 10.0.26200, AMD Ryzen 7 9800X3D x64, Docker Desktop |
| Source branches | `usb-host-reliability` in `zz9000-firmware` and `zz9000-drivers` |
| Firmware | 2.9; Arm GNU Toolchain 13.2.Rel1; `ZZ9000OS.elf` SHA-256 `3ae18a153b913b95df7737fba6f47131d125e9b8ac620501a3c26533b46d8d3c` |
| Default boot image | `BOOT-sdk-docker.bin` SHA-256 `ddf41c4ea750f062686cd2032ab18c49b12d1e063d6161fa0291d55d717ca996` |
| Zorro III release archive | SHA-256 `d5da172745db4745136370636b5a8907088f35d688e30f53dfe68d1c7c07464e` |
| Driver | `zzusbhw.device` 2.2; `m68k-amigaos-gcc` 6.5.0b; SHA-256 `88b4a55a1e8508bbbbca27d069cb7c51acfc1cd3aff232559c56a9d3fb3f6c45` |
| Diagnostics | `ZZDiag` 1.12; SHA-256 `51625e885c37ba97978116e444d7196ed447e276bca3a67ca540758420679bc3` |

## Automated and artifact gates

| Gate | Result | Evidence boundary |
|---|---|---|
| Firmware USB proxy/EHCI host models | Pass | `make -C test/usb test`: protocol, identity/recovery, EHCI lifecycle, diagnostics, periodic scheduling, shared IRQ, iTD/siTD, and ISO batch tests |
| Driver host models | Pass | `make -C usb-poseidon/tests test`: legacy ownership, exact-once engine, error mapping, diagnostics, interrupt delivery, ISO wire/planner/lifecycle |
| Firmware/driver mirror | Pass | Four Python contract tests plus direct checker; command, status, capability, diagnostics, and ISO constants match |
| Firmware artifact | Pass | Current and legacy-bitstream ELFs linked; default `BOOT-sdk-docker.bin` generated from the committed bitstream |
| Firmware release profiles | Pass for assembly only | Seven archives generated with `--require-all`: Zorro III, Zorro III no-fast-RAM, Zorro II, Zorro II 2 MiB, A500, A500 2 MiB, A500+ |
| Amiga artifacts | Pass for cross-build only | `zzusbhw.device` 2.2 and `ZZDiag` 1.12 linked in `sacredbanana/amiga-compiler:m68k-amigaos` |
| Driver release package | Pass for assembly only | Full release input check passed and the installer LHA assembled with fresh driver, diagnostics, USB README, and this matrix |

These gates cover code, wire compatibility, artifact construction, and bounded
lifecycle models. They do not cover electrical behavior, Zorro bus timing,
commercial-Poseidon callbacks, a particular USB descriptor set, or sustained
traffic on an Amiga.

## Recorded device evidence

The following issue-23 reports are historical observations. None was rerun
with the matched 2.9/2.2 pair.

| Device | Historical observation | Matched 2.9/2.2 result | Missing physical evidence |
|---|---|---|---|
| Known-good mass storage | Reported working, including through an unpowered USB 3 hub in USB 2 fallback mode | Not run | Exact model/VID:PID, descriptors, speed, topology, throughput, hotplug count |
| Sun Type 6 keyboard | Direct attachment reported to hard-lock or reboot | Not run | Exact model/VID:PID, descriptors, direct and hub result, last successful action, `ZZDiag` |
| Sun Type 6 mouse | Direct attachment reported to hard-lock or reboot | Not run | Exact model/VID:PID, descriptors, direct and hub result, last successful action, `ZZDiag` |
| ASUS ASIX AX88772C adapter | Hard-lock or reboot reported | Not run | VID:PID, descriptors, direct and hub result, traffic action at failure, `ZZDiag` |
| USB MIDI device | Soft crash reported; MIDIStreaming classification under Audio is expected | Not run | Model/VID:PID, bulk endpoint descriptors, binding, concurrent audio/MIDI result |
| USB audio input device | Product string reported with a trailing `?`; streaming previously unavailable | Not run | Model/VID:PID, raw string bytes, control requested/actual lengths, UAC1 formats, capture/playback result |

Stop after the first useful reproduction of a hard-lock-prone device. Do not
repeat uncontrolled crash loops before installing the matched diagnostics.

## Implemented scope and physical coverage

| Area | Implemented or modelled | Physical result |
|---|---|---|
| Control and bulk | Exact request ownership, bounded actual length, precise errors | No matched hardware run |
| Interrupt | Persistent endpoint, interval/split model, event-driven reap | No HID, hub, or AX88772C run |
| Simple ISO | High-speed and split full-speed IN/OUT, explicit/ASAP frame, per-packet status | No commercial-Poseidon run |
| Realtime ISO | Add/remove/start/stop handlers, four-batch replenishment, stop/reset fencing | No UAC1 run |
| MIDI | Descriptor-defined bulk endpoints | No concurrent MIDI/audio run |
| High speed | Implemented | No matched root-port run |
| Full speed behind high-speed hub | Single-TT/multi-TT split scheduling modelled | No physical hub run |
| Direct low speed | Deliberately unadvertised | Unsupported in this release |

The driver exposes simple and realtime ISO flags only after it receives the
complete firmware 2.9 protocol-v2 capability set and initializes callback
support. That advertisement does not qualify an audio device.

## Version-pair and board-profile coverage

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

## Hardware qualification requirements

Do not describe this release as hardware-qualified until every row is
recorded. The recorded work session did not pass any of these gates.

| Required gate | Required record |
|---|---|
| Device cycling | 100 attach/enumerate/use/detach cycles for every issue-23 device |
| Concurrency soak | 24-hour keyboard, mouse, AX88772C, and mass-storage concurrency |
| Audio and MIDI soak | 60-minute capture and playback for every advertised UAC1 format while MIDI, HID, and storage remain active |
| Failure handling | Error injection and disconnect during enumeration, queued, in-flight, completion, stream-stop, and reset states |
| Hub coverage | Single-TT and multi-TT hub runs with address, port, think time, and speed recorded |
| Version-pair coverage | New/new and both mixed-version pairings on the same Poseidon 4.5 install |
| Board-profile coverage | Zorro III first, then no-fast-RAM, Zorro II, 2 MiB, and A500-family physical profiles |
| Poseidon installation integrity | Before/after checksums of installed `poseidon.library`, class files, and configuration |

## Per-run evidence packet

| Record group | Include |
|---|---|
| System | Host model, CPU/accelerator, Kickstart, Workbench, firmware/driver versions and checksums, Poseidon/library/class versions and checksums |
| Device and topology | Device and hub VID:PID, topology and power, negotiated speed, TT type/think time, attach timestamp |
| Outcome | Last successful action, `PsdErrorlog`, Trident device and binding output, raw device/configuration/string descriptors, correlated `ZZDiag` output |
| Raw logs | Retain raw logs outside the source tree. Commit concise results and explicit gaps here. |

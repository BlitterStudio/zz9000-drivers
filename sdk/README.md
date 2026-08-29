# SDK runtime payloads

End-user components whose sources live in the
[zz9000-sdk](https://github.com/BlitterStudio/zz9000-sdk) repository (where
their headers, test suite, and hardware smoke procedures are):

| Payload | Installs to | What it does |
| --- | --- | --- |
| `zz9k.library` | `LIBS:` | AmigaOS gateway to the SDK v2 firmware services (image/video decode, audio, compression, crypto). Requires SDK-service firmware. |
| `mpega.library` | `LIBS:` | Drop-in MPEGA replacement that decodes MP3 on the ZZ9000's ARM cores; current SDK builds use host-window/card-only allocation flags for supported Zorro 2 audio buffers. |
| `zz9k-picture.datatype` | `Classes/DataTypes/` | Hardware-accelerated picture datatype; JPEG/PNG descriptors are staged inactive under `Storage/DataTypes` for explicit opt-in. |
| `zz9k-info`, `zz9k-services` | `C:` | Board/service diagnostics. |
| `zz9k-view`, `zz9k-mp3`, `zz9k-cryptobench`, `zz9k-archive` | `C:` | Feature tools: accelerated image viewer, MP3 player, crypto-offload benchmark, and archive extractor/tester with LHA/LZH offload where firmware supports it. |
| `ZZPlay` (+ `ZZPlay.info`) | `SYS:Utilities/ZZ9000/` | MPEG-1/P96 video and MP3 media player. Ships capitalised and with a Workbench icon, so it installs as a Workbench application rather than a CLI tool, sharing the ZZ9000 drawer with ZZTop. |

`build.sh` pulls the SDK at the commit pinned in `SDK_REF` (or uses a
`ZZ9000_SDK`/sibling checkout), drives the SDK's own Docker build and package
scripts, and collects the payloads above into `sdk/out/` in installer layout.
`tools/package-local.sh` and the CI release assembly stage them into the
installer drawer from there.

To track a newer SDK: bump `SDK_REF`, rerun `sdk/build.sh`, and smoke-test per
the SDK's `docs/zz9k-release-smoke.md`. Keep SDK bumps synchronized with any
driver code that consumes new SDK headers or allocation flags.

## Zorro II support

Current matched firmware, bitstream, SDK payloads, and drivers negotiate an
aperture-relative layout before the SDK maps any host-window allocation. The
layout has two generations: generation 1 (previous matched sets, and still
published by older FPGA/firmware pairs) provides one shared 64 KiB host heap;
generation 2 (with the direct-ring reservation) shrinks the negotiated host
heap to 16 KiB on the 2 MiB and 4 MiB profiles and carves the freed 48 KiB
for the Z2 audio direct-ring grant. Both generations negotiate and
acknowledge with their own token — a driver update alone never disables RTG
on a generation-1 board. Plan Zorro II heap allocations against the
generation the card actually acknowledges (`zz9k-info` reports it).

| Payload | Zorro II status |
| --- | --- |
| `mpega.library`, `mhizz9000.library`, ZZPlay standalone MP3 | Supported on 2 MiB and 4 MiB through compact staging and card-only rings. |
| `zz9k-view`, `zz9k-picture.datatype` | Supported on 2 MiB and 4 MiB through streamed input and bounded tiles. An over-wide DataType row rejects the accelerated path rather than overrunning the heap. |
| `zz9k-archive` | Streamed test/extract paths are supported; large LHA batch arenas retain their per-member/software fallback. |
| Accelerated `amissl.library` | Supported on 2 MiB and 4 MiB. Provider open reserves one 32-byte probe; other exact 16-byte-aligned persistent buffers grow lazily, and a later allocation miss falls back in software for that operation. |
| ZZPlay MPEG-1/PIP | Unsupported on 2 MiB. On 4 MiB, one aligned YUY2 frame must fit the fixed 224 KiB pool; 352x288 fits and 640x360 does not. |
| Generic `zz9k-inflate`/`zz9k-mp3`/crypto/smoke diagnostics and plain `zz9k-jpeg <file>` output | Still use at least one legacy default shared allocation and are not Zorro II qualification tools. Use the adapted production clients or `zz9k-jpeg --fb` instead. |

The host heap is shared across clients, not provided per process. Keep the
release set matched and use `zz9k-info` to verify the acknowledged layout and
memory counters. The full matrix and hardware-qualification status live in the
SDK's
[Zorro II service documentation](https://github.com/BlitterStudio/zz9000-sdk/blob/master/docs/zz9k-zorro2-services.md).

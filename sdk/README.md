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
| `zz9k-view`, `zzplay`, `zz9k-mp3`, `zz9k-cryptobench`, `zz9k-archive` | `C:` | Feature tools: accelerated image and MPEG-1/P96 video players, MP3 player, crypto-offload benchmark, and archive extractor/tester with LHA/LZH offload where firmware supports it. |

`build.sh` pulls the SDK at the commit pinned in `SDK_REF` (or uses a
`ZZ9000_SDK`/sibling checkout), drives the SDK's own Docker build and package
scripts, and collects the payloads above into `sdk/out/` in installer layout.
`tools/package-local.sh` and the CI release assembly stage them into the
installer drawer from there.

To track a newer SDK: bump `SDK_REF`, rerun `sdk/build.sh`, and smoke-test per
the SDK's `docs/zz9k-release-smoke.md`. Keep SDK bumps synchronized with any
driver code that consumes new SDK headers or allocation flags.

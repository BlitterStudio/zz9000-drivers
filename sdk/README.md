# SDK runtime payloads

The drivers release includes selected end-user components built from the [`sdk/`](https://github.com/BlitterStudio/zz9000-firmware/tree/master/sdk) subtree of the firmware repository. They require matching SDK-service firmware. `zz9k.library` is the AmigaOS gateway to that service ABI; the remaining payloads use it to access image, audio, compression, crypto, and media services.

| Payload | Installer destination | Purpose |
| --- | --- | --- |
| `zz9k.library` | `LIBS:` | AmigaOS gateway for SDK firmware services and the board's mailbox. |
| `aar.library` | `LIBS:` | AAR client library: claims, loads, starts and stops apps on the ARM cores and keeps their leases alive. |
| `mpega.library` | `LIBS:` | Replacement MPEGA library that decodes MP3 on the card. When the card cannot decode a stream it uses `LIBS:mpega-software.library`, where the installer keeps the software `mpega.library` it replaced. |
| `zz9k-picture.datatype` | `SYS:Classes/DataTypes/` | Picture DataType that decodes JPEG, PNG, and WebP on the card. |
| `zz9k-sound.datatype` | `SYS:Classes/DataTypes/` | Sound DataType that decodes MP3, FLAC, and Ogg Vorbis on the card and plays them through the system `sound.datatype`. |
| `ZZ9000-JPEG`, `ZZ9000-PNG`, `ZZ9000-WebP`, `ZZ9000-MP3`, `ZZ9000-FLAC`, and `ZZ9000-OggVorbis` descriptors | `SYS:Storage/DataTypes/` | Inactive DataType descriptors for explicit installation. |
| `zz9k-info`, `zz9k-services` | `C:` | Board and service diagnostics. `zz9k-services --check-release` runs the SDK release check. |
| `zz9k-view` | `C:` | JPEG, PNG, and WebP viewer. |
| `zz9k-mp3` | `C:` | Low-level MP3 decode and transport diagnostic. |
| `zz9k-cryptobench` | `C:` | Crypto-offload benchmark. |
| `zz9k-archive` | `C:` | Archive listing, test, and extraction tool, including LHA/LZH paths where firmware supports them. |
| `aar-run` and `aar-run.info` | `C:` | Reference launcher for trusted ARM apps: it serves an app's files, console, display, audio and input. Its manual installs as `aar-run.guide`. |
| `ZZPlay` and `ZZPlay.info` | `SYS:Utilities/ZZ9000/` by default | Workbench media player for MPEG-1/2, WebM and animated WebP video and MP3, FLAC and Ogg Vorbis audio, with an ARexx port for scripts. The installer lets you select the parent drawer. |

## Build and package

`sdk/build.sh` reads `SDK_REF` and uses `ZZ9000_SDK`, a sibling `zz9000-firmware/sdk` checkout, or a clone in `sdk/work/zz9000-firmware`. Unless `SDK_SKIP_BUILD=1`, it runs the SDK's `scripts/build-m68k-amigaos.sh` and `scripts/package-m68k-amigaos.sh --skip-build`, then copies the selected payloads to `sdk/out/` in installer layout. `SDK_REF` holds the pinned firmware commit. The SDK package also carries `libaarserve.a` and its header for programs that embed the serving loop; `sdk/build.sh` does not collect them, because the installer does not ship them.

To use a newer SDK revision, update `SDK_REF`, rebuild with `sdk/build.sh`, and follow the SDK's [`docs/zz9k-release-smoke.md`](https://github.com/BlitterStudio/zz9000-firmware/blob/master/sdk/docs/zz9k-release-smoke.md). Keep the SDK revision synchronized with driver code that consumes SDK headers or allocation flags.

## App runtime

Apps for the ARM cores are loaded and served over the AAR queue transport.
`zz9k.library` no longer carries a `ZZ9KApp*` client API. The launcher
`aar-run` calls the AAR client library `aar.library` instead. The firmware SDK
builds and packages both, from the aar repository, and the installer puts
`aar.library` in `LIBS:` and `aar-run` in `C:`. The library claims an app,
loads and starts it and keeps its lease alive; `aar-run` serves its requests.
`aar.library` uses `zz9k.library` only to reach the board's mailbox (revision
36 or later). It also needs `mmu.library` 41 or later to map the card's shared
buffers cache-inhibited. See the
[aar-run manual](https://github.com/BlitterStudio/aar/blob/main/client/run/README.md),
installed as `aar-run.guide`, for launcher behaviour.

## Zorro II support

Current generation-2 2 MiB and 4 MiB Zorro II layouts provide a shared 16 KiB CPU-visible host window. Generation 2 reserves 48 KiB of the former 64 KiB window for the audio direct-ring grant. Generation 1 used a 64 KiB host window on 2 MiB and 4 MiB layouts; descriptor-absent legacy 4 MiB stacks retain that layout. A driver update alone does not change the acknowledged generation.

| Payload or operation | 2 MiB | 4 MiB | Current behavior |
| --- | --- | --- | --- |
| `mpega.library` | No | No | Its compact PCM ring is 32 KiB, followed by 16 KiB of feed staging. Neither current layout has a 32 KiB host window, so streams decode through `LIBS:mpega-software.library` when it exists. |
| ZZPlay with AHI | No | No | The compact AHI path requires a 32 KiB PCM ring and 16 KiB staging buffer. |
| ZZPlay card-local streaming | Limited | Limited | Card-local PCM and compressed rings use up to 16 KiB of host-window staging and require an otherwise free window. |
| `zz9k-view` JPEG, PNG, and WebP | No | No | Each viewer path requests a 32 KiB host-window staging buffer. |
| `zz9k-picture.datatype` | Limited | Limited | Input staging is capped at 24 KiB and output tiles at 32 KiB. The accelerated path requires allocations that fit the remaining window. |
| `zz9k-archive` streamed decode, test, and extract | Yes | Yes | Input and output share `min(48 KiB, host window)`. A 16 KiB profile starts with 8 KiB chunks and retries a failed pair allocation down to 4 KiB. |
| Accelerated `amissl.library` | Yes | Yes | Hardware algorithms need a 32-byte host-window probe. Scratch buffers are 16-byte aligned and grow to the requested size. |
| ZZPlay MPEG Program Stream video/PIP | No | No | Current code accepts only an acknowledged generation-1 PIP layout. A legacy 4 MiB pool of 224 KiB fits 352x288 YUY2 (202,752 bytes) and rejects 640x360 (460,800 bytes). |
| `zz9k-inflate`, `zz9k-mp3`, crypto diagnostics, `zz9k-smoke`, and plain `zz9k-jpeg <file>` | No | No | These paths use a legacy default shared allocation. On Zorro II, use adapted clients or `zz9k-jpeg --fb`. |

The host window is shared across clients. Keep firmware, bitstream, SDK payloads, and drivers in one release set. Run `zz9k-info` after boot, confirm that the aperture is active and the layout is acknowledged, then inspect the host-window counters when clients run together.

The SDK's [Zorro II service documentation](https://github.com/BlitterStudio/zz9000-firmware/blob/master/sdk/docs/zz9k-zorro2-services.md) contains the full support matrix and recorded hardware results.

# Accelerated AmiSSL libraries

These optional replacements add the ZZ9000 OpenSSL provider to the AmiSSL core libraries. Existing AmiSSL applications keep using the AmiSSL API. When matching board and firmware services are available, the provider uses the card for selected TLS operations; all other paths use AmiSSL's default software provider.

AmiSSL applications are tied to the major version they were built for, so two libraries are built:

| AmiSSL release | OpenSSL | Library | Used by |
| --- | --- | --- | --- |
| 5.27 | 3.6 | `amissl_v362.library` | Applications built with the AmiSSL 5 SDK, including ZZTop and ZZPlay |
| 6.1 | 4.1 | `amissl_v41x.library` | Applications built with the AmiSSL 6 SDK |

Installing AmiSSL 6.1 adds `amissl_v41x.library` and keeps `amissl_v362.library`, so a system upgraded from 5.27 has both.

The provider source and AmiSSL patch are in [`sdk/integration/amissl/`](https://github.com/BlitterStudio/zz9000-firmware/tree/master/sdk/integration/amissl) in the firmware repository. This directory builds and stages the libraries for the drivers release.

## Install

1. Install AmiSSL 5.27, 6.1, or both. The installer replaces whichever of `LIBS:AmiSSL/amissl_v362.library` and `LIBS:AmiSSL/amissl_v41x.library` is present.
2. Run the ZZ9000 drivers installer and select the accelerated libraries when they are offered.
3. Select the CPU build that matches the machine. `68020-40` is for 68020, 68030, 68040, and Apollo 68080; `68060` is for 68060 and Amithlon.

The installer saves each original library with a `.bak` suffix, for example `LIBS:AmiSSL/amissl_v41x.library.bak`, before replacing it. It creates a backup only when no `.bak` exists, so a later installation does not overwrite the original. To revert, delete the accelerated library and rename its backup.

Reinstalling AmiSSL from Aminet overwrites the accelerated copy of each library that its archive ships. Run the ZZ9000 installer again afterwards to restore acceleration.

A `68020-40` library runs on a 68060, but 64-bit multiplies are trap-emulated there. Use the `68060` library on a 68060.

## Build

Run this from the repository root:

```sh
make amissl
```

`amissl/build.sh` builds the `zz9000-amissl-adtools` image, obtains the firmware repository at the ref in `sdk/SDK_REF` unless `ZZ9000_SDK` names an SDK tree, and invokes `sdk/integration/amissl/build.sh` in the container once per AmiSSL series and CPU target. `OS=os3-68020` or `OS=os3-68060` selects one target and `AMISSL_MAJOR=5` or `AMISSL_MAJOR=6` selects one series; the default builds all four libraries.

The outputs are:

| AmiSSL target | Release directory |
| --- | --- |
| `os3-68020` | `amissl/out/68020-40/amissl_v362.library`, `amissl/out/68020-40/amissl_v41x.library` |
| `os3-68060` | `amissl/out/68060/amissl_v362.library`, `amissl/out/68060/amissl_v41x.library` |

`make build-all` does not build these libraries. `tools/package-local.sh` stages each output that exists. GitHub Actions stages all four release artifacts.

## Offload coverage

The provider registers `zz9000` and the default provider during AmiSSL initialization, preferring `zz9000` where it advertises an operation. It advertises hardware algorithms only when the required crypto-service flags and initial board allocation are available.

| Operation | Card path | Software path |
| --- | --- | --- |
| X25519 | Key management and key exchange | The provider is not advertised without the service; a board failure after advertisement fails the operation. |
| P-256 ECDHE | Key generation and derive | Operations that cannot use the card use the default provider. |
| P-256 ECDSA | SHA-256 certificate verification | Other curves and failed offloads use the default provider. |
| RSA-2048 | PKCS#1 v1.5/SHA-256 certificate verification | RSA-PSS, signing, key generation, unsupported sizes, and other verification use the default provider. RSA verification stays in software by default; `ENV:ZZ9K_RSA_VERIFY_OFFLOAD` enables the board path for testing. |
| AES-128-GCM and AES-256-GCM | TLS record encryption and decryption when the AES-GCM service flag is available | Records below the threshold and failed offloads use the default provider. |
| ChaCha20-Poly1305 | TLS record encryption and decryption when the crypto service is available | Records below the threshold and failed offloads use the default provider. |

`ENV:ZZ9K_AEAD_OFFLOAD_MIN` sets the AEAD record-size threshold in bytes. The default is 2048 and the provider reads it once per process. Set `ENV:ZZ9K_DISABLE_OFFLOAD` to disable card access for comparison or diagnosis. `ENV:ZZ9K_OFFLOAD_TIMEOUT_MS` sets the unarmed polling timeout; its default is 250 ms, and empty, invalid, or non-positive values retain that default.

## Zorro II limits

Current generation-2 2 MiB and 4 MiB Zorro II layouts provide a shared 16 KiB CPU-visible host window. The provider first reserves a persistent 32-byte `ZZ9K_ALLOC_HOST_WINDOW` probe. It does not advertise hardware algorithms when that allocation fails. Later scratch buffers use exact 16-byte-aligned growth, so a later allocation can affect one operation while other software paths remain available.

Generation-1 2 MiB and 4 MiB layouts used a 64 KiB host window. Keep firmware, bitstream, SDK payloads, and drivers in one release set, then use `zz9k-info` to inspect the acknowledged layout and host-window counters before combining clients.

## Licensing

The provider is GPL-3.0-or-later. AmiSSL and OpenSSL are Apache-2.0. The combined libraries are GPLv3, and redistribution must meet the GPLv3 source and notice requirements.

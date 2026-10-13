# Accelerated `amissl.library`

This optional replacement for AmiSSL 5.27 adds the ZZ9000 OpenSSL provider to `amissl_v362.library`. Existing AmiSSL applications keep using the AmiSSL API. When matching board and firmware services are available, the provider uses the card for selected TLS operations; all other paths use AmiSSL's default software provider.

The provider source and AmiSSL patch are in [`sdk/integration/amissl/`](https://github.com/BlitterStudio/zz9000-firmware/tree/master/sdk/integration/amissl) in the firmware repository. This directory builds and stages the two libraries for the drivers release.

## Install

1. Install AmiSSL 5.27 first. The installer requires `LIBS:AmiSSL/amissl_v362.library`.
2. Run the ZZ9000 drivers installer and select the accelerated library when it is offered.
3. Select the CPU build that matches the machine. `68020-40` is for 68020, 68030, 68040, and Apollo 68080; `68060` is for 68060 and Amithlon.

The installer saves the original library as `LIBS:AmiSSL/amissl_v362.library.bak` before replacing it. It creates that backup only when no `.bak` exists, so a later installation does not overwrite the original. To revert, delete the accelerated `amissl_v362.library` and rename the backup.

A `68020-40` library runs on a 68060, but 64-bit multiplies are trap-emulated there. Use the `68060` library on a 68060.

## Build

Run this from the repository root:

```sh
make amissl
```

`amissl/build.sh` builds the `zz9000-amissl-adtools` image, obtains the firmware repository at the ref in `sdk/SDK_REF` unless `ZZ9000_SDK` names an SDK tree, and invokes `sdk/integration/amissl/build.sh` in the container. `OS=os3-68020` or `OS=os3-68060` selects one target; the default builds both.

The outputs are:

| AmiSSL target | Release directory |
| --- | --- |
| `os3-68020` | `amissl/out/68020-40/amissl_v362.library` |
| `os3-68060` | `amissl/out/68060/amissl_v362.library` |

`make build-all` does not build these libraries. `tools/package-local.sh` stages either output when it exists. GitHub Actions stages both release artifacts.

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

The provider is GPL-3.0-or-later. AmiSSL and OpenSSL are Apache-2.0. The combined library is GPLv3, and redistribution must meet the GPLv3 source and notice requirements.

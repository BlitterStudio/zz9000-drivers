# Accelerated amissl.library

A drop-in `amissl.library` with the ZZ9000 crypto-offload OpenSSL provider
compiled inside it, so every AmiSSL application (AWeb3, IBrowse, mail clients)
gets hardware-accelerated TLS with no application changes. Current builds can
offload X25519, P-256 ECDHE, P-256 ECDSA certificate verification,
RSA-2048 PKCS#1/SHA-256 certificate verification, AES-GCM, and
ChaCha20-Poly1305 when the board and firmware advertise the matching service.
Other algorithms, non-P256 curves, RSA-PSS, signing, and unsupported key sizes
delegate to AmiSSL's software. When the board, the firmware crypto service, or
a specific algorithm is absent, the provider advertises nothing for that path
and operations run in AmiSSL's software exactly as stock, so the library is
safe on machines without a ZZ9000.

The integration (provider sources, AmiSSL patch, pinned AmiSSL ref) lives in
the zz9000-sdk repo under `integration/amissl/`; this component only builds
and stages the result. `build.sh` creates the adtools toolchain image
(`Dockerfile` — AmiSSL cannot be built with the sacredbanana image) and runs
the SDK's `integration/amissl/build.sh` inside it. It produces both CPU builds
AmiSSL itself ships — `amissl/out/68020-40/amissl_v362.library` (68020/030/040,
and the Apollo 68080) and `amissl/out/68060/amissl_v362.library` — and the
installer copies the one matching the host CPU to `LIBS:AmiSSL/`, mirroring
AmiSSL's own installer. The match matters: an os3-68020 library on a 68060 traps
and emulates 64-bit multiplies in software, which makes TLS crypto markedly
slower than the native os3-68060 build.

Notes:

* Requires the SDK-service ZZ9000 firmware and an existing **AmiSSL 5.27**
  installation (the versioned library name must match what `amisslmaster.library`
  loads).
* The installer automatically backs up the stock library to
  `LIBS:AmiSSL/amissl_v362.library.bak` before replacing it — but only if no
  `.bak` already exists, so re-running never overwrites the genuine original.
  To revert: delete the accelerated `amissl_v362.library` and rename the `.bak`
  back.
* The provider is GPL-3.0-or-later and AmiSSL/OpenSSL are Apache-2.0, so the
  combined binary is a GPLv3 work; redistributions carry GPLv3 obligations
  (sources are public in the two repos).
* The first build is slow (full AmiSSL + OpenSSL 3 cross-build with the
  adtools gcc).

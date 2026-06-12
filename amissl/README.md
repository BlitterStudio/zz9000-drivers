# Accelerated amissl.library

A drop-in `amissl.library` with the ZZ9000 crypto-offload OpenSSL provider
compiled inside it, so every AmiSSL application (AWeb3, IBrowse, mail clients)
gets hardware-accelerated TLS — X25519, ECDSA-P256 and RSA verify, AES-GCM and
ChaCha20-Poly1305 — with no application changes. Operations fall back to
AmiSSL's software crypto when the board, the firmware crypto service, or a
specific algorithm is absent, so the library is safe on machines without a
ZZ9000.

The integration (provider sources, AmiSSL patch, pinned AmiSSL ref) lives in
the zz9000-sdk repo under `integration/amissl/`; this component only builds
and stages the result. `build.sh` creates the adtools toolchain image
(`Dockerfile` — AmiSSL cannot be built with the sacredbanana image) and runs
the SDK's `integration/amissl/build.sh` inside it. The library lands in
`amissl/out/amissl_v362.library` and installs to `LIBS:AmiSSL/`.

Notes:

* Requires the SDK-service ZZ9000 firmware and an existing **AmiSSL 5.27**
  installation (the versioned library name must match what `amisslmaster.library`
  loads). Keep a backup of the original `LIBS:AmiSSL/amissl_v362.library`.
* The provider is GPL-3.0-or-later and AmiSSL/OpenSSL are Apache-2.0, so the
  combined binary is a GPLv3 work; redistributions carry GPLv3 obligations
  (sources are public in the two repos).
* The first build is slow (full AmiSSL + OpenSSL 3 cross-build with the
  adtools gcc).

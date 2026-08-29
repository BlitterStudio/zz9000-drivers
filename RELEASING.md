# Releasing the ZZ9000 stack

A user-facing release is **coordinated across three repos** that are tagged with
the same version (`vX.Y.Z`):

| Repo | Branch (dev) | Produces |
|------|--------------|----------|
| [`zz9000-sdk`](https://github.com/BlitterStudio/zz9000-sdk) | `sdk-v2-foundation` | `zz9k.library`, `mpega.library`, `zz9k-picture.datatype`, the OpenSSL crypto provider, CLI tools |
| [`zz9000-firmware`](https://github.com/BlitterStudio/zz9000-firmware) | `sdk-v2-mailbox` | `BOOT.bin` per board variant (the SDK-service firmware) |
| [`zz9000-drivers`](https://github.com/BlitterStudio/zz9000-drivers) | `sdk-payloads` | the Amiga **installer** that bundles the drivers + SDK payloads + accelerated `amissl.library` |

The drivers installer pins the SDK by commit via [`sdk/SDK_REF`](sdk/SDK_REF) and
builds the accelerated `amissl.library` from that pinned SDK in CI.

## Prerequisites

- **Commit *and* tag signing are required** in all three repos
  (`commit.gpgsign=true`, `tag.gpgsign=true`). Each `git commit`/`git tag` will
  prompt for the GPG passphrase. If running tooling non-interactively, unlock the
  agent first (e.g. `echo test | gpg --clearsign`), because a non-interactive
  shell cannot answer the pinentry prompt and signing will time out.
- The authenticated `gh` CLI for release verification.

## 1. Decide the version & bump it

`vX.Y.Z` is the release tag for **firmware + drivers** (keep them aligned).

| What | Where | Bump when |
|------|-------|-----------|
| Firmware version (what the card reports) | `zz9000-firmware` `ZZ9000_proto.sdk/ZZ9000OS/src/main.c` (`REVISION_MAJOR`/`REVISION_MINOR`) | every release |
| SDK mailbox ABI / service versions | `sdk_mailbox.h` / `sdk_mailbox.c` | only on a protocol change (they are a compatibility contract) |
| Driver component versions | each component's `version.h` / `*_VERSION`/`*_REVISION` / `$VER:` string | only when that component's **binary** actually changed since the last tag (`git diff --stat vX.Y.Z..HEAD`) — bumping an unchanged binary's version lies about it |
| SDK component versions (`zz9k.library`, `mpega.library`, provider, datatype) | their source (`library_vectors.h`, `mpega_resident.c`, `zz9k_provider.c`, `zz9k_picture_datatype.c`) | independent of the release number; bump when that component changed |

## 2. Release order (dependency order)

CI must be green on each dev branch before tagging. Cut the tags in this order:

1. **SDK** — merge `sdk-v2-foundation` → `master`; tag `vX.Y.Z`; push branch + tag.
2. **Drivers `SDK_REF`** — set [`sdk/SDK_REF`](sdk/SDK_REF) to the SDK release
   commit, commit, push `sdk-payloads`, and wait for green drivers CI (the `sdk`
   and `amissl` jobs rebuild against the new pin).
3. **Firmware** — merge `sdk-v2-mailbox` → `master`; tag `vX.Y.Z`; push. The
   tag push runs CI and the `release` job publishes the firmware ZIPs.
4. **Drivers** — merge `sdk-payloads` → `master`; tag `vX.Y.Z`; push. The tag
   push runs CI and the `release` job assembles + publishes the installer.

### Version-skew note for release notes (Zorro 2)

Current Zorro 2 bitstreams publish an aperture-relative layout in two
generations: generation 1 (the previously supported 64 KiB host-heap layout,
still published by older matched sets) and generation 2 (16 KiB host heap
plus the 48 KiB Z2 audio direct-ring reservation). The RTG driver validates
the descriptor against the 2 MiB or 4 MiB AutoConfig size, reserves the
framebuffer, template, host, audio, and optional PIP regions, then
acknowledges it with the descriptor's own generation token before firmware
exposes the host heap or relocated `GFXData`. Invalid or unacknowledged
descriptors fail closed, so template/PIP activity and SDK clients cannot
disagree about ownership of the top aperture; a driver update alone never
disables RTG on a generation-1 board. Descriptor-absent legacy 4 MiB
compatibility intentionally keeps the historical fixed 64 KiB `HOST_WINDOW`
path, but has no negotiated layout or PIP; legacy 2 MiB and unknown sizes
reject that path.

Releases ship the pieces bundled. Call out that Zorro 2 users must update the
bitstream/firmware, `ZZ9000.card`, SDK payloads, and accelerated AmiSSL library
together. The negotiated host heap serves audio, streamed image/DataType and
archive clients, and AmiSSL, while the 4 MiB profile also owns one fixed
224 KiB PIP source. The 2 MiB profile has no PIP, and no 8 MiB bitstream
variant is shipped.

## 3. CI notes

- All three repos run CI on every branch push (plus PRs and `workflow_dispatch`),
  with `concurrency` cancelling superseded non-tag runs.
- The `amissl` build (adtools + OpenSSL 3 for m68k) is the slow job (~30–60 min,
  per CPU variant); the adtools image and built libraries are cached.
- The drivers `release` job and the firmware `release` job are gated on `v*`
  tags and auto-generate GitHub release notes.
- `sd-boot/zzsd.device` has a hard 7424-byte ceiling enforced by
  `tools/check-release.sh`.

## 4. Post-release

- Verify the published releases: `gh release view vX.Y.Z -R BlitterStudio/<repo>`.
- Sanity-check the installer ZIP attached to the drivers release contains the
  per-CPU `Libs/AmiSSL/<cpu>/amissl_v362.library` and the SDK payloads.

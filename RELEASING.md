# Releasing the ZZ9000 stack

A ZZ9000 release publishes firmware ZIPs from
[`zz9000-firmware`](https://github.com/BlitterStudio/zz9000-firmware) and an
installer LHA from
[`zz9000-drivers`](https://github.com/BlitterStudio/zz9000-drivers). Release
the firmware first, pin that firmware in the drivers repository, then publish
the drivers installer with the same `vX.Y.Z` tag.

| Repository | Published artifact |
| --- | --- |
| `zz9000-firmware` | Firmware ZIPs, including `BOOT.bin` variants and the `sdk/` source subtree. |
| `zz9000-drivers` | An installer LHA containing the drivers, SDK runtime payloads, and accelerated `amissl.library` builds. |

## Prerequisites

- Commit and tag signing are required in both repositories
  (`commit.gpgsign=true`, `tag.gpgsign=true`). Each `git commit` and `git tag`
  prompts for the GPG passphrase. For non-interactive tooling, unlock the
  agent first (for example `echo test | gpg --clearsign`); a non-interactive
  shell can't answer the pinentry prompt, so signing times out.
- Install and authenticate the `gh` CLI for release verification.

## Procedure

1. Choose the version and prepare both repositories.

   Use `vX.Y.Z` for a normal release. Update the firmware version in
   `ZZ9000_proto.sdk/ZZ9000OS/src/main.c` (`REVISION_MAJOR` and
   `REVISION_MINOR`) and update the version of every changed driver binary.
   Update release notes for any compatibility change. If the release changes
   the Zorro 2 aperture layout, tell users to install matching firmware and
   bitstream, `ZZ9000.card`, SDK payloads, and accelerated AmiSSL together.

2. Publish the firmware release.

   Merge the intended firmware changes, push the commit, and wait for its CI
   run to pass. Create and push the tag:

   ```sh
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

   The firmware workflow builds release assets on every run. For a `v*` tag,
   it requires the complete asset set and publishes a GitHub release after its
   required jobs finish.

3. Pin the drivers repository to the tagged firmware commit.

   Resolve the commit that the firmware tag identifies, then place that
   40-character commit ID on the only line of `sdk/SDK_REF`:

   ```sh
   git -C ../zz9000-firmware rev-parse vX.Y.Z^{commit}
   ```

   Commit and push the `SDK_REF` change. Wait for the drivers CI run to pass
   before tagging the drivers repository. The `sdk` and `amissl` CI jobs build
   from this pin, and the installer job checks out the same commit to generate
   its AmiGuide manuals.

   `sdk/build.sh` and `amissl/build.sh` select an SDK tree in this order:
   `ZZ9000_SDK`, a sibling `../zz9000-firmware/sdk` checkout, then a clone at
   `sdk/work/zz9000-firmware` checked out at `SDK_REF`. The override and
   sibling checkout are used as-is. When building a release locally, ensure
   either one is at the commit recorded in `sdk/SDK_REF`.

4. Build a local installer only when it is needed for inspection.

   ```sh
   make build-all
   amissl/build.sh
   tools/check-release.sh
   tools/package-local.sh vX.Y.Z
   ```

   `make build-all` builds the driver components and SDK payloads. It does not
   build AmiSSL. `amissl/build.sh` produces the required `68020-40` and
   `68060` libraries, while `tools/package-local.sh` stages them only when
   present. The package command creates `zz9000-drivers-vX.Y.Z.lha` after its
   release check passes.

5. Publish the drivers installer.

   After the `SDK_REF` commit has passed CI, create the same tag in the drivers
   repository and push it:

   ```sh
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

   The drivers workflow rebuilds its required artifacts for the tag. Its
   `release` job assembles an installer on all workflow runs, but creates a
   GitHub release only for a `refs/tags/v*` ref.

6. Verify both published releases.

   ```sh
   gh release view vX.Y.Z -R BlitterStudio/zz9000-firmware
   gh release view vX.Y.Z -R BlitterStudio/zz9000-drivers
   ```

   Inspect the drivers LHA. It must include the SDK runtime payloads and
   `Libs/AmiSSL/68020-40/amissl_v362.library` plus
   `Libs/AmiSSL/68060/amissl_v362.library`.

## Tag and packaging rules

Both workflows run for branch pushes, pull requests, manual dispatches, and
`v*` tag pushes. A tag beginning with `v` creates a GitHub release. A tag that
contains a hyphen is marked as a prerelease. The workflows do not enforce
matching firmware and drivers tag names, so keep that coordination in the
release procedure.

`tools/check-release.sh` checks the complete local installer input set. It
also rejects `sd-boot/zzsd.device` at 7,424 bytes or larger; the maximum size
is 7,423 bytes.

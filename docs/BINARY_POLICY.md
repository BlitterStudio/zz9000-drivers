# Binary artifact policy

The repository tracks source, build scripts, icons, templates, installer metadata, and documentation. Generated AmigaOS executables, libraries, devices, cards, archives, and the populated installer drawer are ignored. GitHub Actions builds the release artifacts from source, fills the installer drawer during release assembly, and creates the release LHA.

## Build and package a local release

1. Run `make build-all` to build the standard driver and tool payloads. It also runs the RTG tests and `tools/check-release.sh --quick`.
2. Run `make amissl` when the release includes the accelerated `amissl.library`. It is intentionally separate because it builds AmiSSL and OpenSSL with the adtools toolchain.
3. Run `tools/package-local.sh [tag]`. It runs `tools/check-release.sh`, stages the built payloads in the installer layout, generates AmiGuide manuals, and writes `zz9000-drivers-[tag].lha`.

`tools/check-release.sh` checks the required source and documentation files. Without `--quick`, it also requires the standard local build outputs and rejects known generated outputs that Git tracks.

## Exceptions

Do not add a generated binary to Git. An exception requires a file that the repository cannot reproduce, an explanation beside that file, and an update to this policy. Add the corresponding ignored installer path when a new tool or payload is staged by release packaging.

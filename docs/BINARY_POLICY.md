# Binary Artifact Policy

Release binaries are built by GitHub Actions from source and copied into
the installer drawer during release assembly. The release zip is the
supported end-user distribution.

Generated AmigaOS binaries should not be tracked in git. The source tree
should contain source code, scripts, icons, templates, and documentation;
CI and local builds are responsible for producing driver and tool
payloads.

Before tagging a release, rebuild through CI or `make build-all` and run
`tools/check-release.sh`. That check fails if known build outputs are
tracked, while the full check still verifies that local release artifacts
exist.

Only keep a binary in source control when it is truly source material or
cannot be reproduced by the repo build. Document that exception beside the
file and in this policy. Add ignored installer slots for new tools so
`tools/package-local.sh` cannot accidentally stage generated payloads.

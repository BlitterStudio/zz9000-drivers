#!/bin/sh
set -eu

# Stage the zz9k.library client headers from a zz9000-sdk checkout into a
# build directory so the docker build (which mounts only this repo) can
# include them. Shared by ahi/driver/build.sh, mhi/build.sh and
# ZZTop/build-gcc.sh -- each keeps only its per-consumer grep gate.
#
# Usage: tools/stage-zz9k-headers.sh <stage-dir> <repo-root>
#
# Source conventions (mirrors sdk/build.sh): ZZ9000_SDK override, else a
# sibling checkout as-is, else clone/reuse the SDK at the pinned
# sdk/SDK_REF into sdk/work/zz9000-sdk (shared with sdk/build.sh, so
# releases compile every consumer against exactly the zz9k.library they
# package). Runs on the host before the docker re-exec; inside the
# container the staged copy is already present, so callers skip this.
stage_dir=$1
repo_root=$2

sdk_src=${ZZ9000_SDK:-}
if [ -z "$sdk_src" ] && [ -d "$repo_root/../zz9000-sdk/include/zz9k" ]; then
  sdk_src="$repo_root/../zz9000-sdk"
fi
if [ -z "$sdk_src" ] && command -v git >/dev/null 2>&1; then
  SDK_REF=$(cat "$repo_root/sdk/SDK_REF")
  SDK_REPO=${SDK_REPO:-https://github.com/BlitterStudio/zz9000-sdk.git}
  sdk_src="$repo_root/sdk/work/zz9000-sdk"
  if [ ! -d "$sdk_src/.git" ]; then
    echo ">> Cloning zz9000-sdk into $sdk_src"
    git clone "$SDK_REPO" "$sdk_src"
  fi
  echo ">> Checking out pinned ref $SDK_REF"
  git -C "$sdk_src" fetch origin 2>/dev/null || true
  git -C "$sdk_src" checkout -f "$SDK_REF"
fi
if [ -n "$sdk_src" ] && [ -d "$sdk_src/include/zz9k" ]; then
  rm -rf "$stage_dir"
  mkdir -p "$stage_dir/zz9k" \
           "$stage_dir/proto" \
           "$stage_dir/clib"
  cp -r "$sdk_src/include/zz9k/." "$stage_dir/zz9k/"
  cp -r "$sdk_src/host/include/zz9k/." "$stage_dir/zz9k/"
  cp -r "$sdk_src/amiga/include/zz9k/." "$stage_dir/zz9k/"
  cp "$sdk_src/amiga/include/proto/zz9k.h" "$stage_dir/proto/"
  cp "$sdk_src/amiga/include/clib/zz9k_protos.h" "$stage_dir/clib/"
fi
if [ ! -d "$stage_dir/zz9k" ]; then
  echo "ERROR: zz9k headers not staged. Provide a zz9000-sdk checkout as a" >&2
  echo "       sibling directory or set ZZ9000_SDK=/path/to/zz9000-sdk." >&2
  exit 1
fi

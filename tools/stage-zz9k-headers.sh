#!/bin/sh
set -eu

# Stage the zz9k.library client headers from the SDK subtree so the Docker
# builds (which mount only this repo) can include them.
#
# Since the SDK consolidation the sources live at sdk/ inside the
# zz9000-firmware repository; sdk/SDK_REF pins a firmware commit.
#
# Resolution: ZZ9000_SDK override (path to the sdk/ subtree root), else a
# sibling firmware checkout's sdk/, else clone the firmware repo into
# sdk/work/ at the pinned ref and use its sdk/.
#
# Usage: stage-zz9k-headers.sh <stage_dir> <repo_root>
stage_dir=$1
repo_root=$2

sdk_src=${ZZ9000_SDK:-}

if [ -z "$sdk_src" ] && [ -d "$repo_root/../zz9000-firmware/sdk/include/zz9k" ]; then
  sdk_src="$repo_root/../zz9000-firmware/sdk"
fi

if [ -z "$sdk_src" ] && command -v git >/dev/null 2>&1; then
  SDK_REF=$(cat "$repo_root/sdk/SDK_REF")
  SDK_REPO=${SDK_REPO:-https://github.com/BlitterStudio/zz9000-firmware.git}
  fw="$repo_root/sdk/work/zz9000-firmware"
  if [ ! -d "$fw/.git" ]; then
    echo ">> Cloning zz9000-firmware into $fw"
    git clone "$SDK_REPO" "$fw"
  fi
  echo ">> Checking out pinned firmware ref $SDK_REF"
  git -C "$fw" fetch origin 2>/dev/null || true
  git -C "$fw" checkout -f "$SDK_REF"
  sdk_src="$fw/sdk"
fi

if [ -n "$sdk_src" ] && [ -d "$sdk_src/include/zz9k" ]; then
  rm -rf "$stage_dir"
  mkdir -p "$stage_dir/zz9k" "$stage_dir/proto" "$stage_dir/clib"
  cp -r "$sdk_src/include/zz9k/." "$stage_dir/zz9k/"
  cp -r "$sdk_src/host/include/zz9k/." "$stage_dir/zz9k/"
  cp -r "$sdk_src/amiga/include/zz9k/." "$stage_dir/zz9k/"
  cp "$sdk_src/amiga/include/proto/zz9k.h" "$stage_dir/proto/"
  cp "$sdk_src/amiga/include/clib/zz9k_protos.h" "$stage_dir/clib/"
fi

if [ ! -d "$stage_dir/zz9k" ]; then
  echo "ERROR: zz9k headers not staged. Provide a firmware checkout with the" >&2
  echo "       SDK subtree (sibling directory, or ZZ9000_SDK=/path/to/sdk)." >&2
  exit 1
fi

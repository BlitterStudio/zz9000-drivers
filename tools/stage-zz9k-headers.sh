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
  mkdir -p "$stage_dir/zz9k" "$stage_dir/proto" "$stage_dir/clib" \
           "$stage_dir/inline" "$stage_dir/pragmas"
  cp -r "$sdk_src/include/zz9k/." "$stage_dir/zz9k/"
  cp -r "$sdk_src/host/include/zz9k/." "$stage_dir/zz9k/"
  cp -r "$sdk_src/amiga/include/zz9k/." "$stage_dir/zz9k/"
  cp "$sdk_src/amiga/include/proto/zz9k.h" "$stage_dir/proto/"
  cp "$sdk_src/amiga/include/clib/zz9k_protos.h" "$stage_dir/clib/"
  # zz9k.library 41 dropped these two (they held only the removed app
  # entries); copy them while an older SDK still has them.
  if [ -f "$sdk_src/amiga/include/inline/zz9k.h" ]; then
    cp "$sdk_src/amiga/include/inline/zz9k.h" "$stage_dir/inline/"
  fi
  if [ -f "$sdk_src/amiga/include/pragmas/zz9k_pragmas.h" ]; then
    cp "$sdk_src/amiga/include/pragmas/zz9k_pragmas.h" "$stage_dir/pragmas/"
  fi
  # The voice engine core, for the host model tests that run the driver's
  # records through the real engine (tools/ahi-voice-tests.sh). The firmware
  # keeps it beside the SDK subtree, not inside it. The engine includes the
  # half-band table of the stream reducer (audio_reduce_table.h, its 2x
  # oversampling decimator): every local include of audio_voice.c is staged
  # with it, and a missing one fails here instead of in the compiler.
  engine_src="$sdk_src/../ZZ9000_proto.sdk/ZZ9000OS/src"
  if [ -f "$engine_src/audio_voice.c" ] && [ -f "$engine_src/audio_voice.h" ]; then
    mkdir -p "$stage_dir/engine"
    cp "$engine_src/audio_voice.c" "$engine_src/audio_voice.h" "$stage_dir/engine/"
    local_includes=$(sed -n 's/^#include "\([^"]*\)".*$/\1/p' "$engine_src/audio_voice.c")
    # Read from a here-document, not a pipe: the exit below must end this
    # script, and a piped loop would run in a subshell.
    while IFS= read -r inc; do
      [ -n "$inc" ] || continue
      if [ ! -f "$stage_dir/engine/$inc" ]; then
        if [ ! -f "$engine_src/$inc" ]; then
          echo "ERROR: audio_voice.c includes $inc, which is not beside it in the SDK checkout." >&2
          exit 1
        fi
        cp "$engine_src/$inc" "$stage_dir/engine/"
      fi
    done <<EOF
$local_includes
EOF
  fi
fi

if [ ! -d "$stage_dir/zz9k" ]; then
  echo "ERROR: zz9k headers not staged. Provide a firmware checkout with the" >&2
  echo "       SDK subtree (sibling directory, or ZZ9000_SDK=/path/to/sdk)." >&2
  exit 1
fi

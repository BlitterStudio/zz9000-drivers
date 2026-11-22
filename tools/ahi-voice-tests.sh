#!/bin/sh
set -eu

# Host tests for the AHI hardware-mixing channel model (ahi/driver/voice_model.c).
#
# voice_model_test drives AHI call sequences through the model and checks the
# records. voice_ref_test runs the model's records through the real voice
# engine (the firmware's audio_voice.c, staged with the pinned SDK headers by
# tools/stage-zz9k-headers.sh) and compares every voice, every output frame,
# against an AHI-semantics frame stepper.
#
# The tests need only a C compiler. Without one on the PATH the script runs
# them in the zz9-host-tests Docker image, with this repository mounted.
#
# Environment: ZZ9000_SDK (an sdk/ subtree root) as for the other staging
# builds; CC and CFLAGS override the compiler; AHI_VOICE_SEEDS is the number of
# random sequences the differential test runs (default 200).

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
driver_dir=ahi/driver
stage=$driver_dir/voice-test-stage
build=$driver_dir/voice-test-build

if [ "${1:-}" != "--staged" ]; then
  "$script_dir/stage-zz9k-headers.sh" "$repo_root/$stage" "$repo_root"
  if [ ! -f "$repo_root/$stage/engine/audio_voice.c" ]; then
    echo "ERROR: the SDK checkout has no voice engine source" >&2
    echo "       (ZZ9000_proto.sdk/ZZ9000OS/src/audio_voice.c beside sdk/)." >&2
    exit 1
  fi
  if ! command -v "${CC:-cc}" >/dev/null 2>&1; then
    mount=$repo_root
    case "$(uname -s)" in
      MINGW*|MSYS*|CYGWIN*)
        mount=$(cygpath -w "$repo_root")
        export MSYS_NO_PATHCONV=1
        ;;
    esac
    exec docker run --rm -v "$mount":/src -w /src \
      -e AHI_VOICE_SEEDS="${AHI_VOICE_SEEDS:-200}" -e AHI_VOICE_MONO="${AHI_VOICE_MONO:-}" \
      zz9-host-tests:latest sh tools/ahi-voice-tests.sh --staged
  fi
fi

cd "$repo_root"
cc=${CC:-cc}
cflags=${CFLAGS:--O2 -g}
rm -rf "$build"
mkdir -p "$build"

# shellcheck disable=SC2086
$cc -std=c99 $cflags -Wall -Wextra -Werror -I"$stage" -I"$driver_dir" \
  -o "$build/voice_model_test" \
  "$driver_dir/voice_model.c" "$driver_dir/voice_model_test.c"
"$build/voice_model_test"

# shellcheck disable=SC2086
$cc -std=c99 $cflags -Wall -Wextra -Werror -I"$stage" -I"$stage/engine" \
  -I"$driver_dir" -o "$build/voice_ref_test" \
  "$driver_dir/voice_model.c" "$driver_dir/voice_ref_test.c"
# AHI_VOICE_MONO=1 skips the stereo views (an engine that predates them).
"$build/voice_ref_test" "${AHI_VOICE_SEEDS:-200}" ${AHI_VOICE_MONO:+mono}

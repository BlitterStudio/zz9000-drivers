#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
status=0

cd "$repo_root"

make -C rtg/tests test
python3 -m unittest tools/tests/test_repo_tooling.py

if command -v shellcheck >/dev/null 2>&1; then
    shellcheck \
        tools/amiga-docker.sh tools/build-all.sh tools/package-local.sh \
        tools/check-release.sh tools/check-quality.sh \
        rtg/build.sh net/build.sh usb-poseidon/build.sh sd-boot/build.sh \
        ZZTop/build-gcc.sh ax-direct/build-gcc.sh ax-direct/build-axtest.sh \
        ahi/driver/build.sh ahi/axtest/build-gcc-single.sh mhi/build.sh \
        ZZDiag/build.sh || status=$?
else
    echo "SKIP: shellcheck not installed"
fi

if command -v actionlint >/dev/null 2>&1; then
    actionlint .github/workflows/ci.yml || status=$?
else
    echo "SKIP: actionlint not installed"
fi

if command -v cppcheck >/dev/null 2>&1; then
    cppcheck --enable=warning,style,performance,portability \
        --error-exitcode=1 --suppress=missingIncludeSystem \
        -Iinclude -Irtg rtg/tests/register_cache_test.c ZZDiag/ZZDiag.c || status=$?
else
    echo "SKIP: cppcheck not installed"
fi

exit "$status"

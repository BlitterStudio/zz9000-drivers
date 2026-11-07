#!/bin/sh
# Native m68k check of amiga_stack_run(): build it the way ZZFwUpdate and
# ZZTop do and run it under vamos with less stack than it asks for, so the
# private-stack swap and the swap back both execute. Host tests cannot see
# this: GCC 16 omits the frame pointer and once addressed the swap structure
# relative to the swapped-in stack.
#
# Needs the m68k toolchain and vamos; re-execs in the Amiga toolchain
# container (AMIGA_IMAGE) when they are not on PATH.
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
    exec "$script_dir/../../tools/amiga-docker.sh" common/tests \
        ./amiga_stack_m68k.sh "$@"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
m68k-amigaos-gcc -m68020 -O2 -noixemul -Wall -Wextra -Werror \
    -I"$script_dir/.." -I"$script_dir/../../include" \
    -o "$tmp/stackrun" "$script_dir/amiga_stack_m68k.c" \
    "$script_dir/../amiga_stack.c" -lamiga

(cd "$tmp" && vamos -C 68020 -m 8000 -s 8 stackrun) >"$tmp/out" 2>&1 || {
    cat "$tmp/out" >&2
    echo "FAIL: stackrun did not exit cleanly under vamos" >&2
    exit 1
}
cat "$tmp/out"
grep -q 'entry ran on a private stack and returned 37' "$tmp/out"

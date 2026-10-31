# ZZRun

`ZZRun` loads one trusted ARM app into the ZZ9000 app runtime. It is the v0
launcher for GLQuake. It needs a Zorro III board, `zz9k.library`, app-runtime
firmware, and Picasso96 when the app asks for a display.

The launcher is a Shell command and a Workbench tool. It runs one app at a
time; firmware rejects a second load while one is active.

## Starting an app

Give the ELF first. Remaining Shell tokens become the app's `argv`; `argv[0]`
is not added by ZZRun.

```text
Stack 32768
ZZRun RAM:glquake.elf +timedemo demo1
```

`ROOT` controls files requested by the app. A Shell launch defaults to the
current directory. A Workbench launch defaults to the directory holding the
ZZRun icon. For Quake, set `ROOT` to the directory containing `id1/`.

On normal app exit, ZZRun returns the app exit code. It returns 20 after an
app fault, an unavailable runtime, display failure, or launcher error. It
returns 10 for invalid launcher arguments.

## Workbench ToolTypes

| ToolType | Meaning |
| --- | --- |
| `APP` | ELF path when ZZRun is started without a project file. |
| `ROOT` | Root directory for relative app file paths. |
| `ABS=1` | Permit app paths containing an Amiga volume or assign colon. The root check does not apply to those paths. |
| `MODE` | Optional Picasso96 DisplayID, in decimal, `$` hexadecimal, or `0x` hexadecimal. The app still chooses width, height, and RGB format. |
| `ARGS` | Optional space-separated arguments. Quoting is not parsed. |

Dragging an ELF project onto the ZZRun icon takes precedence over `APP`.

## Runtime protocol walk-through

ZZRun uses the existing `zz9k.library` mailbox. It does not add a doorbell or
another shared-memory convention.

1. It finds the board through `expansion.library`, opens `zz9k.library`, and
   calls `APP_QUERY`. Zorro II has no app runtime: query is unsupported and
   ZZRun stops before allocating a window.
2. `APP_ALLOC_WINDOW` returns an ARM address, size, and `host_offset`. ZZRun
   maps the rings at `board_base + host_offset`; it does not assume a board
   offset. The returned block is the fixed 384 KiB `ZZAPP_WINDOW_BYTES`
   layout in `zzapp_proto.h`.
3. `APP_LOAD_BEGIN` reserves the app arena. ZZRun streams the ELF through
   `ZZAPP_STAGING_OFF` in chunks no larger than `ZZAPP_STAGING_BYTES`, posting
   `APP_LOAD_CHUNK` with the file offset, byte count, and that staging offset.
   `APP_LOAD_END` asks firmware to validate and relocate the image.
4. ZZRun packs `argc`, argument offsets, and strings in `ZZAPP_ARGV_OFF`, then
   calls `APP_START`. Strings are capped at 127 bytes plus NUL; the complete
   packed area must fit `ZZAPP_ARGV_BYTES`.
5. While the app runs, every poll writes `heartbeat_ms`, drains firmware log
   records, forwards input, services one file request, binds a requested P96
   screen, and acknowledges completed flips. `APP_STATUS` is called when the
   app ends or faults.
6. Ctrl-C or the close gadget writes the lifecycle `stop` word, enqueues one
   `ZZ_EV_STOP`, and posts `APP_STOP`. ZZRun remains alive for the 500 ms
   grace period so firmware can publish its final state. It closes the custom
   screen and returns the public screen even after a fault.

All mailbox payload and rings fields are big-endian. ZZRun stages the firmware
`zzapp_proto.h` header at build time; that header is the layout authority.

### Input, display, and logs

ZZRun forwards IDCMP raw keys without converting them to ASCII. A key-up has
`ZZ_EV_KEY_UP`; its raw key code has the Amiga up bit removed. Key qualifiers
stay in `buttons`. Mouse movement carries deltas, and mouse buttons use bits
0, 1, and 2 for left, right, and middle. `lowlevel.library` is optional;
when present, joystick events use the same three button bits and `dx`/`dy` in
`-1, 0, 1`.

The launcher owns the input producer side. It drops a new event rather than
blocking when all 256 slots are full, and increments `input_drops`. Firmware
owns the log producer side. ZZRun consumes its padded log records and prints
them on stdout.

When firmware sets the display-unbound flag, ZZRun opens the requested P96
screen. RGB565 is the first path; BGRA8888 is accepted when P96 returns that
format. ZZRun binds the bitmap's actual pitch and card addresses with
`APP_DISPLAY_BIND`. Firmware clears the flag only after a successful bind.
With two displayable P96 bitmaps, each completed flip changes screen buffer;
with one, ZZRun only waits for vertical sync when requested and acknowledges
the generation.

### Files and ROOT

The file slot has one outstanding request. Firmware writes it under the
seqlock and finishes at an even request sequence. ZZRun responds at
`request_seq + 2`: it first writes the odd sequence, then `err`, `out_n`,
`out_pos`, `mode`, `mtime`, and the same file or list id, then commits the
next even sequence. Read and list-next bytes are written to `ZZAPP_DATA_OFF`;
a list name is NUL-terminated and `out_n` excludes the NUL. A short read with
`err == 0` is EOF.

Relative paths stay below `ROOT`. `..`, leading `/`, and empty path components
that would leave the root are rejected. Paths containing `:` are rejected
unless `ABS=1`. This is path hygiene for an app that is already trusted; it is
not an isolation boundary. File reads and writes are chunked at 8 KiB so the
launcher refreshes its heartbeat while serving a large request. A stalled
AmigaDOS volume can still block its DOS call.

The launcher reports file modification times as Unix seconds derived from an
Amiga `DateStamp`, or zero when the time is absent.

## Faults and cleanup

A data abort, prefetch abort, undefined instruction, watchdog, or
watchdog-plus-restart prints the captured pc, sp, lr, far, fsr, cpsr, exit
code, and generation, then returns 20. A forced stop caused by Ctrl-C or the
close gadget returns the app's normal exit code when it has one. Every exit
path closes opened files and directory handles, restores the public screen,
and releases its Amiga-side resources.

## Building

Use the repository wrapper. It stages the zz9k client headers and the current
`app/services` `zzapp_proto.h`, then uses GCC 6.5.0b in
`sacredbanana/amiga-compiler:m68k-amigaos` when no local cross compiler is on
`PATH`.

```sh
cd ZZRun
AMIGA_IMAGE=sacredbanana/amiga-compiler:m68k-amigaos ./build.sh
```

Set `ZZ9000_FIRMWARE` to a firmware checkout when it is not a sibling of this
repository. The checkout must contain the app-runtime services header.

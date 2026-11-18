# ZZRun

`ZZRun` loads a trusted ARM app into the ZZ9000 app runtime. It needs a
Zorro III board, app-runtime firmware that advertises the slots capability
(ABI 0.3), and Picasso96 when the app asks for a display. On firmware without
slots it prints `ZZRun requires app-runtime firmware with app slots.` and
returns 20.

ZZRun opens `zz9k.library` at version 0 for mailbox transport. It calls
`ZZ9KCall`, `ZZ9KCallAsync`, `ZZ9KPoll`, and `ZZ9KCancelAsync`, so it needs
library revision 2 or later. The current drivers release includes revision 40.

The launcher is a Shell command and a Workbench tool. Every instance claims its
own app slot, so up to four apps can run at once.
When every slot is claimed, a new launch reports `window allocation failed
(busy)` and returns 20.

## Starting an app

Run `Stack 32768` in the launching Shell first. ZZRun requires at least a
16 KiB process stack.

```text
ZZRun <app.elf> [MAXMEM=<MiB>] [HOSTMEM=<MiB>] [AUDIO=AHI] [args]
```

Give the ELF first. The app's `argv[0]` is the ELF's file name and the
remaining Shell tokens follow it, as C programs expect (up to 15 arguments).
`AUDIO=AHI`, `MAXMEM=<MiB>`, and `HOSTMEM=<MiB>` are recognized anywhere after
the ELF, so they cannot be passed to the app as arguments with those exact
forms.

```text
Stack 32768
ZZRun RAM:swquake.elf +timedemo demo1
```

`MAXMEM=<MiB>` caps the card memory committed by this app. The value must be a
positive decimal MiB count. For example:

```text
ZZRun RAM:swquake.elf MAXMEM=256 +timedemo demo1
```

`HOSTMEM=<MiB>` sets how much host-visible card memory this app may allocate
(the memory the Amiga can address through the board window). The value must be
a positive decimal MiB count up to 4095; anything else prints the usage and
returns 10. Without it the app gets the firmware default of 16 MiB. The limit
is a cap, not a reservation: nothing is set aside at load, and an allocation
can still fail below the cap while the card's shared heap is busy. 62 MiB is
the per-app host-visible alias-window ceiling; the firmware grants at most the
smaller of that and what the shared heap can hold, and refuses the rest at
`APP_LOAD_BEGIN`. Above 62 MiB ZZRun prints `HOSTMEM too large` and returns 20.
At or below it the firmware's answer is the same "no memory" status it gives
when the loader cannot allocate its staging arena, so ZZRun prints `not enough
card memory to load` with the requested value and returns 20. Firmware that
does not advertise `ZZAPP_CAP_HOST_LIMIT` would ignore the limit, so ZZRun
prints `ZZRun: HOSTMEM requires firmware with host-visible limits.` and returns
20 before loading. `HOSTMEM` and `MAXMEM` can be combined.

```text
ZZRun RAM:swquake.elf HOSTMEM=32
```

`ROOT` controls files requested by the app. A Shell launch defaults to the
current directory. A Workbench launch defaults to the directory holding the
ZZRun icon. For Quake, set `ROOT` to the directory containing `id1/`.

A relative ELF path is opened from ZZRun's current AmigaDOS directory. A path
with a volume or assign prefix is resolved through `GetDeviceProc`; on a
multi-directory assign, ZZRun tries each directory after an
`ERROR_OBJECT_NOT_FOUND` result.

On normal app exit, ZZRun returns the app exit code. It returns 20 after an
app fault, an unavailable runtime, display failure, a console that cannot be
opened or written, an AHI route that cannot be opened, or another launcher
error. It returns 10 for invalid launcher arguments. Ctrl-C while ZZRun opens
or loads the ELF stops the load and returns 5, including when a DOS packet
wait such as a file read or closing the ELF consumed the break. Other load
failures return 20.

## Runtime status

```text
ZZRun STATUS
```

`STATUS` is case-insensitive. It prints one line for each
slot with its state, app name, generation, committed and capped MiB, CPU
seconds, block reason, and terminal reason. The block reason is `none`, `file`,
`console`, `event`, `sleep`, `vsync`, or `audio`. `event` means the app is
waiting in `zz_msg_wait`; firmware before app API 0.4 never reports it.

An interrupted load is shown as `state stopping ... reason quarantined`. Its
app memory has been reclaimed, but the window stays reserved so a late DOS
`Read` from the old launcher cannot corrupt a new launch. ZZRun returns the
stale ownership result when that old read finishes; firmware records it even
while teardown is in progress and frees the window immediately afterwards.
When the old launcher cannot return, confirm that it is gone and run:

```text
ZZRun RELEASE <slot>
```

`RELEASE` accepts slots 0 through 3. It succeeds only for a fully torn-down
quarantined slot and refuses every other state. A card reset also clears the
reservation.

## Workbench ToolTypes

| ToolType | Meaning |
| --- | --- |
| `APP` | ELF path when ZZRun is started without a project file. |
| `ROOT` | Root directory for relative app file paths. |
| `ABS` | Enabled when the ToolType has no value, is exactly `1` or `y`, or begins with `ye` or `t` (case-insensitive). Permits app paths containing an Amiga volume or assign colon. The root check does not apply to those paths. |
| `MODE` | Optional Picasso96 DisplayID, in decimal, `$` hexadecimal, or `0x` hexadecimal. The app still chooses width, height, and RGB format. |
| `ARGS` | Optional space-separated arguments. Quoting is not parsed. |
| `JOYPORT0` | Enabled by the same values as `ABS`. Enables joystick port 0 as well as port 1. |
| `AUDIO=AHI` | Route app PCM to AHI. The default remains the card audio fabric. |
| `MAXMEM=<MiB>` | Positive decimal MiB cap for the app's `APP_LOAD_BEGIN` request. |
| `HOSTMEM=<MiB>` | Positive decimal MiB (1 to 4095) host-visible memory limit for the app's `APP_LOAD_BEGIN` request. Refused when the firmware lacks `ZZAPP_CAP_HOST_LIMIT`; firmware refuses more than its per-app ceiling of 62 MiB (`HOSTMEM too large`, return 20); a refusal at or below 62 MiB reports `not enough card memory to load`, return 20. |


Dragging an ELF project onto the ZZRun icon takes precedence over `APP`.

## Runtime protocol walk-through

ZZRun uses the existing `zz9k.library` mailbox. It does not add a doorbell or
another shared-memory convention.

1. It finds the board through `expansion.library`, opens `zz9k.library`, and
   calls the extended `APP_QUERY`. Zorro II has no app runtime: query is
   unsupported and ZZRun stops before allocating a window. The `caps` value on
   the `runtime abi` line it prints is the extended reply's: `000001ff` when
   the firmware offers every bit through `ZZAPP_CAP_HOST_LIMIT`, where the
   legacy reply showed at most `0000003f`. Firmware without slots is refused.
2. `APP_ALLOC_WINDOW` atomically claims a free slot and returns its ARM address,
   size, host offset, slot, and generation. ZZRun maps the rings at
   `board_base + host_offset`; it does not assume a board offset. The returned
   block is the fixed 592 KiB `ZZAPP_WINDOW_BYTES` layout in `zzapp_proto.h`.
3. `APP_LOAD_BEGIN` reserves the app arena and carries the optional `MAXMEM`
   cap and, with `ZZAPP_CAP_HOST_LIMIT`, the optional `HOSTMEM` limit. ZZRun
   refuses a `HOSTMEM` on firmware without that capability before it claims a
   slot. A launch without either leaves both words zero, which means no
   launcher cap and the 16 MiB host-visible default.
   ZZRun streams the ELF through `ZZAPP_STAGING_OFF` in chunks no larger
   than `ZZAPP_STAGING_BYTES`, posting `APP_LOAD_CHUNK` with the file offset,
   byte count, and that staging offset. ZZRun refreshes the claim heartbeat
   around `Open` and `Seek`; its DOS reads are asynchronous, so a slow device
   read keeps the loading lease alive and detects ownership loss while it is
   outstanding. `APP_LOAD_END` asks firmware to validate and relocate the
   image.

Every subsequent app mailbox request carries that slot and generation selector,
so one launcher cannot affect another app.

4. ZZRun packs `argc`, argument offsets, and strings in `ZZAPP_ARGV_OFF`, then
   calls `APP_START`. Strings are capped at 127 bytes plus NUL; the complete
   packed area must fit `ZZAPP_ARGV_BYTES`.
5. ZZRun advertises its longer ten-second main-loop lease while retaining the
   legacy `heartbeat_ms` write on every poll. Older firmware ignores that
   capability bit and retains its two-second lease. The main loop drains
   firmware log records and app console output, fills app console input when
   data is ready, forwards input, services one file request, binds a requested
   P96 screen, loads changed CLUT8 palette entries, acknowledges completed
   flips, and when extended `APP_QUERY` advertises the low-memory capability,
   reports each changed crossing count as `ZZRun: card memory low (N)`.
   `APP_STATUS` is called when the app ends or faults.
6. Ctrl-C from the Shell or an app-display raw key requests a stop: ZZRun
   writes the lifecycle `stop` word, enqueues one `ZZ_EV_STOP`, and posts
   `APP_STOP` carrying the generation it owns. Firmware rejects a stop whose
   generation does not match, so a stale stop cannot abort a newer load.
   Ctrl-D, Ctrl-E, and Ctrl-F on the app display remain normal raw-key events
   and additionally enqueue `ZZ_EV_BREAK` with codes 4, 5, and 6. The app
   display has one borderless input window, not a close gadget; its raw keys
   are forwarded to the app, including Ctrl-C and the app's own quit controls.
   ZZRun remains alive for the 500 ms grace period so firmware can publish its
   final state. Ctrl-C ends ZZRun after that grace, plus a second 500 ms,
   even if console output is still queued. It closes the custom screen and
   returns the public screen even after a fault.

All mailbox payload and rings fields are big-endian. ZZRun stages the firmware
`zzapp_proto.h` header at build time; that header is the layout authority.

### Input, display, and logs

ZZRun forwards IDCMP raw keys without converting them to ASCII. A key-up has
`ZZ_EV_KEY_UP`; its raw key code has the Amiga up bit removed. Key qualifiers
stay in `buttons`. Mouse movement carries deltas, and mouse buttons use bits
0, 1, and 2 for left, right, and middle. Joystick events report one fire
button in bit 0 and `dx`/`dy` in `-1, 0, 1`. ZZRun reads joystick port 1 only
while its display window is active; `JOYPORT0=1` also enables port 0. It only
reads `JOYxDAT` and the CIA-A fire bits and never drives the port pins, so the
mouse and joystick ports are left exactly as it found them.

The launcher owns the input producer side. It drops a new event rather than
blocking when all 256 slots are full, and increments `input_drops`. Firmware
owns the log producer side. ZZRun stages log records and its own runtime
status through the same asynchronous writer as app console output. If a
stalled destination fills the bounded staging queue, ZZRun drops diagnostic
log records but reserves space for launcher status and errors; after pending
writes finish during cleanup it reports the number of dropped records. ZZRun
does not write to stderr while the app is running.

### Console and audio routes

App `stdout` and `stderr` use the dedicated non-lossy console-output ring, not
the diagnostic `zz_log` ring. A Shell launch writes those bytes through
`Output()` and takes app stdin from `Input()`. EOF from that handle is published
to the app after all input bytes. A redirected input (`<file` or a pipe) ends
the app's stdin at the end of that stream. From the keyboard, the app gets
whatever ends a `Type *` in the same console; on OS 3.2.3 with the standard or
CCON handler Ctrl-\ does not, so stop such an app with Ctrl-C. ZZRun starts a
keyboard read only while the app it owns is running and has asked for input.
If the app is waiting for keyboard input when it stops, ZZRun writes
`press Return to finish.` to the console that owns that read (the Workbench
`CON:` window, or `Input()`), not to redirected stdout: the console cannot
cancel the line read it has open. A pipe or other non-interactive input uses
the same asynchronous read, so an open writer cannot stall heartbeat, display,
or audio. A regular file completes immediately. If a non-interactive read is
still pending at exit, ZZRun writes `waiting for pending input to finish.` to
the user console (not stdout, and not the pipe) and waits, because a pipe read
cannot be cancelled. The custom screen is closed before either of those waits,
so the prompt is not hidden behind the app display. A Workbench launch opens
no console by default; only when the app first uses console I/O does firmware
set its console flag, causing ZZRun to open a closable `CON:` window. If
opening that requested console fails, or if writing app output fails, ZZRun
stops the app, does not wait for the output ring to empty, and returns 20.

The default audio route is the card fabric. Set `AUDIO=AHI` in the app icon or
pass `AUDIO=AHI` after the ELF in a Shell command to request the AHI route; it
is advertised to firmware before the app starts.
The shared PCM ring is interleaved S16LE and carries its requested rate and
channel count. Each successful firmware audio open publishes a stream epoch in
the audio header. If that epoch changes, ZZRun aborts and reaps its previous
AHI requests without retiring their bytes onto the new stream, then accepts
the new head, tail, and rate. ZZRun accepts the route only when AHI grants the
exact requested stereo rate; if AHI cannot be opened, ZZRun stops the app and
returns 20. It converts the ring's S16LE samples into AHI's
S16BE bounce buffer without resampling or downmixing. AHI underruns are
reported in the audio ring/lifecycle counters.

Known limitation: running an app with stdout redirected to `NIL:` (`>NIL:`)
made swquake's file seeks fail on the card with every ZZRun build tested,
while `>file` works. This is under investigation.

When firmware sets the display-unbound flag, ZZRun opens the requested P96
screen. RGB565 is the first path; CLUT8 is supported and ZZRun loads changed
palette entries from the lifecycle window; BGRA8888 is accepted when P96
returns that format. ZZRun binds the bitmap's actual pitch and card addresses
with `APP_DISPLAY_BIND`. Firmware clears the flag only after a successful bind.
With two displayable P96 bitmaps (`AllocScreenBuffer`), each completed flip
changes screen buffer. If P96 refuses screen buffers, ZZRun first opens a
bitmap three times the requested height and pans between its pages. If that
fails, it retries with a double-height bitmap. For address-based scanout flips,
firmware acknowledges after card vblank; if that acknowledgement has not
arrived within 100 ms, ZZRun uses `WaitTOF` for the rest of the run. In mailbox
mode, firmware chooses the scanout frame and ZZRun does not pan. Only if both
panning layouts fail does it bind one bitmap, which the firmware copies each
frame into.

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
unless `ABS` is enabled. With `ABS`, file open, stat, and directory-list
requests use `GetDeviceProc` and try later multi-directory assign candidates
after `ERROR_OBJECT_NOT_FOUND`. This is path hygiene for an app that is already
trusted; it is not an isolation boundary. File reads and writes are chunked at
8 KiB so the launcher refreshes its heartbeat while serving a large request. A
stalled AmigaDOS volume can still block its DOS call.

The launcher reports file modification times as Unix seconds derived from an
Amiga `DateStamp`, or zero when the time is absent.

## Faults and cleanup

A data abort, prefetch abort, undefined instruction, watchdog, or
watchdog-plus-restart prints the captured pc, sp, lr, far, fsr, cpsr, exit
code, and generation, then returns 20. A forced stop caused by Ctrl-C returns
the app's normal exit code when it has one. Every exit path closes opened files
and directory handles, restores the public screen, and releases its Amiga-side
resources. ZZRun inhibits caching on the shared window and does not restore
the previous cache mode; the window stays cache-inhibited.

## Building

Use the repository wrapper. It stages the zz9k client headers and the current
`app/services` `zzapp_proto.h`, then builds in the repository's GCC 16 image
(`tools/amiga-docker.sh`) when no local cross compiler is on `PATH`.

```sh
cd ZZRun
./build.sh
```

Set `ZZ9000_FIRMWARE` to a firmware checkout when it is not a sibling of this
repository. The checkout must contain the app-runtime services header, and that
header must define `ZZAPP_CAP_HOST_LIMIT` and `ZZAPP_LOAD_HOST_VISIBLE_MIB`;
a checkout from before the host-limit change does not compile.

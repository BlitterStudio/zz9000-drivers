# ZZRun

`ZZRun` loads a trusted ARM app into the ZZ9000 app runtime. It needs a
Zorro III board, firmware with the AAR queue transport (app ABI 0.5), and
Picasso96 when the app asks for a display. It needs `zz9k.library` revision 36
or later (only to reach the board's mailbox) and `mmu.library` 41 or later,
because the card's shared buffers must be mapped cache-inhibited. Without the
firmware it prints `ZZRun requires a Zorro III board and app-runtime
firmware.` and returns 20.

The launcher is a Shell command and a Workbench tool. Every instance claims its
own app, so many apps can run at once; the card's shared memory is the limit.
When the card cannot admit another one, a launch reports `claim refused:` with
the reason (not enough card memory, no translation tables left, not enough
shared memory for the queues) and returns 20.

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

`HOSTMEM=<MiB>` sets how much shared (host-visible) card memory this app may
allocate: the memory the Amiga can address through the board, including the
buffers the runtime allocates for the app's console, files and audio. The value
must be a positive decimal MiB count up to 4095; anything else prints the
usage and returns 10. Without it the app gets the firmware default of 16 MiB.
The limit is a cap, not a reservation. Both limits are written to the claim's
control block before the load; the card reads them at load begin and refuses
what it cannot grant. ZZRun then prints `load begin refused:` with the reason
(a platform limit means `HOSTMEM` or `MAXMEM` is too large) and returns 20.

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
app fault, a watchdog, lease loss or any other end the platform imposed, an
unavailable runtime, a revoked claim, display failure, a console that cannot be
opened or written, an AHI route that cannot be opened, or another launcher
error. It returns 10 for invalid launcher arguments. Ctrl-C while ZZRun opens
or loads the ELF stops the load and returns 5, including when a DOS packet
wait such as a file read or closing the ELF consumed the break. Other load
failures return 20.

## Runtime status

```text
ZZRun STATUS
```

`STATUS` is case-insensitive. It prints one line for each claim the card
holds: its handle, state, app name, committed MiB, CPU seconds, block reason
and end reason. The block reason is `none`, `file`, `console`, `event`,
`sleep`, `vsync`, or `audio`. `event` means the app is waiting for a message.

A claim whose launcher disappeared without releasing it (a crashed ZZRun, or
a launcher that lost its lease) keeps its control block and buffers on the card
until it is released, because the old launcher may still write through its
mapping. `STATUS` shows it as ended, with end reason `lease loss` or
`quarantined`. When the old launcher is gone, release it by its handle:

```text
ZZRun RELEASE <handle>
```

The handle is the hexadecimal number `STATUS` prints, written as `$` or `0x`
followed by the digits, or in decimal. `RELEASE` ends a claim that is finished
(an app that ended or faulted, or a claim the card revoked) and refuses one
whose app is running, loading or being claimed. A card reset also clears every
claim.

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
| `MAXMEM=<MiB>` | Positive decimal MiB cap, written to the claim's private memory limit. |
| `HOSTMEM=<MiB>` | Positive decimal MiB (1 to 4095) shared memory limit, written to the claim's shared memory limit. |

Dragging an ELF project onto the ZZRun icon takes precedence over `APP`.

## How ZZRun talks to the card

ZZRun links the client transport module of the ZZ9000 SDK
(`aar_xport_zz9000`), which implements the AAR transport contract
(`aar/transport.h`) for this board. Nothing is shared by fixed addresses any
more: every claim has its own control block and queue pair, and the card names
each buffer it wants ZZRun to use in the request that needs it.

1. ZZRun finds the board through `expansion.library`, opens `zz9k.library` and
   `mmu.library`, and calls `APP_QUERY`. Zorro II has no app runtime: the query
   is unsupported and ZZRun stops before claiming.
2. `CLAIM` admits an app. The card returns a handle and the control block of
   the claim; ZZRun maps it cache-inhibited, checks its magic and version, and
   binds the submission queue (requests and logs from the app) and completion
   queue (ZZRun's replies and input events) from the control block's
   descriptors.
3. ZZRun writes `MAXMEM` and `HOSTMEM` to the control block, then `LOAD_BEGIN`
   announces what ZZRun can serve (CLUT8 screens, pan acknowledgement,
   card-owned flips, the longer lease, AHI). The card allocates a load buffer;
   ZZRun reads the ELF into its 64 KiB staging area and posts `LOAD_CHUNK`
   for each piece, then `LOAD_END` has the card validate and relocate the
   image. ZZRun's DOS reads are asynchronous, so a slow device keeps the
   loading lease alive and a revoked claim is noticed while a read is
   outstanding.
4. ZZRun hands the argument strings to `START`. They travel in the load
   buffer; strings are capped at 127 bytes plus NUL.
5. The main loop wakes every millisecond. Each pass it renews the lease (one
   store of the control block's lease counter; the lease is two seconds, or ten
   with the capability ZZRun sends), forwards input, serves the requests the
   app has posted, completes the ones that were waiting for DOS, the display or
   AHI, and reads the app's state from the control block. A completion is
   followed by a doorbell so the card looks at the queue at once, but the queue
   indices stay authoritative: a lost doorbell costs latency, never data.
6. Ctrl-C from the Shell or an app-display raw key requests a stop: ZZRun
   writes the control block's stop request, posts one stop event (retried
   until the queue has room for it), and sends `STOP`. Ctrl-D, Ctrl-E, and
   Ctrl-F on the app display remain normal raw-key events and additionally
   post a break event with codes 4, 5, and 6. The app display has one
   borderless input window, not a close gadget; its raw keys are forwarded to
   the app, including Ctrl-C and the app's own quit controls. ZZRun remains
   alive for the 500 ms grace period so the card can publish the final state.
   Ctrl-C ends ZZRun after that grace, plus a second 500 ms, even if console
   output is still queued. It closes the custom screen and returns the public
   screen even after a fault.
7. When the app ends, ZZRun reads the exit code, end reason and fault record
   from the control block, prints them, and releases the claim. The release
   returns the control block, the queues and every buffer to the card. If the
   card revokes the claim first (lease lost), every call answers `claim
   revoked`, ZZRun stops serving and returns 20.

All words in the control block and in queue entries are big-endian. ZZRun
stages `zzapp_proto.h`, the `aar/` headers and the module source at build time;
they are the layout authority.

### Input, display, and logs

ZZRun forwards IDCMP raw keys without converting them to ASCII. A key-up has
`AAR_EV_KEY_UP`; its raw key code has the Amiga up bit removed. Key qualifiers
stay in `buttons`. Mouse movement carries deltas, and mouse buttons use bits
0, 1, and 2 for left, right, and middle. Joystick events report one fire
button in bit 0 and `dx`/`dy` in `-1, 0, 1`. ZZRun reads joystick port 1 only
while its display window is active; `JOYPORT0=1` also enables port 0. It only
reads `JOYxDAT` and the CIA-A fire bits and never drives the port pins, so the
mouse and joystick ports are left exactly as it found them.

Events travel on the completion queue, which holds 64 entries shared with
completions and messages. The module sums consecutive mouse motion into one
pending event, so a flood costs one entry; every other event keeps its order
behind the motion that came before it. An event that finds the queue full is
dropped and counted in the control block's events-dropped word, except the stop
event, which is retried until it is delivered.

The card posts the app's log records on the submission queue. ZZRun stages them
and its own runtime status through the same asynchronous writer as app console
output. If a stalled destination fills the bounded staging queue, ZZRun drops
diagnostic log records but reserves space for launcher status and errors; after
pending writes finish during cleanup it reports the number of dropped records.
ZZRun does not write to stderr while the app is running.

### Console and audio routes

The app's console output arrives as `CONSOLE_WRITE` requests naming bytes in a
console buffer the card allocated; a request completes when the DOS write has
finished, with the number of bytes written. A Shell launch writes those bytes
through `Output()` and takes app stdin from `Input()`. A `CONSOLE_READ` request
is the app's demand for input; ZZRun starts one DOS read for it and completes it
with the bytes read, or with 0 at the end of input. A redirected input (`<file`
or a pipe) ends the app's stdin at the end of that stream. From the keyboard,
the app gets whatever ends a `Type *` in the same console; on OS 3.2.3 with the
standard or CCON handler Ctrl-\ does not, so stop such an app with Ctrl-C.
ZZRun starts a read only while the app it owns is running and has asked for
input. If the app is waiting for keyboard input when it stops, ZZRun writes
`press Return to finish.` to the console that owns that read (the Workbench
`CON:` window, or `Input()`), not to redirected stdout: the console cannot
cancel the line read it has open. A pipe or other non-interactive input uses
the same asynchronous read, so an open writer cannot stall the lease, display,
or audio. A regular file completes immediately. If a non-interactive read is
still pending at exit, ZZRun writes `waiting for pending input to finish.` to
the user console (not stdout, and not the pipe) and waits, because a pipe read
cannot be cancelled. The custom screen is closed before either of those waits,
so the prompt is not hidden behind the app display. A Workbench launch opens
no console by default; only when the app first uses console I/O, ZZRun opens a
closable `CON:` window. If opening that requested console fails, or if writing
app output fails, ZZRun stops the app and returns 20.

The default audio route is the card fabric and needs nothing from ZZRun. Set
`AUDIO=AHI` in the app icon or pass `AUDIO=AHI` after the ELF in a Shell
command to request the AHI route; it is announced at load begin. On that route
the card allocates the audio ring and names it in an `AUDIO_OPEN` request; ZZRun
opens AHI at the requested rate (stereo) and reads the PCM straight from the
ring, which is interleaved S16LE with its rate and channel count in its
header. If AHI cannot be opened, ZZRun stops the app and returns 20. It
converts the ring's S16LE samples into AHI's S16BE bounce buffers without
resampling or downmixing, one buffer of at most 1/50 s per pass. An underrun
(AHI finished a buffer and found the ring empty) is counted in the ring header.

Known limitation: running an app with stdout redirected to `NIL:` (`>NIL:`)
made swquake's file seeks fail on the card with every ZZRun build tested,
while `>file` works. This is under investigation.

When the app opens its display, the card posts `DISPLAY_OPEN` with the width,
height and format. RGB565 is the first path; CLUT8 is supported and ZZRun loads
the palette the app posts (`DISPLAY_PALETTE`); BGRA8888 is accepted when P96
returns that format. ZZRun opens the P96 screen and completes the request with
the bitmap's actual pitch and the tokens of its memory, which are board offsets.
With two displayable P96 bitmaps (`AllocScreenBuffer`), each flip request
changes screen buffer and completes when P96 has finished with the old one. If
P96 refuses screen buffers, ZZRun first opens a bitmap three times the
requested height and pans between its pages. If that fails, it retries with a
double-height bitmap. A pan completes with the submitted flag: the card
acknowledges the flip after its own next vertical blank has latched the new
address, which keeps the old front unavailable until scanout has stopped reading
it. Only if both panning layouts fail does it use one bitmap, which the card
copies each frame into. When the app asks for latest-frame flips, the card owns
every flip and ZZRun sees none: it does not pan or change buffers.

### Files and ROOT

File requests (`FILE_OPEN`, `FILE_READ`, `FILE_WRITE`, `FILE_SEEK`,
`FILE_STAT`, `FILE_CLOSE`, `LIST_OPEN`, `LIST_NEXT`, `LIST_CLOSE`) carry paths
and data in buffers the card names. The app has up to 16 open files and 16 open
directory lists. A short read is end of file. A list name is NUL-terminated;
the end of a list is `ENOENT`.

The open request carries the AAR file flags: `READ` 1, `WRITE` 2, `CREATE` 4,
`TRUNCATE` 8, and `EXCL` 16. ZZRun refuses an invalid combination with
`EINVAL` (-2) before it touches AmigaDOS: neither `READ` nor `WRITE`, `CREATE`
or `TRUNCATE` without `WRITE`, `EXCL` without `CREATE`, or an unknown bit.
Valid combinations open as follows.

- `READ`: `MODE_OLDFILE`; a missing file is `ENOENT` (-5).
- `WRITE` or `READ|WRITE`: the file must exist (ZZRun probes with `Lock`,
  because `MODE_READWRITE` creates a missing file); then `MODE_READWRITE`. A
  missing file is `ENOENT`.
- `WRITE|TRUNCATE`: as above, then the file is set to size 0 with
  `ACTION_SET_FILE_SIZE`. A handler without that action gets `MODE_NEWFILE`
  after the probe.
- `WRITE|CREATE`: `MODE_READWRITE` on an existing file, keeping its contents;
  `MODE_NEWFILE` if it is missing.
- `WRITE|CREATE|TRUNCATE`: `MODE_NEWFILE`; create, or replace an existing file.
- `WRITE|CREATE|EXCL`, with or without `TRUNCATE`: ZZRun probes with `Lock`. An
  existing file is `EEXIST` (-11), including one that another program holds
  open exclusively (the handler refuses the lock with `ERROR_OBJECT_IN_USE`),
  for relative and `ABS` paths alike; otherwise `MODE_NEWFILE`.

`READ` may be added to any of the `WRITE` entries. AmigaDOS has no atomic exclusive
create, so `EXCL` is exclusive against the other apps ZZRun serves, which share
one serving loop, but not against other Amiga programs that create the same name
between the probe and the open.

With `ABS` on a multi-directory assign, the probe finds the directory that holds
the file and the open, `SetFileSize` and `MODE_NEWFILE` fallback all go to that
same directory, so an existing file is updated, truncated, kept or replaced
where it is rather than shadowed by a new file in the first directory. Every
step that can create, truncate or replace a file is skipped, and answers `EIO`,
once the request is no longer wanted: the claim was revoked, a Ctrl-C or stop is
pending, or an earlier wait for AmigaDOS was abandoned.

Relative paths stay below `ROOT`. `..`, leading `/`, and empty path components
that would leave the root are rejected, and so is a link whose target lies
outside it. Paths containing `:` are rejected unless `ABS` is enabled. With
`ABS`, file open, stat, and directory-list requests use `GetDeviceProc` and try
later multi-directory assign candidates after `ERROR_OBJECT_NOT_FOUND`. This is
path hygiene for an app that is already trusted; it is not an isolation
boundary. File reads and writes are chunked at 8 KiB so the launcher renews its
lease while serving a large request. A stalled AmigaDOS volume can still block
its DOS call.

The launcher reports file modification times as Unix seconds derived from an
Amiga `DateStamp`, or zero when the time is absent.

## Faults and cleanup

A data abort, prefetch abort, undefined instruction or alignment fault prints
the captured pc, sp, lr, fault address, fault syndrome, program status, exit
code and handle, then returns 20. A watchdog, a runtime restart or a lost lease
is reported the same way and returns 20. A forced stop caused by Ctrl-C
returns the app's normal exit code when it has one. Every exit path closes
opened files and directory handles, restores the public screen, releases the
claim and releases its Amiga-side resources. ZZRun inhibits caching on the
shared buffers it maps and does not restore the previous cache mode; those
pages stay cache-inhibited.

## Building

Use the repository wrapper. It stages the zz9k client headers, `zzapp_proto.h`,
the `aar/` transport headers and the module source (`aar_xport_zz9000.[ch]`)
from the firmware checkout, then builds in the repository's GCC 16 image
(`tools/amiga-docker.sh`) when no local cross compiler is on `PATH`.

```sh
cd ZZRun
./build.sh
```

Set `ZZ9000_FIRMWARE` to a firmware checkout when it is not a sibling of this
repository. The checkout must contain the client transport module
(`sdk/amiga/src/aar_xport_zz9000.[ch]`); an older checkout is refused with that
message. The `aar/` headers come from `AAR_DIR` when it is set, else from the
firmware's own `sdk/aar` (`sh sdk/scripts/fetch-aar.sh` in the firmware
checkout fetches the commit it pins in `sdk/AAR_REF`), else from a sibling
`aar` checkout.

The host tests (`make -C tests test`, or `make zzrun-tests` at the repository
root) need the same two header sets: `FIRMWARE_DIR` (or `ZZ9K_FIRMWARE_DIR`)
and `AAR_DIR`.

# WHDLoad.Audio with the ZZ9000AX driver

WHDLoad 21 can play the CD-DA tracks of CDTV and CD32 games from files
(`Audio/K` option, `resload_Audio`). The files are played by additional audio
hardware, which a plugin called `WHDLoad.Audio` drives. This folder holds a
driver that plays the tracks through the audio fabric of the ZZ9000, and the
script that builds `WHDLoad.Audio` with that driver in it.

Sources: `zz9000.s` is the driver (GPL-3.0-or-later). `WHDLoad.Audio.asm`,
`amigus.s` and the includes belong to Wepl's
[winstalls](https://github.com/wepl/winstalls/tree/master/audio) and are
fetched when the plugin is built, at the commit in `WINSTALLS_REF`. They are
not stored here, because `whdaudio.i` and `whdload.i` do not state a licence
that allows it.

## Requirements

- WHDLoad 21.0 or newer with the option `Audio/K` set to the directory of the
  tracks, a Slave that uses `resload_Audio`, and a program that uses the level 2
  interrupt (`INTB_PORTS`). The plugin is ignored on a 68000, and needs a CPU
  with a movable VBR, so a 68010 or newer.
- A ZZ9000 on Zorro III. Zorro II is not supported.
- Firmware that advertises the audio fabric with source-rate leases
  (`ZZ9K_CAP_AUDIO_FABRIC`, `ZZ9K_SERVICE_FLAG_AUDIO_FABRIC` and
  `ZZ9K_SERVICE_FLAG_AUDIO_FABRIC_RATE`) that also accepts the acquire flag
  `ZZ9K_AUDIO_RING_ACQUIRE_FLAG_IDLE_PAUSED` (bit 2, see `docs/audio-fabric.md`
  in zz9000-firmware; there is no service flag for it, firmware without it
  answers `BAD_REQUEST` and the driver reports no hardware), and
  `zz9k.library` 2.44 or newer, both from the same release as the drivers.
- For FLAC, MP3 and Ogg Vorbis, firmware that accepts
  `ZZ9K_AUDIO_RING_ACQUIRE_FLAG_CODEC` (bit 3). Firmware that refuses this
  flag can still play WAV over a source-rate lease.
- INT2 mode of the card: `ENV:ZZ9K_INT2` or `int2 = on` in `ZZ9000.CFG`, as for
  AHI. WHDLoad calls the plugin from the level 2 interrupt only, and the card
  interrupt has to arrive there. In INT6 mode the driver reports no hardware
  and WHDLoad runs without audio.

## Track files

Name the files `track01.<ext>` to `track99.<ext>` in the `Audio/K`
directory. With codec-ring firmware, WHDLoad checks `flac`, `wav`, `mp3`
and `ogg` in that order for each track number and uses the first file it
finds. It does not try another extension if that file cannot be played.
On older firmware, WHDLoad only looks for `wav`.

The driver checks the file header. It accepts native FLAC, MPEG Layer III
(including files with an ID3v2 tag), and Ogg Vorbis. These are decoded on
the card. AAC, M4A, WMA and Ogg Opus are not available through the codec
ring, and the plugin does not decode them on the Amiga.

The driver converts WAV on the Amiga. It accepts:

| Property | Accepted |
|---|---|
| Format | PCM |
| Rate | 44100 Hz |
| Channels | 1 or 2 |
| Sample size | 8 or 16 bit |

Chunks other than `fmt ` and `data` are skipped, the `fmt ` chunk has to come
first, and a data size that is wrong or `0xffffffff` (a stream written to a
pipe) is cut to the size of the file. An unsupported file is skipped and the
last refusal is recorded in the trace. If the card reports a decode error, the
track ends and the lease stays open for the next track.

To convert a file for WAV playback, run
`ffmpeg -i in.mp3 -ar 44100 -ac 2 -c:a pcm_s16le track01.wav`.
A CD track takes about 10 MB per minute as 16 bit stereo WAV.

## Build

```sh
./build.sh            # out/WHDLoad.Audio
./build.sh slave      # out/audio.slave, the test Slave of winstalls
tests/run-tests.sh    # host test of the driver
make whdload-audio    # from the repository root, same as ./build.sh
```

`build.sh` runs in the m68k container of `tools/amiga-docker.sh` (GCC 16 image
with vasm) and needs network access for the winstalls fetch. It adds
`zz9000_driver` to the driver table of `WHDLoad.Audio.asm`, after the AmiGUS
driver, and `INCLUDE zz9000.s` after `INCLUDE amigus.s`. It stops if those two
lines are not found. The result is one hunk, as WHDLoad requires, and it
still drives an AmiGUS: the plugin probes its drivers in table order and uses
the first one that finds its hardware.

WHDLoad searches for the plugin by the fixed name `WHDLoad.Audio`, in `PROGDIR:`
or in the directory of WHDLoad, and calls it through the header at its start.
There is no way to load a second plugin file, and the driver table is inside the
hunk, so the driver has to be part of that file. Copy `out/WHDLoad.Audio` over
the plugin in the directory of WHDLoad.

What I would offer Wepl for `WHDLoad.Audio`: `zz9000.s` as a third driver file
(relicensed to Public Domain if that is what the project needs), and the two
lines that add it to `WHDLoad.Audio.asm`. The interface does not pass the
track's file extension, so the driver reads the file's header.

## How it plays

`waud_init`, with the operating system active, finds the card and takes a
direct-ring lease. It tries `ZZ9K_OP_AUDIO_RING_ACQUIRE` with `CODEC` and
`IDLE_PAUSED`, slot 1 and then slot 2. On `BAD_REQUEST` for `CODEC` it
retries with a 44100 Hz stereo S16LE `SOURCE_RATE` lease and lists only WAV.
One lease serves all tracks until `waud_exit` releases it with its
generation. The firmware converts the source to its 48 kHz output.
The ring and the control block go into `waud_mem` as non cachable
serialized memory, so the driver needs no cache operation while WHDLoad has the
operating system off. WHDLoad maps those pages and the first 4 KiB of the card
(its registers) that way. The AHI driver and the SDK client helpers call
`CacheClearE` with `CACRF_ClearD` after every write to the ring and the control
line instead, because AmigaOS may map the board cachable; the firmware reads
the ring as a non-coherent producer and invalidates before it copies.

`waud_play` identifies the format, publishes a start marker with the codec
and byte cursor, and primes the ring. The card raises its vblank interrupt
(`ZZ_REG_CONFIG` write `0x82`, acknowledged with `0x48`), because the
audio interrupt is not available while the fabric owns the output.
`waud_irq` runs on level 2 and checks for the card's vblank bit before
servicing the lease. For each card interrupt it

1. acknowledges the card interrupt,
2. reads the firmware line of the control block (consumed cursor, generation,
   status and decode state) under its seqlock,
3. copies the next bytes of the file, or converts WAV frames to stereo S16LE,
   within the ring credit and a limit of 10,584 bytes per call, and
4. publishes the new write cursor and heartbeat, marking the final bytes
   as end of stream on a codec lease.

On a codec lease, `waud_irq` returns `WAUDIRQF_FINISHED` after the card
reports the matching stream drained, or reports a decode error. A loop starts a
new copy of the stream without discarding the end of the previous copy. The
last partial period may be padded before the next repetition. Stop and track
replacement discard the old stream and its queued audio; suspend pauses it
in place.

On a plain source-rate lease, the driver pads a WAV track to a full 20 ms
source period, or to the firmware's preroll if the track is shorter. WAV
loops repeat the samples without padding. On older firmware that ignores
stream markers, `WAUDIRQF_FINISHED` is sent when all frames are written,
before the old audio has drained.

For WAV, the driver applies volume as `(sample * volume) >> 15`, with a plain
copy at `$7fff` for 16 bit stereo. Mono is written to both channels, 8 bit is
converted from unsigned. The card applies volume for compressed tracks. The
stereo separation of the card (ZZTop setting,
`audio_separation`) is a DSP stage behind the mixer that joins Paula and the
playback of the card, so it applies to these tracks as well.

### Heartbeat

The firmware revokes a lease whose heartbeat token has not changed for 2000 ms.
`waud_irq` publishes a new heartbeat on every card interrupt. While WHDLoad has
the operating system active (it loads a track or a file) the plugin is not
called, and the driver leaves nothing in the operating system that could run
instead: no interrupt server, no task, no interrupt. (An exec vertical blank
server that did this made the machine reset on a 68060 with WHDLoad 21.0 beta;
the reason was not found.) The lease is acquired with `IDLE_PAUSED`, which
suspends the firmware's timeout while the producer line carries `PAUSED`. The
line is `PAUSED` from `waud_init` until the first track, after `waud_stop`,
after a finished track once the card has played it, and from `waud_suspend`
until `waud_resume`. `waud_resume` clears it only while a track is playing.
So the timeout runs only while a track plays and the level 2 interrupt is
served. The card interrupt is enabled only while a track is
playing: `waud_play` enables it, `waud_resume` enables it only if a track is
playing, and `waud_stop`, `waud_suspend`, the end of a track once the card has
played it, and `waud_exit` (before anything else) disable it. WHDLoad gives the
operating system back after `waud_stop`, and the PORTS chain of the operating
system does not know the vblank interrupt of the card: left enabled it would
raise level 2 interrupts that nothing acknowledges. With 21.0 beta build 7269
the first version of this driver left it enabled after a stop, and quitting
ended on a black screen.

When the firmware has revoked the lease anyway (the level 2 interrupt masked for
2 s or longer while a track plays, or a fault), the driver sees a generation or status change in
the firmware line, ends the track like a finished one and refuses further
tracks. The plugin cannot take a new lease, because that needs the mailbox and
the operating system. WHDLoad has to be started again. The trace file says
`lease lost` and the firmware status (1 = heartbeat timeout, 2 = cursor fault).

### Limits

- On firmware that recognizes stream markers, stop or a new track discards
  queued audio for the old track. Firmware without marker support can still
  play up to about 240 ms of the old track.
- The first sound follows the card's output fill. Plain PCM leases also
  require a 23040-byte preroll.
- Playing needs the level 2 interrupt and the card interrupt together. A program
  that masks `INTB_PORTS` for longer than 2 s while a track plays loses the lease.
  A paused lease (idle, stopped, or while WHDLoad has the operating system
  active) has no timeout. If WHDLoad stops without `waud_exit` (a crash), a
  paused lease holds its slot until the next Amiga reset.
- Mono, 8 bit and a volume below `$7fff` are converted by the CPU, about 130
  cycles per frame. That is around 40% of a 14 MHz 68020 and little on a 68040
  or 68060. 16 bit stereo at full volume is copied.
- WAV is limited to 44.1 kHz. The firmware resamples compressed tracks from
  the decoded rate to 48 kHz.
- The lease occupies one of the two direct-ring slots for as long as WHDLoad
  runs, and `zz9k.library` stays open.
- The firmware admits two producers that convert (the pump and the two leases
  together). If two others convert already, for example AHI at 44.1 kHz and a
  media session at 44.1 kHz, the lease is refused and WHDLoad runs without
  audio. The trace file says so.

## Trace messages

With the WHDLoad option `Trace` the plugin writes messages to the trace file
during init and exit. At init, `audio: ZZ9000AX: ...` names why the driver was
not used (no card, Zorro II, INT2 mode missing, `zz9k.library` missing or older
than 2.44, no audio fabric or no source rate leases in the firmware, a firmware
that refuses `IDLE_PAUSED`, no free lease, or an unusable grant) or the lease it
took (slot, generation, ring capacity and addresses). At exit:

```
audio: ZZ9000AX: tracks 4, refused 1, interrupts 1520, suspend 3, resume 3, lease intact (firmware status 0)
```

`refused` counts files the driver could not play, and `interrupts` counts
the card vblank interrupts that reached the plugin. `suspend` and `resume`
count the calls from WHDLoad. If the driver refused a file or the card
reported a decode error, the trace also shows the last refusal reason or
decode error code. See the codec ring section in
[`docs/audio-fabric.md`](https://github.com/BlitterStudio/zz9000-firmware/blob/master/docs/audio-fabric.md)
for the firmware error codes.

## Test on a card

1. Install WHDLoad 21.0 or newer from [whdload.de](https://whdload.de/) and
   copy `out/WHDLoad.Audio` into the directory of WHDLoad (`C:` by default),
   replacing the plugin there.
2. `python3 tools/make-test-tracks.py tracks` writes `track01.wav` to
   `track09.wav` (about 6 MB). Copy the folder to the Amiga as `cdda`, next to
   the Slave. The tracks are described in the header of the script.
3. Copy `out/audio.slave` (`./build.sh slave`) to the Amiga, or use the one of
   the WHDLoad developer package.
4. Set `ENV:ZZ9K_INT2` (`setenv ZZ9K_INT2 1`, reboot) or `int2 = on` in
   `ZZ9000.CFG`, and use matching firmware and `zz9k.library` 2.44 or newer.
5. Start `WHDLoad audio.slave Audio=cdda Trace` from a shell, with Preload if
   the tracks should play in sequence.
6. With codec-ring firmware, convert copies of the sample tracks to FLAC,
   MP3 and Ogg Vorbis. Remove any higher-priority copies of the same track
   before testing MP3 or Ogg Vorbis. Try a loop and switch between compressed
   and WAV tracks without stopping in between.

Keys of the test Slave, and what to check:

| Key | Check |
|---|---|
| `1` | Track 1 starts after a fraction of a second, a steady 440 Hz tone. The status line shows track 1 on the main loop and in the vertical blank. |
| `space` | The tone stops. With stream-marker firmware, the queued part of the track is discarded. |
| `2`, `3` | The tone is in the left, then in the right channel only. |
| `l`, then `4` | The 1 kHz beep repeats without a gap. |
| `5` | A short click. The command returns, the callback count rises. |
| `6`, `7` | The 8 bit mono and the 16 bit mono tone play on both channels. |
| `8` | Nothing is heard, the callback count rises at once (48000 Hz is refused). |
| `9`, cursor up and down | The tone gets louder in steps, the cursor keys change the volume smoothly down to silence. |
| `c` several times, then `1` | Tracks 1, 2, ... play in sequence when the tracks are cached; the callback count rises once, after the last one. |
| `o` | The Slave saves a file, WHDLoad switches the operating system on. The sound pauses and continues afterwards. The trace file at the end shows `lease intact` and `suspend` and `resume` both counted. |
| `h` | The level 2 interrupt is masked for 2 s. The sound stops. The lease timeout of the firmware is 2000 ms, so the lease may be revoked; the trace file then shows `lease lost (firmware status 1)`. |
| `s` | The status call returns the track number while playing. |
| `q` | WHDLoad ends and releases the lease. `zz9k-fabriclease` of the firmware SDK is a proof client that takes a slot and plays a tone: while WHDLoad runs it finds slot 1 taken, after `q` it plays. |

Let track 1 play for 10 s or more without keys: the lease must stay (the trace
file shows `lease intact`), and the sound must not break up. Idle time
before the first track is covered by the paused lease, so it does not matter
whether WHDLoad calls `waud_resume` before it; the `resume` count in the trace
file tells whether it does.

## Diagnostic build

`./build.sh diag` writes `out/WHDLoad.Audio.diag`, for finding out where the
driver stops on a card. It behaves like the normal plugin and adds:

- progress codes: characters written to `REG_ZZ_PRINT_CHR` (`0xF0`), which the
  firmware prints on its UART between its own lines. The register lies in the
  page that `waud_mem` gives to WHDLoad. `C` card found and INT2 mode, `Z`
  `zz9k.library` open, `L` lease taken, `O` init done; `R` `waud_resume`, `S` `waud_suspend`, `v` `waud_volume`, `P`
  `waud_play`, `T` `waud_stop`, `X` `waud_exit`, `+` the card interrupt is about
  to be enabled (written directly after it); in `waud_exit`, `D` card interrupt
  disabled, `E` lease released, `F` library closed. In the level 2 routine, for the
  first 300 codes only: `i` entry, the card status word as four hex digits and a
  blank, `a` acknowledged, `c` reading the credit line, `p` publishing and `e` end. The last character before a reset is
  the place to look at.
- files in `ENV:` that switch parts off, checked in `waud_init`: `ZZ9K_WA_NORESUME`
  (`waud_resume` does not enable the card interrupt), `ZZ9K_WA_NOIRQ` (`waud_irq` returns at once; use it
  with NORESUME, otherwise nothing acknowledges the card), `ZZ9K_WA_NOARM` (the
  card interrupt is never enabled). `echo "" >ENV:ZZ9K_WA_NOARM` creates one;
  the trace file shows the switches as a number (1 = NORESUME, 4 = NOIRQ,
  8 = NOARM).

## Host test

`tests/run-tests.sh` assembles `zz9000.s` with `tests/harness.s` into a flat
blob and runs it under qemu-m68k together with `tests/zz9000_audio_test.c`.
The C program provides exec, `expansion.library`, `dos.library`,
`zz9k.library` and a model of the lease in the firmware: seqlock reads of the
producer line, generation, write cursor distance, the 2000 ms heartbeat
timeout, PCM consumption in 20 ms periods, and a codec ring with byte
credits and a separate drain delay. The model checks compressed bytes and
markers without decoding the audio. It covers:

- every refusal path of `waud_init` and the cleanup after it, the memory list,
  the driver table and the first publication of the producer line;
- the audio the model consumes against a reference conversion, for 8 and 16
  bit, mono and stereo, several volumes, long and word aligned samples, tracks
  that wrap the ring many times, and interrupt rates of 20, 50 and 60 Hz;
- volume changes in the middle of a track, loops, stop, replacing a track, a
  sequence of tracks, tracks shorter than the preroll and than one period;
- the WAV parser with valid, odd and damaged files, and the files written by
  `tools/make-test-tracks.py`;
- codec negotiation and extension order, MP3, FLAC and Vorbis bytes across
  ring wrap, EOS and drain, a looping stream, stop and replacement into WAV,
  corrupt file recovery, and refusal of Ogg Opus and unknown files;
- the order of calls when WHDLoad quits (a track playing, stopped, suspended,
  resumed, finished): the card interrupt is disabled before the driver does
  anything that needs the operating system;
- that the driver leaves no interrupt server in exec, a lease that stays paused
  with no heartbeat for 10 s while the operating system is active or before the
  first track, loss of the lease by status and by generation, a torn firmware line, a
  stalled consumer, interrupts that are not the card's;
- that every entry point keeps `d2-d7` and `a2-a6`.
- that, after init, the driver touches only the pages of the board that it has
  declared in `waud_mem` (the other pages of the board are made inaccessible
  while a call runs, like the page tables WHDLoad builds on a 68040 or 68060).

The model is written from `docs/audio-fabric.md` and `audio_fabric_lease.c` of
zz9000-firmware. It does not replace a run on a card.

```sh
tests/run-tests.sh     # exit status 0 when all checks pass
```

It needs Docker for the vasm step and for the m68k Linux compiler and qemu
(`HARNESS_IMAGE`, default `ubuntu:24.04`), or `m68k-linux-gnu-gcc` and
`qemu-m68k` on the PATH.

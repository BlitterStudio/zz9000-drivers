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
  (`SDK_CAP_AUDIO_FABRIC`, `SDK_SERVICE_FLAG_AUDIO_FABRIC` and
  `SDK_SERVICE_FLAG_AUDIO_FABRIC_RATE`), and `zz9k.library` 2.44 or newer,
  both from the same release as the drivers.
- INT2 mode of the card: `ENV:ZZ9K_INT2` or `int2 = on` in `ZZ9000.CFG`, as for
  AHI. WHDLoad calls the plugin from the level 2 interrupt only, and the card
  interrupt has to arrive there. In INT6 mode the driver reports no hardware
  and WHDLoad runs without audio.

## Track files

The tracks are named `track01.wav` to `track99.wav` in the `Audio/K` directory.
The driver plays WAV files with these properties:

| Property | Accepted |
|---|---|
| Format | PCM |
| Rate | 44100 Hz |
| Channels | 1 or 2 |
| Sample size | 8 or 16 bit |

Chunks other than `fmt ` and `data` are skipped, the `fmt ` chunk has to come
first, and a data size that is wrong or `0xffffffff` (a stream written to a
pipe) is cut to the size of the file. Any other file is refused: playing it
reports the end of the track at the next card interrupt and nothing is heard. The trace file
shows how many tracks were refused.

MP3, Ogg, FLAC, AAC and the other formats the AmiGUS decodes are not played.
WHDLoad passes the file in Amiga memory to the plugin, and the plugin runs in
the level 2 interrupt with the operating system off. The decoders of the card
are reached through the mailbox of `zz9k.library`, which needs the operating
system, and decoding on the 68k is too slow. Convert the tracks to WAV, for
example with `ffmpeg -i in.mp3 -ar 44100 -ac 2 -c:a pcm_s16le track01.wav`.
A CD track is 10 MB per minute as 16 bit stereo.

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
lines that add it to `WHDLoad.Audio.asm`. Two things the interface does not
give a driver, and that this one works around: a call when the operating
system is active (the lease of the card needs a heartbeat while WHDLoad loads
files), and the track format in the plugin call, so that a plugin could refuse
a file before it is played.

## How it plays

`waud_init`, with the operating system active, finds the card, checks the
requirements above and takes a direct-ring lease of the audio fabric:
`SDK_OP_AUDIO_RING_ACQUIRE` for a 44100 Hz stereo S16LE source (flag
`SOURCE_RATE`), slot 1 and then slot 2. The firmware converts the lease to its
48 kHz output, so the driver does not resample. The lease, the ring and the
control block are held until `waud_exit`, which releases the lease with its
generation. The ring and the control block go into `waud_mem` as non cachable
serialized memory, so the driver needs no cache operation while WHDLoad has the
operating system off. WHDLoad maps those pages and the first 4 KiB of the card
(its registers) that way. The AHI driver and the SDK client helpers call
`CacheClearE` with `CACRF_ClearD` after every write to the ring and the control
line instead, because AmigaOS may map the board cachable; the firmware reads
the ring as a non-coherent producer and invalidates before it copies.

`waud_play` reads the WAV header and starts feeding. The card is told to raise
its vblank interrupt (`ZZ_REG_CONFIG` write `0x82`, acknowledged with `0x48`),
because the audio interrupt of the card is not available while the fabric owns
the output. `waud_irq` runs for every level 2 interrupt and only reports the end of a
track, unless the vblank bit of the card is set. For each card interrupt it

1. acknowledges the card interrupt,
2. reads the firmware line of the control block (consumed cursor, generation,
   status) under its seqlock,
3. copies the next frames of the track into the ring, converted to stereo S16LE
   with the volume applied, until 12 periods of 20 ms (240 ms, at most 3 periods
   per call) are ahead of the consumed cursor, and
4. publishes the producer line with the new write cursor and a new heartbeat.

At the end of a track the last 20 ms period is completed with silence, because
the firmware consumes whole source periods only, and a track shorter than the
preroll of the firmware (23040 bytes, about 130 ms) is extended until the card
starts to play.
Then `waud_irq` returns `WAUDIRQF_FINISHED`. That is when the last frame has
been written, up to 240 ms before it is heard. `WHDLAUDF_LOOP` restarts the
track from its first frame without a gap. WHDLoad plays the next track of a
sequence by calling `waud_play` from the interrupt, and that track follows the
previous one in the ring.

Volume is applied by the driver as `(sample * volume) >> 15`, with a plain
copy at `$7fff` for 16 bit stereo. Mono is written to both channels, 8 bit is
converted from unsigned.

### Heartbeat

The firmware revokes a lease whose heartbeat token has not changed for 2000 ms.
The heartbeat is published on every card interrupt, which covers the time WHDLoad
has the operating system off. While the operating system is active (WHDLoad
loads a track or a file) `waud_irq` is not called, so `waud_init` adds an
interrupt server on the vertical blank to exec. It publishes the unchanged
producer line with a new heartbeat about every 500 ms and flushes the data cache
for the control line. `waud_suspend` and `waud_exit` disable the card interrupt,
`waud_resume` and `waud_play` enable it. The server is removed in `waud_exit`.

When the firmware has revoked the lease anyway (the level 2 interrupt masked for
2 s or longer, or a fault), the driver sees a generation or status change in
the firmware line, ends the track like a finished one and refuses further
tracks. The plugin cannot take a new lease, because that needs the mailbox and
the operating system. WHDLoad has to be started again. The trace file says
`lease lost` and the firmware status (1 = heartbeat timeout, 2 = cursor fault).

### Limits

- A stop or a new track does not cut the audio at once. What the card has been
  given plays first: the audio fabric queues up to 6 periods (120 ms) in front of
  the output, and the driver keeps up to 12 periods in the ring. A new track
  follows the old one after at most about 240 ms.
- The first sound comes after the preroll of the firmware (23040 bytes) and the
  fill of the ring, which I estimate at 200 ms; it has not been measured.
- Playing needs the level 2 interrupt and the card interrupt together. A program
  that masks `INTB_PORTS` for longer than 2 s loses the lease.
- Mono, 8 bit and a volume below `$7fff` are converted by the CPU, about 130
  cycles per frame. That is around 40% of a 14 MHz 68020 and little on a 68040
  or 68060. 16 bit stereo at full volume is copied.
- 44.1 kHz only. Other rates would need their own lease, which cannot be taken
  with the operating system off.
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
than 2.44, no audio fabric or no source rate leases in the firmware, no free
lease, or an unusable grant) or the lease it took (slot, generation, ring
capacity and addresses). At exit:

```
audio: ZZ9000AX: tracks 4, refused 1, interrupts 1520, suspend 3, resume 3, lease intact (firmware status 0)
```

`refused` counts tracks that were not WAV files in the format above,
`interrupts` the vblank interrupts of the card that reached the plugin.
`suspend` and `resume` count the calls of WHDLoad.

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

Keys of the test Slave, and what to check:

| Key | Check |
|---|---|
| `1` | Track 1 starts after a fraction of a second, a steady 440 Hz tone. The status line shows track 1 on the main loop and in the vertical blank. |
| `space` | The tone stops within about a quarter of a second. |
| `2`, `3` | The tone is in the left, then in the right channel only. |
| `l`, then `4` | The 1 kHz beep repeats without a gap. |
| `5` | A short click. The command returns, the callback count rises. |
| `6`, `7` | The 8 bit mono and the 16 bit mono tone play on both channels. |
| `8` | Nothing is heard, the callback count rises at once (48000 Hz is refused). |
| `9`, cursor up and down | The tone gets louder in steps, the cursor keys change the volume smoothly down to silence. |
| `c` several times, then `1` | Tracks 1, 2, ... play in sequence when the tracks are cached; the callback count rises once, after the last one. |
| `o` | The Slave saves a file, WHDLoad switches the operating system on. The sound stops after about a quarter of a second and continues afterwards. The lease stays: the trace file at the end shows `lease intact` and `suspend` and `resume` both counted. |
| `h` | The level 2 interrupt is masked for 2 s. The sound stops. The lease timeout of the firmware is 2000 ms, so the lease may be revoked; the trace file then shows `lease lost (firmware status 1)`. |
| `s` | The status call returns the track number while playing. |
| `q` | WHDLoad ends and releases the lease. `zz9k-fabriclease` of the firmware SDK is a proof client that takes a slot and plays a tone: while WHDLoad runs it finds slot 1 taken, after `q` it plays. |

Let track 1 play for 10 s or more without keys: the lease must stay (the trace
file shows `lease intact`), and the sound must not break up. The trace file
also shows whether WHDLoad calls `waud_resume` before the first track: if
`resume` is 0 at the end of a run in which the card interrupt worked, the
heartbeat before the first track depended on the interrupt server only.

## Host test

`tests/run-tests.sh` assembles `zz9000.s` with `tests/harness.s` into a flat
blob and runs it under qemu-m68k together with `tests/zz9000_audio_test.c`.
The C program provides exec, `expansion.library`, `dos.library`,
`zz9k.library` and a model of the lease in the firmware: seqlock reads of the
producer line, generation, write cursor distance, the 2000 ms heartbeat
timeout, the preroll, consumption in whole 20 ms periods and credits that trail
the consumption by the fill-ahead. It checks:

- every refusal path of `waud_init` and the cleanup after it, the memory list,
  the driver table and the first publication of the producer line;
- the audio the model consumes against a reference conversion, for 8 and 16
  bit, mono and stereo, several volumes, long and word aligned samples, tracks
  that wrap the ring many times, and interrupt rates of 20, 50 and 60 Hz;
- volume changes in the middle of a track, loops, stop, replacing a track, a
  sequence of tracks, tracks shorter than the preroll and than one period;
- the WAV parser with valid, odd and damaged files, and the files written by
  `tools/make-test-tracks.py`;
- heartbeat while the operating system is active with and without the server,
  loss of the lease by status and by generation, a torn firmware line, a
  stalled consumer, interrupts that are not the card's;
- that every entry point keeps `d2-d7` and `a2-a6`.

The model is written from `docs/audio-fabric.md` and `audio_fabric_lease.c` of
zz9000-firmware. It does not replace a run on a card.

```sh
tests/run-tests.sh     # exit status 0 when all checks pass
```

It needs Docker for the vasm step and for the m68k Linux compiler and qemu
(`HARNESS_IMAGE`, default `ubuntu:24.04`), or `m68k-linux-gnu-gcc` and
`qemu-m68k` on the PATH.

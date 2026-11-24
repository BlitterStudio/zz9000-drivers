# ZZ9000AX AHI hardware-mixing specification

The AudioModes `ZZ9000AX:Hardware mixing, 32 voices` (`$CAFE0002`),
`ZZ9000AX:Hardware mixing, 32 voices, interpolated` (`$CAFE0003`) and
`ZZ9000AX:Hardware mixing, 32 voices, cubic` (`$CAFE0004`) let the
ZZ9000AX mix the channels of an AHI program. `ahi.device` mixes nothing in
these modes. It hands every `AHI_SetSound`, `AHI_SetFreq` and `AHI_SetVol`
call to `zz9000ax.audio`, and the driver maps each AHI channel to a voice of
the card's wavetable voice engine (the engine `zz9k-voiceplay` uses). The
three modes differ only in how the engine reads a sample: nearest, linear
and 4-point cubic.

The modes are for programs that set a sound, a frequency and a volume per
channel: trackers and module players. A program that streams samples through
the AHI device interface (Play16, ZZPlay's AHI output, the sound datatype)
loads every buffer as `AHIST_DYNAMICSAMPLE`. The engine plays from its own
memory and cannot follow a buffer that changes, so such a load fails with
`AHIE_BADSOUNDTYPE`. These programs use `ZZ9000AX:16 bit Stereo` or
`16 bit Stereo++`.

The modes need firmware that advertises the voice engine in the audio
service flags and `zz9k.library` revision 42 or newer. The existing modes
`$CAFE0000` and `$CAFE0001` are unchanged.

## Mode identity

The mode file gives each hardware-mixing mode a private tag whose data is the
mode id: `AHIDB_ZZVoice` (`AHI_TagBase + 500`) for `$CAFE0002`,
`AHIDB_ZZVoiceLinear` (`AHI_TagBase + 501`) for `$CAFE0003` and
`AHIDB_ZZVoiceCubic` (`AHI_TagBase + 502`) for `$CAFE0004`. A tag whose data
is another mode's id makes no hardware mode. `ahi.device`
passes the mode's tag list to `AHIsub_AllocAudio` and `AHIsub_GetAttr`, so the
driver reads the mode from there and needs no state of its own. The ids
`$CAFE0000` to `$CAFE0003` keep the meaning they had: the cubic mode is a
separate id, so a program or a saved AHI setting that names an older mode
plays as before.

| AHI property | Value |
| --- | --- |
| Allocation flags | None: `ahi.device` neither mixes nor times |
| Channels | Up to 32 (`AHIDB_MaxChannels`), one engine voice each |
| Rate | 48000 Hz only (`AHIDB_Frequencies` is 1, index 0) |
| `AHIDB_Volume`, `AHIDB_Panning`, `AHIDB_Stereo` | TRUE |
| `AHIDB_PingPong` | TRUE: a negative length plays a sound backward |
| `AHIDB_HiFi` | FALSE |
| `AHIDB_Record`, `AHIDB_FullDuplex` | FALSE: playback only |
| `AHIDB_MaxPlaySamples` | 33554432 frames, the largest bank |
| `AHIDB_Realtime` | TRUE |

## What is refused

`AHI_AllocAudio` fails (`AHISF_ERROR`) and the debug channel carries one line
that starts with `ZZ9000AX: hardware mixing refused:`. Nothing falls back to
software mixing inside a hardware mode.

| Condition | Debug text | Remedy |
| --- | --- | --- |
| More than 32 channels requested | `N channels, the voice engine has 32` | Fewer channels, or a software mode |
| Zorro II card | `does not advertise the voice engine on Zorro II` | A software mode, until firmware advertises the engine there |
| `zz9k.library` missing | `zz9k.library is not available` | Install the matched drivers release |
| Library older than revision 42 | `older than revision 42` | Install the matched drivers release |
| Firmware without the voice flag | `does not advertise the voice engine` | Install matched firmware |
| `$CAFE0004` on firmware that does not advertise cubic interpolation | `does not advertise cubic interpolation` | Install matched firmware, or use `$CAFE0003`. The cubic mode never plays with another interpolation |
| Engine busy (another voice client, a register-driven AHI client, no card memory for the buffers) | `refused the session` with the status | Close the other voice client |
| No card memory for the bank at any size | `no memory for the sample bank` | Free card memory, or lower `ENV:ZZ9K_AHI_BANK` |
| A legacy MHI owner, or a second AHI owner | `audio hardware already owned` | Close the other owner |

A refused allocation leaves the owner, the library base and the card as they
were. Only a fabric-qualified MHI may play beside a hardware-mixing session.

## Timeline

The engine applies a record at the output frame named by its stamp. The
driver keeps a model of every channel in engine frames and stamps each
record with the model time at which `ahi.device` would have made the change.

- The model starts at `playing_frame + lead_frames`, and never closer to the
  engine horizon than one period (960 frames). The granted lead is 7680
  frames (160 ms) unless `ENV:ZZ9K_AHI_LEAD` raises it.
- `PlayerFunc` runs `ahiac_PlayerFreq` times per second on the model clock,
  paced from the card's `playing_frame`. Each tick is 48000 divided by
  `ahiac_PlayerFreq` frames, carried as a 16.16 fraction, read again after
  every `PlayerFunc`, so a tempo change through `AHI_ControlAudio` takes
  effect at the next tick.
- The model runs up to two leads ahead of the sound. A tick's records are
  written when its stamp is still a lead ahead of the horizon. So
  `PlayerFunc` and `SoundFunc` run 160 to 320 ms ahead of what is heard.
- Every pass boundary is predicted with the engine's own arithmetic
  (`zz9k_voice_pass_frames` in `zz9k/voice.h`), and `SoundFunc` is called at
  that frame, in frame order. The walk keeps the channels that have a
  boundary due in a heap ordered by (frame, channel number), so a boundary
  costs one heap update whatever the channel count; a call made inside
  `SoundFunc` can move any channel's boundary, and the heap is built again
  then. The model uses 32-bit arithmetic on that path (a 64-bit product from
  16-bit pieces, a long division in base 65536 for the pass length),
  bit-identical to `zz9k_voice_pass_frames`, because the 68060 does not
  execute the 64-bit `mulu.l` and `divu.l` forms. A rate of 48 kHz or more
  over a pass longer than 65535 frames still takes the shared function and
  its libgcc divide. The driver is built with `-m68020-60` for the same
  reason.
- Stamps never decrease. The worker is the only writer of the command ring.
- A worker that could not run for longer than the lead is behind the sound.
  It resets the engine, which drops the queued records, and starts the model
  again at the current frame. The stats count this as `resyncs`. A single
  late record is not resynchronised; it is counted as `late`.
- `AHI_ControlAudio` stop resets the engine and stops the model. Start begins
  a new timeline and every channel that was sounding starts again at its
  model position. A reset moves the engine on (its playing frame resumes at
  the staged cursor and it renders silence up to the lead again), so Start
  and a resync read the status again after the reset and anchor on that
  one; an anchor from the status before it would sit behind the new horizon
  and count its first records as `late`.

## AHI calls and engine records

| AHI feature | AHI rule | Records |
| --- | --- | --- |
| `AHI_SetSound` with `AHISF_IMM` | Replaces the current and the next pass, `SoundFunc` runs at the start | One `SET` (rate and volume, when they changed), `START`, `LOOP` over the sound's region, `DIRECTION` backward for a negative length |
| `AHI_SetSound` queued | Takes effect when the current pass ends | `LOOP` over the new region when the next pass is the same sample type and forward; `START`, `LOOP` and `DIRECTION` at the predicted boundary otherwise |
| `AHI_NOSOUND`, immediate | Stops at once | `STOP` |
| `AHI_NOSOUND`, queued | Silence at the pass end | `LOOP` with length 0, or `STOP` at the boundary |
| `AHI_SetFreq`, `AHI_SetVol` | Immediate or at the pass end | `SET` at the model time, or at the predicted boundary |
| `AHI_SetFreq` with 0 | Pauses and resumes at the same point | `STOP`, then `START` at the saved position with the rest of the pass |
| `AHI_MIXFREQ` | The mode's rate | 48000 Hz |
| Negative length | Offset is the first sample, the sound plays down to `offset + length + 1` | `START` at the offset, `LOOP` from the low bound over the region, `DIRECTION` backward |
| Volume and pan | Left is volume times (1 minus pan), right is volume times pan, each channel divided by the channel count | Voice volumes from the same law; the master gain carries the division (see Levels) |
| Negative volume or pan | Invert or surround, which not every driver can do | The absolute value, as the autodoc allows |
| `SoundFunc` | Runs at the start of every pass with the channel number | Called by the driver at the predicted boundary; calls made inside it are queued and define the following pass |
| `PlayerFunc` | `ahiac_PlayerFreq` times per second | The tick scheduler above |
| `AHIET_MASTERVOLUME` | 0 to the channel count | `MASTER` gain, see Levels |
| `AHIET_CHANNELINFO` | Offsets of every channel, in sample frames | Model positions at the model time, through the client's hook once per worker pass |
| `AHIET_OUTPUTBUFFER`, `AHIET_DSPMASK`, `AHIET_DSPECHO` | Optional | `AHIE_UNKNOWN` |
| `AHIST_SAMPLE` of `AHIST_M8S`, `AHIST_M16S`, `AHIST_S8S`, `AHIST_S16S` | Memory stays unchanged until `AHI_UnloadSound` | Copied once into the card bank through the staging buffer |
| `AHIST_DYNAMICSAMPLE`, other sample types | | `AHIE_BADSOUNDTYPE`, `AHIE_BADSAMPLETYPE` |
| `AHIET_MASTERVOLUME` cancel | Volume back to 1.0 | `MASTER` for 1.0 |
| Anti-click (`ahiac_AntiClickSamples`) | Smooths immediate changes | The engine ramp, when the firmware advertises it; see Ramps, limiter and cubic reading |
| Recording | | Not offered |

A batch of calls that arrives together, for example the three calls of one
`AHI_PlayA` channel, makes one `START`. `AHIsub_Disable` keeps the batch
whole and keeps the client's hooks quiet: while the client holds `Disable`
the worker takes nothing from the queue and calls no `PlayerFunc`,
`SoundFunc` or channel-info hook. It still reads the status block, so the
session stays alive, and sends the records it already made.

### Levels

The engine's master gain is a whole number of 1/256, so `master volume /
channels` cannot always be expressed. The driver rounds the gain up and
multiplies each voice volume by the remainder, so the mix equals what
`ahi.device` would produce to within two least significant bits, and within
four when `AHIET_MASTERVOLUME` is above 1.0.

### What the engine cannot carry

When a pass starts through a `START` record, the engine begins at a whole
sample frame and drops the fractional part the previous pass had accumulated.
A repeating sound, or a next pass of the same type and direction, wraps in
the engine and keeps its fraction. The difference is below one sample.

## Ramps, limiter and cubic reading

The driver reads the features word of the status block (`ZZ9K_VOICE_FEATURE_*`
in `zz9k/voice.h`) once, after the session opens, and sends `CONFIG` records
only for what the word advertises. Every Start sends them again, at the first
stamp of the new timeline and after the `MASTER` record. Firmware that
advertises none of it gets the records of the earlier driver and nothing
else.

| Firmware advertises | Record | Content |
| --- | --- | --- |
| `CONFIG`, `RAMP` | `CONFIG` ramp | A ramp of `ahiac_AntiClickSamples` frames, at most 512. The modes run at 48000 Hz, so one sample is one engine frame. 0 sends no ramp |
| `CONFIG`, `LIMITER` | `CONFIG` limiter | The soft knee on. It goes in the same record as the ramp when both are sent |
| `CONFIG`, `VOICE_INTERP`, `CUBIC`, in `$CAFE0004` | `CONFIG` interpolation | Cubic as the default interpolation, in a record of its own |

With a ramp the engine fades instead of stepping: a `START` rises from
silence over the ramp length, a volume change moves over it, and a `STOP` or
a restart of a sounding voice lets the old sound fade out. A pass that
reaches its end still stops at once. The model's positions, pass boundaries,
`SoundFunc` calls and `AHIET_CHANNELINFO` offsets are the same with and
without a ramp. The limiter bends a sum above 24576 toward the 16-bit rails
without reaching them, and passes anything below unchanged.

### Fade tails and the bank

A faded sound is read after the channel has dropped it, so the client may
unload it while the engine still plays it. The model keeps a list of tails:
the bank bytes the engine can read for a voice at every `STOP` and at every
`START` over a sounding voice. The range is the region the engine plays and
its programmed loop, taken from the model's picture of the engine and not from
the channel's AHI state, plus one frame on each side (inside the sound's
guards, see Sample bank). A tail
lasts until the engine horizon is strictly past the stamp of the record plus
the ramp length. A range of an unloaded sound is handed out again only after
the plain rule (the horizon is past the unload) and when no tail overlaps
it.

- Equal ranges share one entry, and an entry only ever ends later, so a later
  record, even one made with a shorter ramp, cannot shorten an earlier tail.
  A sound that several voices stop, or that one voice is restarted from
  several times, stays held until the last of its tails ends.
- A record behind the engine applies at the horizon, so its tail starts
  there.
- The list holds 128 ranges. When it is full the new range joins the entry
  that ends first. The merged range covers both and ends with the later of
  the two, so a full list holds bytes longer and never shorter.
- Unloading a sound that a queued pass would wrap into clears that loop at
  once (`LOOP` with length 0), with or without a ramp. The engine reads the
  loop start as the blend partner of the last frames of the pass, and as the
  two taps ahead for cubic reading, and with a ramp it wraps into the range
  at the pass end and fades out there. All of that would read the range after
  it was free. The voice ends at the pass end instead, as it does when the
  queued sound is replaced by silence.
- An unload first installs a pass that ends exactly at the model time (its
  `SoundFunc` stays owed), because the engine is already in the next pass, and
  in its loop, at that frame. The voice and the loop are then stopped and
  cleared in the pass the engine really plays.

### Cubic reading

`$CAFE0004` needs the `CONFIG`, `VOICE_INTERP` and `CUBIC` features. When
one is missing `AHI_AllocAudio` fails and the debug channel names the
reason. The mode does not fall back to linear or nearest reading. It opens
the session without the linear flag and sends the cubic default before the
first note. The cubic reader looks at the frame behind the one it plays, in
play order, which for a forward pass at the first frame lies one frame below
the sound and for a backward pass at the first frame one frame above it. The
two frames ahead are clamped to the pass or its loop, and a pass lies inside
its sound. Those frames are the sound's guards (see Sample bank), which are
zero, so a sound's samples never depend on its neighbours in the bank.

The engine's 2x oversampling is not used. The model predicts pass ends with
48 kHz steps, and an engine that steps at 96 kHz splits a step in two (at
44100 Hz, 60211 in one step against 2 x 30105), so the boundaries would
diverge.

## Sample bank

All sounds live in one bank on the card. The driver defines four engine
samples over the whole bank, one per sample type, and addresses a sound as
its type and a byte offset in the bank. So the number of sounds is not limited
by the engine's 256 samples, and the next pass can be any sound of the same
type.

Every sound has an allocation of its own: 8 bytes of zero (two frames of the
widest format), the data rounded up to 4 bytes, and 8 bytes of zero. The
bank offset of the sound is that of its data, and the allocator hands out,
takes back and quarantines the whole allocation. From audio_voice.c, nearest
and linear reading stay inside a pass, and cubic reading goes one frame
(at most 4 bytes) outside it, so the guard is more than the engine can read
and the margin also covers a vector load of whole taps. `AHI_LoadSound`
writes the guards and the rounding bytes on every load, because the range
may hold an older sound. A sound's samples are therefore the same whatever its
neighbours hold, and whatever is loaded, changed, unloaded or loaded again
beside it.

A pass that is queued after another sound's pass and wraps into it in the
engine reads that sound by design, while it is loaded; unloading it clears the
loop (see Fade tails and the bank).

- The driver asks for 16 MiB. `ENV:ZZ9K_AHI_BANK` (in KiB) overrides the
  request. When the card has no memory for it, the driver halves the request
  down to 1 MiB, then asks again with the default host buffers.
- On Zorro III the session uses a 1024-record ring and a 64 KiB staging
  buffer, so a multi-megabyte sample set loads in few round trips.
- `AHI_LoadSound` copies the guard, the sample and the rounding and guard
  zeros in staging-sized pieces, then defines the
  sound in the driver. The voice session belongs to the worker process that
  opened it, and the firmware refuses a voice request under any other
  client id. Before revision 44, `zz9k.library` sent `ZZ9KVoiceOpen`, `Close`
  and `Reset` as client 0 and `ZZ9KCall` as the client id of the calling
  task's opener; from revision 44 both carry the opener's id. The worker does
  not open `zz9k.library` itself, so with every revision its open and its raw
  upload and define requests all carry client 0, the id the session was
  opened under. The worker therefore sends the copy:
  the calling task reserves the bank range and
  checks for a held sound as before, hands the sample to the worker, wakes
  it and waits for the answer. Two tasks that load at once take turns. The
  worker copies one staging buffer per pass and reads the status in every
  pass, so a large sample neither lets the session lapse (it closes after 2
  seconds without a heartbeat) nor stalls the voices that play meanwhile. A
  failed upload, or a session the card has closed, frees the range and
  returns `AHIE_UNKNOWN`. A full bank returns `AHIE_NOMEM` and prints the
  free space.
- Loading a sound number that is still loaded replaces it, as `ahi.device`
  replaces its own entry. A voice that plays the old sound stops first.
- `AHI_UnloadSound` never calls the mailbox, so it cannot break a `Forbid()`.
  The worker frees the sound: a voice that plays it stops at the model time,
  and the bank range is handed out again only after the engine horizon has
  passed that frame and no fade tail reads the range (see Fade tails and the
  bank). The driver tracks up to 256 such ranges. Past that, the
  range stays with its sound number until the horizon passes, and loading
  that number again in the meantime returns `AHIE_NOMEM`.
- `AHIST_S8S` and `AHIST_S16S` need firmware that advertises
  `ZZ9K_SERVICE_FLAG_AUDIO_VOICE_STEREO` (bit 29 of the audio service flags).
  Without the flag a stereo `AHI_LoadSound` returns `AHIE_BADSAMPLETYPE` and
  mono sounds work.

## Environment and statistics

| Variable | Meaning |
| --- | --- |
| `ENV:ZZ9K_AHI_BANK` | Sample bank request in KiB, default 16384, at most 32768 |
| `ENV:ZZ9K_AHI_LEAD` | Lead in engine frames, default 0 for the firmware floor (7680) |
| `ENV:ZZ9K_AHI_VOICE_STATS` | Written by the driver when a hardware-mixing session closes |

The stats line holds the engine counters (`late`, `rejected`, `underruns`,
`dropped`) and the driver's own: `ringfull` (writes the ring could not take
whole), `ticks` (`PlayerFunc` calls), `frames` (model frames run),
`resyncs`, `fifodrops` (calls lost because the call queue was full),
`recorddrops` (records lost because the record buffer was full), `overload`
(tick walks cut short), `features` (the features word the firmware
advertised), `ramp` (the ramp length of the last Start, 0 for off) and
`limiter` (1 when the last Start switched the soft knee on). A clean run has
zero `late`, `rejected`, `underruns`, `dropped`, `ringfull`, `resyncs`,
`fifodrops`, `recorddrops` and `overload`; the other fields describe the run.

## Tests

- `make ahi-voice-tests` builds the channel model on the host and runs it
  against the firmware's engine source (`voice_ref_test.c`): every voice, on
  every frame, against an independent AHI frame stepper, for random call
  sequences with up to 32 channels. Every sequence runs with one of eight
  quality settings, from none (the earlier modes) to a 512 frame ramp with
  the limiter and cubic reading, and a voice that fades out counts as
  silent in the AHI picture.
- The same test runs unload and reload against fade tails: two worlds get
  the same client calls and a real engine each, one that never reuses a bank
  range and one that uses the model's allocator in a small bank, polled with
  its engine's horizon while the engine runs behind the model. Both must
  render the same samples. Random clients cover it, and three scripted runs
  (a restart, a stop and a queued pass that is unloaded) cover each way a
  fade can outlive its sound, with ramps of 1 to 512 frames and engine lags
  of 0 to 3000 frames.
- A third test plays one asset through the real engine in two banks that
  differ in every byte outside the asset's allocation, while its neighbours
  below and above are loaded, rewritten, unloaded and loaded again into the
  reused range. The assets have 1, 2, 3 and more frames in all four formats;
  they play forward and backward from the first and last frame, whole and in
  part, with and without a loop over themselves, at rates from 300 to 96000
  Hz, with each interpolation, with and without a ramp. The samples must be
  identical. Guards of 4 bytes pass this test, and no guard fails it for
  cubic reading, which is the bound the guard size rests on.
- `voice_model_test.c` checks the `CONFIG` records, the tail ranges and the
  frame on which an unloaded range is released (the last frame of the tail is
  still held, the next is free), with shared sounds, restarts, stops at
  different and equal stamps, a shorter later ramp, a full tail list and
  stamps across the frame counter wrap, and that a ramp changes none of the
  other records, the `SoundFunc` order or the channel offsets, and the
  guarded allocations: offsets, the room the guards need and the release of
  a whole allocation.
- `voice_math_test.c` (also in `make ahi-voice-tests`) compares the model's
  32-bit arithmetic with the plain 64-bit expressions and with
  `zz9k_voice_pass_frames`: the edges of every fast path and millions of
  random operands.
- `ahi/driver/voice_worker_test.c` compiles the driver with a fake voice
  session and runs the worker's steps by hand under vamos. Its header comment
  has the build and run commands.
- [`ZZAXVoiceTest`](../ahi/voicetest/README.md) runs the scenarios on the card.

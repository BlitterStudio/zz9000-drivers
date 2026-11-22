# ZZ9000AX AHI hardware-mixing specification

The AudioModes `ZZ9000AX:Hardware mixing, 32 voices` (`$CAFE0002`) and
`ZZ9000AX:Hardware mixing, 32 voices, interpolated` (`$CAFE0003`) let the
ZZ9000AX mix the channels of an AHI program. `ahi.device` mixes nothing in
these modes. It hands every `AHI_SetSound`, `AHI_SetFreq` and `AHI_SetVol`
call to `zz9000ax.audio`, and the driver maps each AHI channel to a voice of
the card's wavetable voice engine (the engine `zz9k-voiceplay` uses).

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
mode id: `AHIDB_ZZVoice` (`AHI_TagBase + 500`) for `$CAFE0002` and
`AHIDB_ZZVoiceLinear` (`AHI_TagBase + 501`) for `$CAFE0003`. `ahi.device`
passes the mode's tag list to `AHIsub_AllocAudio` and `AHIsub_GetAttr`, so the
driver reads the mode from there and needs no state of its own.

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
  that frame, in frame order.
- Stamps never decrease. The worker is the only writer of the command ring.
- A worker that could not run for longer than the lead is behind the sound.
  It resets the engine, which drops the queued records, and starts the model
  again at the current frame. The stats count this as `resyncs`. A single
  late record is not resynchronised; it is counted as `late`.
- `AHI_ControlAudio` stop resets the engine and stops the model. Start begins
  a new timeline and every channel that was sounding starts again at its
  model position.

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
| Anti-click (`ahiac_AntiClickSamples`) | Smooths immediate changes | Not reproduced |
| Recording | | Not offered |

A batch of calls that arrives together, for example the three calls of one
`AHI_PlayA` channel, makes one `START`. `AHIsub_Disable` keeps the batch
whole: the worker takes nothing from the queue while the client holds
`Disable`.

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

## Sample bank

All sounds live in one bank on the card. The driver defines four engine
samples over the whole bank, one per sample type, and addresses a sound as
its type and a byte offset in the bank. So the number of sounds is not limited
by the engine's 256 samples, and the next pass can be any sound of the same
type.

- The driver asks for 16 MiB. `ENV:ZZ9K_AHI_BANK` (in KiB) overrides the
  request. When the card has no memory for it, the driver halves the request
  down to 1 MiB, then asks again with the default host buffers.
- On Zorro III the session uses a 1024-record ring and a 64 KiB staging
  buffer, so a multi-megabyte sample set loads in few round trips.
- `AHI_LoadSound` copies the sample in staging-sized pieces, then defines the
  sound in the driver. A failed upload frees the range and returns
  `AHIE_UNKNOWN`. A full bank returns `AHIE_NOMEM` and prints the free space.
- Loading a sound number that is still loaded replaces it, as `ahi.device`
  replaces its own entry. A voice that plays the old sound stops first.
- `AHI_UnloadSound` never calls the mailbox, so it cannot break a `Forbid()`.
  The worker frees the sound: a voice that plays it stops at the model time,
  and the bank range is handed out again only after the engine horizon has
  passed that frame.
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
`recorddrops` (records lost because the record buffer was full) and
`overload` (tick walks cut short). A clean run has zeros everywhere except
`ticks` and `frames`.

## Tests

- `make ahi-voice-tests` builds the channel model on the host and runs it
  against the firmware's engine source (`voice_ref_test.c`): every voice, on
  every frame, against an independent AHI frame stepper, for random call
  sequences with up to 32 channels.
- `ahi/driver/voice_worker_test.c` compiles the driver with a fake voice
  session and runs the worker's steps by hand under vamos. Its header comment
  has the build and run commands.
- [`ZZAXVoiceTest`](../ahi/voicetest/README.md) runs the scenarios on the card.

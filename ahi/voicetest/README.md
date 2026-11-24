# ZZAXVoiceTest

`ZZAXVoiceTest` is a low-level AHI client for the ZZ9000AX hardware-mixing
AudioModes. It generates its samples in memory, loads static `AHIST_SAMPLE`
sounds, and exercises the same channel calls and hooks used by a tracker.
It does not use the AHI device streaming interface or play files.

## Build

From this directory, run `./build.sh`. It uses `m68k-amigaos-gcc`, or the
repository's pinned GCC 16 Docker image when the compiler is absent. From
the repository root:

```sh
AMIGA_IMAGE= tools/amiga-docker.sh ahi/voicetest ./build.sh
# or:
make ahi-voicetest
```

The output is `ahi/voicetest/ZZAXVoiceTest`, an AmigaOS 3 command-line program
for a 68020 or later. It uses the same `-noixemul` startup and `-lamiga`
linking as `ZZAXDuplexTest`. It opens ahi.device and timer.device itself;
there is no library auto-open for either base. `tools/check-link-map.sh`
is for startup-less drivers, not these command-line executables.

## Run

Close other low-level AHI clients and voice-engine clients first. Install
the matching audio driver and AudioModes file. Hardware modes also need
firmware and zz9k.library with voice-engine support. The maintainer runs
these tests on the card.

```text
ZZAXVoiceTest
ZZAXVoiceTest MODE $CAFE0002 SCENARIO S1 CHANNELS 32 SECONDS 30
ZZAXVoiceTest MODE $CAFE0003 SCENARIO ALL SECONDS 10
ZZAXVoiceTest MODE $CAFE0000 SCENARIO S1 CHANNELS 8 SECONDS 30
ZZAXVoiceTest MODE $CAFE0002 SCENARIO S10 CYCLES 50
ZZAXVoiceTest MODE 0xCAFE0004 SCENARIO S1 CHANNELS 32 SECONDS 20 ANTICLICK 240
```

The ReadArgs template is:

```text
MODE,SCENARIO,CHANNELS/K/N,SECONDS/K/N,CYCLES/K/N,ANTICLICK/K/N
```

`MODE` and `SCENARIO` can also be positional, for example
`ZZAXVoiceTest $CAFE0003 S7`. Mode IDs are hexadecimal, with an optional
`$` or `0x` prefix. The default is `$CAFE0002`, hardware mixing with nearest
sampling. `$CAFE0003` selects interpolated hardware mixing and `$CAFE0004`
cubic hardware mixing, which needs firmware that advertises cubic
interpolation. A software mode,
such as `$CAFE0000`, runs the same scenarios through ahi.device's mixer.
The program opens ahi.device version 4 through `AHI_NO_UNIT`, then requests
48 kHz, the channels and sounds needed by the scenario, and its hooks.
It queries the actual mix frequency after starting playback.

`SCENARIO` accepts `S1` to `S10` or `ALL` (the default). `CHANNELS` accepts
1 to 32. `SECONDS` accepts 1 to 600 and defaults to 5. It sets the listening
interval in S1 and S2, and a measurement budget in S3, S4, S6, S7 and S8.
Minimum measurement windows can make those scenarios longer: S3 needs
several passes at each length; S4 uses at least 2 seconds at each rate.
S5 and S9 have fixed scripts. S10 plays for 100 ms per allocation;
`CYCLES` selects 1 to 2000 repetitions and defaults to 50.

The waits count 20 ms `Delay` steps, not elapsed time, so under load a step
takes longer and `SECONDS` is a nominal `Delay`-step budget, not a wall-clock
deadline. On the card, S1 with 32 cubic voices
and `SECONDS 20` ran 34.9 s of engine time (1673280 frames and 1743
`PlayerFunc` ticks in the statistics). S3, S4 and S6 read the E clock and
compute the expected counts from the time that actually elapsed, so a
stretched window does not fail them. `ALL` with `SECONDS 10` adds up to about
92 s of waits plus 59 allocation sessions (nine scenarios and 50 S10 cycles)
when the machine keeps pace; leave a generous margin when a job has a
time limit.

`ANTICLICK` passes `AHIA_AntiClickSamples` (`AHI_TagBase + 13`, an
`ahi.device` 6 tag) to `AHI_AllocAudio`. The value is a number of sample frames
at the mode's 48000 Hz, so 240 is 5 ms, and the driver cuts it to 512. It
accepts 0 to 512. 0 passes the tag with the value 0, which switches the ramp
off whatever the preferences hold. Without `ANTICLICK` the tag is not passed
and AHI uses the default anti-click time of AHI Preferences. The tool needs
`ahi.device` 6 or newer for `ANTICLICK` and exits with 10 otherwise, because an
older device ignores the tag.

With `ANTICLICK` and a hardware mode, the tool reads `features` and `ramp`
from the driver statistics (see below) after every session. It passes the
run only when the firmware advertises `CONFIG` and `RAMP` and the driver
sent `ramp` equal to the value given. A firmware that does not advertise
them fails the run, because the ramp was not exercised. The statistics prove
what the driver sent. Whether the ramp sounds right is a listening check.

The Shell expands `$name` as a variable, so the `0x` form of a mode ID
(`MODE 0xCAFE0004`) is the safe spelling in a script or when a variable of that
name might exist.

To test the default instead of a given value, leave `ANTICLICK` out and set
the default anti-click time in AHI Preferences, on the `Advanced settings` page
(`Default anti-click time`, in milliseconds; AHI turns it into samples at the
mix rate). That needs the Preferences window, so someone at the machine has to
operate it: note the current value before changing it, save the test value,
and set the noted value back afterwards. The command line `ANTICLICK` leaves
the preferences alone and is the way to run a ramp without touching them. The
driver reads the value as `ahiac_AntiClickSamples` once, in AllocAudio, and
the stats line shows what it used as `ramp=`.

Ctrl-C stops the current scenario, cleans up and returns 5. Keep the
printed log and record whether the listening checks below sounded right.

## Scenarios

Each scenario prints `S<number> PASS` or `S<number> FAIL` and its reason.
PASS covers the API, timing and counter checks. It does not certify sound
quality: the program has no analogue loopback input. In particular, listen
for distortion in S2 and silence/resumption in S8; an audible fault makes
the hardware qualification fail even when the automated checks pass.

| Scenario | Test |
| --- | --- |
| S1 | One shared 16-bit sine sound, a different pitch per channel and pan spread from left to right; prints channel positions through `AHIET_CHANNELINFO`. |
| S2 | A 32-voice major chord at equal channel level; exercises master volume 1.0 and 0.5, staying below the documented clipping limit. |
| S3 | One sample set with intro and loop regions; `SoundFunc` alternates regions without IMM and checks call counts at pass lengths 1, 2, 5, 10, 50, 100, 250 and 500 ms at 48 kHz. |
| S4 | Counts `PlayerFunc` against timer.device EClock while changing `AHIA_PlayerFreq` through 25, 33.33, 50, 60, 75, 100 and 125 Hz; every measured rate must be within 5 percent. |
| S5 | A 50 Hz `PlayerFunc` script issues IMM and queued `SetSound`, `SetFreq` and `SetVol`, then a queued stop; `SoundFunc` timestamps check pass-end and immediate transitions. |
| S6 | Plays an ascending glide forward and backward in 8-bit and 16-bit mono; negative lengths start at the last sample, and pass counts must agree with the requested rate. |
| S7 | Loads and plays M8S, M16S, S8S and S16S; a hardware-mode `AHIE_BADSAMPLETYPE` for stereo is reported without failing, since firmware can withhold its stereo flag. |
| S8 | Stops and restarts with `AHIC_Play`, then pauses channel 0 with frequency 0 and resumes it; checks player timing and, when supported, frozen and moving channel offsets. |
| S9 | Unloads a playing sound, checks that a separate channel continues, reloads and restarts it, then unloads the other playing sound. |
| S10 | Repeats AllocAudio, LoadSound, a short tone, stop, UnloadSound and FreeAudio, 50 times by default. |

Default channel counts are S1: 8, S2: 32, S3: 1, S4: 1, S5: 1, S6: 1,
S7: 1, S8: 4, S9: 2 and S10: 2. In a software mode a default above 8 is
reduced to 8, for comparison with the existing ZZ9000AX modes. An explicit
`CHANNELS` is not reduced. S5, S6 and S7 use one channel regardless of this
option; S8 and S9 need at least two. S9 plays two channels even if more are
allocated. A mode that cannot supply the requested channels refuses the
allocation rather than silently reducing it.

S3 compares SoundFunc counts against elapsed EClock time with a tolerance
of 5 percent or two calls, whichever is greater. The segment lengths are
frames, so a mix rate other than 48 kHz changes their duration; the rate
check uses the actual frequency. S4 allows time for each frequency change
to settle before measuring. Its allocation declares the minimum and maximum
player frequencies, as required by AHI.

S2 uses full per-channel volume and a sample amplitude below full scale.
AHI divides channels by the channel count at master volume 1.0; the test
does not boost the master above that level. Listen for a clean, steady
chord and a 6 dB reduction at master 0.5. S5 should play a centred tone,
then a lower, quieter tone on the left at a pass boundary, then a higher
tone immediately on the right, then silence. S6's forward glides rise and
the backward glides fall. S7's stereo tones differ between the speakers.
S8 prints its steps before each change so silence, a missing voice, and
resumption can be heard.

AHI does not specify what a channel plays after its sound is unloaded.
S9 does not assert a particular result for that channel. It checks that
the unrelated channel continues and that the sound can be loaded and
played again. Sample memory stays allocated until after FreeAudio.

## Hooks and cleanup

The PlayerFunc and SoundFunc hooks only touch the program's counters and
sample plans and call AHI channel functions. Timing, printing, memory
allocation and ENV access happen in the main task. SoundFunc always clears
`AHISF_IMM`. Queued changes in S5 come from PlayerFunc; all channel calls
outside hooks set IMM, as the AHI developer guide requires.

Effects are cancelled before FreeAudio, loaded sounds are unloaded, and
sample buffers are freed only after the audio control has been released.
Only static sample types are used. S3 and S5 keep their regions inside a
single sound, using frame offsets instead of loading each region as a
separate sound.

## Driver statistics and exit codes

After each FreeAudio the program reads the global ENV variable
`ZZ9K_AHI_VOICE_STATS` and prints the line:

```text
late=N rejected=N underruns=N dropped=N ringfull=N ticks=N frames=N ...
```

The driver appends `resyncs`, `fifodrops`, `recorddrops`, `overload`,
`features`, `ramp` and `limiter`. `features` is the firmware features word in
decimal, `ramp` the ramp length of the last Start in frames, and `limiter` 1
when the soft limiter was switched on. The tool prints the whole line.

It deletes the old value before each allocation so it cannot mistake stale
counters for the current session. Hardware mode must publish a new value.
Nonzero `late`, `rejected`, `underruns` or `dropped` fails the scenario.
`ringfull`, `ticks` and `frames` are printed for diagnosis but are not failure
conditions. S10 checks every session, not just its final one. In a software
mode a missing variable prints `no voice stats (software mode)`.

| Exit | Meaning |
| --- | --- |
| 0 | Every selected scenario passed its automated checks. |
| 5 | A scenario failed, a bad counter was reported, or Ctrl-C interrupted the run. |
| 10 | ahi.device or an audio allocation could not be opened. |
| 20 | Invalid command arguments. |

When AllocAudio fails, the driver prints the reason on the debug channel.
The mode may be refused because there is no voice engine, the session is
busy, or the card is on Zorro II. Unsupported channel counts, missing mode
files or insufficient memory can also prevent allocation. There is no
fallback to a software mode inside a hardware test. If an S10 allocation
fails, the program reports the cycle number and returns 10.

Without ahi.device or a card, the program should exit with 10 without a
CPU fault. The startup check used by CI is:

```sh
tools/amiga-docker.sh . tools/smoke-m68k-vamos.sh ahi/voicetest/ZZAXVoiceTest
```

The hook and sample rules follow the [AHI autodocs](https://www.lysator.liu.se/ahi/v4-site/files/ahi/doc/ahi.html),
[Declaring Sounds](https://www.lysator.liu.se/ahi/v4-site/files/ahi/doc/ahidev_9.html)
and [Making Noise](https://www.lysator.liu.se/ahi/v4-site/files/ahi/doc/ahidev_10.html).

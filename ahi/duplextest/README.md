# ZZAXDuplexTest

`ZZAXDuplexTest` checks the ZZ9000AX low-level AHI full-duplex path. It
allocates one `AHIAudioCtrl`, starts recording and playback together, and saves
the captured samples. Use it for the hardware gate; a separate AHI player and
AHI Record use separate exclusive low-level allocations and do not test this
path.

The program requires the `ZZ9000AX:16 bit Stereo++` mode. It selects that mode
only when it reports recording, full-duplex, stereo, and 16-bit support.

## Build

Run the following command from this directory:

```sh
./build.sh
```

The script builds `ZZAXDuplexTest` with `m68k-amigaos-gcc`. When that compiler
is absent, it runs the same command through `tools/amiga-docker.sh`.

## Run the standard capture

1. Connect a stereo source to the ZZ9000AX RCA inputs.
2. Set both auxiliary jumpers to `IN`.
3. Close other AHI and MHI clients.
4. On the Amiga, run:

   ```text
   ZZAXDuplexTest RAM:zzax-duplex.wav 5 >RAM:zzax-duplex.txt
   ```

The default mode plays the generated stereo test tone while recording. The
program writes a 48 kHz, 16-bit stereo WAV file and prints `result=PASS` on
success. During a successful run, the test tone and the RCA source remain
audible, and the WAV file contains the RCA input on both channels without the
generated playback tone or excess noise.

Retain the WAV file and `RAM:zzax-duplex.txt` for the production hardware
gate.

## Command interface

```text
ZZAXDuplexTest [output [seconds 1-10 [ceiling|paula-cross]]]
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `output` | `RAM:zzax-duplex.wav` | Destination for the captured samples. |
| `seconds` | `5` | Capture duration. Values from `1` through `10` are accepted. |
| mode | default | Selects the capture method. Use `ceiling` or `paula-cross`; omit it for the standard WAV capture. |

The standard mode writes a WAV file. `ceiling` and `paula-cross` write raw 48
kHz S16BE stereo samples, so give those captures a `.raw` name.

## Ceiling capture mode

Run ceiling mode when measuring the input ceiling with AHI playback from the
same `AHIAudioCtrl` that records:

```text
ZZAXDuplexTest RAM:cap_0159.raw 5 ceiling
```

This mode loops a coherent 1 kHz, 0.99-full-scale stereo sine wave through the
AHI playback channel. Loop the line-out RCA pair to line-in, set the baseline
to Paula 0 and AX 254, and run the command once for each prefactor step. Do
not start a Paula player for this measurement.

Use the raw capture with `analyze_audio_saturation.py` as required by the
qualification procedure.

## Paula cross-channel capture mode

AHI recording takes its input from the physical ADC before `St Mixer1`, so a
same-channel Paula loopback cannot isolate the intended route. `paula-cross`
uses Paula channel 1, the left output, for a roughly 998.6 Hz PAL tone while
AHI records. The command prints `paula_start=PASS channel=left period=222`
before capture begins.

```text
ZZAXDuplexTest RAM:cap_a205_paula_isolation.raw 5 paula-cross
```

For the A205 measurement, loop the line-out RCA pair to line-in and keep the
auxiliary jumpers set to `IN`. Analyse channel 1 against channel 2:

```text
ZZAXDuplexTest RAM:cap_a205_paula_0080.raw 5 paula-cross
python util/analyze_audio_saturation.py --auto-tone --channel 1 \
    --reference-channel 2 cap_a205_paula_0080.raw
```

The source starts recording with `AHIC_Play` disabled in this mode; the Paula
tone supplies the playback signal. This distinguishes the Paula route from the
AHI playback path while retaining the same low-level AHI recording control.

## Limits and failure conditions

The program requests a 48 kHz mix frequency, one channel, one sound, and an
`AHIA_RecordFunc` callback. It stops playback and recording before cleanup.
It rejects durations outside 1–10 seconds and mode names other than `ceiling`
and `paula-cross`.

If `AHI_AllocAudioA` fails, close other AHI and MHI clients. The program also
fails if it cannot find a compatible `ZZ9000AX:16 bit Stereo++` mode, start
capture, stop capture, or write the output file.

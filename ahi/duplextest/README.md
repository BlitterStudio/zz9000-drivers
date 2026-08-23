# ZZAXDuplexTest

`ZZAXDuplexTest` validates true low-level AHI full duplex: one
`AHIAudioCtrl` starts playback and recording together. It is intentionally
different from opening AHI Record while another player owns the card, which
uses two exclusive low-level allocations.

Build from this directory with:

```sh
./build.sh
```

On the Amiga, connect a stereo source to the ZZ9000AX RCA inputs, set both
auxiliary jumpers to `IN`, close every other AHI/MHI client, and run:

```text
ZZAXDuplexTest RAM:zzax-duplex.wav 5 >RAM:zzax-duplex.txt
```

During the five-second run:

- a 400 Hz left / 600 Hz right test tone must remain audible;
- the connected RCA source must remain audible through the normal analogue
  pass-through;
- the command must print `result=PASS`;
- `RAM:zzax-duplex.wav` must contain the connected RCA source in both
  channels, without the generated playback tone or excess noise.

Copy both output files back for the production hardware gate.

## Ceiling capture mode

AHI's low-level device allocation is exclusive: `AHIRecord` plus a
separate AHI player cannot use ZZ9000AX simultaneously. Ceiling mode
uses this tool's single `AHIAudioCtrl` for playback and capture:

```text
ZZAXDuplexTest RAM:cap_0159.raw 5 ceiling
```

In ceiling mode the playback channel loops a coherent 1 kHz stereo
sine at 0.99 full scale and the capture is written as raw 48 kHz
S16BE stereo, ready for `analyze_audio_saturation.py`. For the primary
ceiling sweep, set the operator baseline to Paula 0 / AX 254 and run
this command once per prefactor step; do not start a Paula player.
The line-out RCA pair must be looped to line-in and both auxiliary
jumpers set to `IN`. The separate Paula-only cross-check uses baseline
254 / 0, a Paula sample player, and this tool's capture-only mode.

For the Paula-only cross-check, start the Paula sample player and run
record-only mode:

```text
ZZAXDuplexTest RAM:cap_paula.raw 5 capture
```

Capture mode starts `AHIC_Record` without AHI playback and writes the
same raw 48 kHz S16BE format. It replaces AHIRecord's AIFF/WAVE output,
so the analyzer needs no container extraction step.

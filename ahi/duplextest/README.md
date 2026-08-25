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
jumpers set to `IN`.

## Paula cross-channel ceiling mode

A same-channel Paula loopback is invalid: AHI recording taps the
physical ADC before `St Mixer1`, so it sees the direct Paula feed as
well as the returned line output. `paula-cross` isolates them by
generating a deterministic 16-sample tone on Paula hardware channel 1
(left) while AHI records without playback.

First prove channel isolation with every line-output-to-auxiliary cable
disconnected:

```text
ZZAXDuplexTest RAM:cap_a205_paula_isolation.raw 5 paula-cross
```

It must print `paula_start=PASS channel=left period=222` before
`result=PASS`; the waveform is copied into chip-accessible DMA memory
and the start message proves `audio.device` accepted the write.

On the qualified R1 card the direct left Paula reference appears on
capture channel 2, while channel 1 is isolated by about 101 dB. After
confirming that mapping, identify which line-output RCA carries the
left-only tone with an amplifier, then connect only that active output
to auxiliary-input **left**. Leave the other line output and auxiliary
input right disconnected. The internal Paula wiring and auxiliary
capture ordering make this a cross-channel route in the capture domain.
With baseline Paula 254 / AX 0, the scene chain processes the left
Paula tone and the cable
returns that output into capture channel 1:

```text
ZZAXDuplexTest RAM:cap_a205_paula_0080.raw 5 paula-cross
python util/analyze_audio_saturation.py --auto-tone --channel 1 \
    --reference-channel 2 cap_a205_paula_0080.raw
```

Never use `paula-cross` with a normal same-channel stereo loopback.
The isolation capture is a mandatory setup gate before a sweep.

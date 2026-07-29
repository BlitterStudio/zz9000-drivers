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

/*
 * ZZ9000AX one-control AHI full-duplex validation tool
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <devices/ahi.h>
#include <devices/audio.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <proto/ahi.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <utility/hooks.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libamiga supplies the documented audio.device BeginIO() trampoline;
 * this NDK's Exec prototypes omit its C declaration. */
extern void BeginIO(struct IORequest *request);

#define MODE_NAME "ZZ9000AX:16 bit Stereo++"
#define MIX_FREQ 48000UL
#define DEFAULT_SECONDS 5UL
#define MAX_SECONDS 10UL
#define TONE_FRAMES 4800UL
#define FRAME_BYTES 4UL
#define PAULA_LEFT_MASK 2U
#define PAULA_PERIOD 222U
#define PAULA_VOLUME 64U

static const char version[] __attribute__((used)) =
    "\0$VER: ZZAXDuplexTest 1.4 (25.08.2026)";


/* One coherent 1 kHz cycle at 48 kHz, 0.99 full scale. Ceiling mode
 * repeats this table in both channels through the same AHIAudioCtrl
 * that owns capture, avoiding a second exclusive AHI allocation. */
static const WORD ceiling_sine[48] = {
       0,   4234,   8396,  12414,  16220,  19748,  22938,  25736,
   28093,  29970,  31334,  32162,  32439,  32162,  31334,  29970,
   28093,  25736,  22938,  19748,  16220,  12414,   8396,   4234,
       0,  -4234,  -8396, -12414, -16220, -19748, -22938, -25736,
  -28093, -29970, -31334, -32162, -32439, -32162, -31334, -29970,
  -28093, -25736, -22938, -19748, -16220, -12414,  -8396,  -4234
};

/* One 16-sample 1 kHz cycle at the nominal 16 kHz Paula rate. Channel
 * mask 2 is hardware channel 1, a left output. The integer Paula
 * period realizes ~998.6 Hz on PAL; host analysis detects it. */
static const BYTE paula_sine[16] = {
     0,  48,  89, 116, 126, 116,  89,  48,
     0, -48, -89,-116,-126,-116, -89, -48
};
struct Library *AHIBase;

struct capture_context {
  UBYTE *buffer;
  ULONG capacity_frames;
  volatile ULONG frames;
  volatile ULONG callbacks;
  volatile ULONG type_errors;
};

static ULONG record_hook(struct Hook *hook asm("a0"),
                         struct AHIAudioCtrl *audioctrl asm("a2"),
                         struct AHIRecordMessage *message asm("a1"))
{
  struct capture_context *context =
      (struct capture_context *)hook->h_Data;
  ULONG remaining;
  ULONG frames;

  (void)audioctrl;

  if (!context || !message) return 0;

  context->callbacks++;
  if (context->frames >= context->capacity_frames) {
    return 0;
  }

  if (message->ahirm_Type != AHIST_S16S) {
    context->type_errors++;
    return 0;
  }

  remaining = context->capacity_frames - context->frames;
  frames = message->ahirm_Length;
  if (frames > remaining) {
    frames = remaining;
  }

  if (frames) {
    CopyMem(message->ahirm_Buffer,
            context->buffer + context->frames * FRAME_BYTES,
            frames * FRAME_BYTES);
    context->frames += frames;
  }

  return 0;
}

static void put_le16(UBYTE *dest, UWORD value)
{
  dest[0] = (UBYTE)(value & 0xffU);
  dest[1] = (UBYTE)(value >> 8);
}

static void put_le32(UBYTE *dest, ULONG value)
{
  dest[0] = (UBYTE)(value & 0xffUL);
  dest[1] = (UBYTE)((value >> 8) & 0xffUL);
  dest[2] = (UBYTE)((value >> 16) & 0xffUL);
  dest[3] = (UBYTE)((value >> 24) & 0xffUL);
}

static BOOL write_wave(const char *path, const WORD *samples, ULONG frames)
{
  UBYTE header[44];
  UBYTE output[4096];
  ULONG sample_count = frames * 2UL;
  ULONG sample_index = 0;
  ULONG data_bytes = frames * FRAME_BYTES;
  BPTR file;

  memset(header, 0, sizeof(header));
  memcpy(header + 0, "RIFF", 4);
  put_le32(header + 4, 36UL + data_bytes);
  memcpy(header + 8, "WAVEfmt ", 8);
  put_le32(header + 16, 16UL);
  put_le16(header + 20, 1U);
  put_le16(header + 22, 2U);
  put_le32(header + 24, MIX_FREQ);
  put_le32(header + 28, MIX_FREQ * FRAME_BYTES);
  put_le16(header + 32, FRAME_BYTES);
  put_le16(header + 34, 16U);
  memcpy(header + 36, "data", 4);
  put_le32(header + 40, data_bytes);

  file = Open((CONST_STRPTR)path, MODE_NEWFILE);
  if (!file) return FALSE;

  if (Write(file, header, sizeof(header)) != (LONG)sizeof(header)) {
    Close(file);
    DeleteFile((CONST_STRPTR)path);
    return FALSE;
  }

  while (sample_index < sample_count) {
    ULONG output_bytes = 0;

    while (sample_index < sample_count &&
           output_bytes + 2UL <= sizeof(output)) {
      UWORD sample = (UWORD)samples[sample_index++];
      output[output_bytes++] = (UBYTE)(sample & 0xffU);
      output[output_bytes++] = (UBYTE)(sample >> 8);
    }

    if (Write(file, output, output_bytes) != (LONG)output_bytes) {
      Close(file);
      DeleteFile((CONST_STRPTR)path);
      return FALSE;
    }
  }

  if (!Close(file)) {
    DeleteFile((CONST_STRPTR)path);
    return FALSE;
  }
  return TRUE;
}

static BOOL write_raw_be(const char *path, const WORD *samples, ULONG frames)
{
  UBYTE output[4096];
  ULONG sample_count = frames * 2UL;
  ULONG sample_index = 0;
  BPTR file = Open((CONST_STRPTR)path, MODE_NEWFILE);

  if (!file) return FALSE;
  while (sample_index < sample_count) {
    ULONG output_bytes = 0;

    while (sample_index < sample_count &&
           output_bytes + 2UL <= sizeof(output)) {
      UWORD sample = (UWORD)samples[sample_index++];
      output[output_bytes++] = (UBYTE)(sample >> 8);
      output[output_bytes++] = (UBYTE)(sample & 0xffU);
    }
    if (Write(file, output, output_bytes) != (LONG)output_bytes) {
      Close(file);
      DeleteFile((CONST_STRPTR)path);
      return FALSE;
    }
  }
  if (!Close(file)) {
    DeleteFile((CONST_STRPTR)path);
    return FALSE;
  }
  return TRUE;
}

static ULONG find_mode(void)
{
  ULONG id = AHI_INVALID_ID;

  while ((id = AHI_NextAudioID(id)) != AHI_INVALID_ID) {
    char name[80];
    LONG record = FALSE;
    LONG duplex = FALSE;
    LONG stereo = FALSE;
    LONG bits = 0;
    struct TagItem attrs[] = {
      { AHIDB_BufferLen, sizeof(name) },
      { AHIDB_Name, (ULONG)name },
      { AHIDB_Record, (ULONG)&record },
      { AHIDB_FullDuplex, (ULONG)&duplex },
      { AHIDB_Stereo, (ULONG)&stereo },
      { AHIDB_Bits, (ULONG)&bits },
      { TAG_DONE, 0 }
    };

    name[0] = '\0';
    if (AHI_GetAudioAttrsA(id, NULL, attrs) &&
        strcmp(name, MODE_NAME) == 0 &&
        record && duplex && stereo && bits == 16) {
      return id;
    }
  }

  return AHI_INVALID_ID;
}

static void make_tone(WORD *samples, BOOL ceiling_mode)
{
  ULONG frame;

  for (frame = 0; frame < TONE_FRAMES; frame++) {
    if (ceiling_mode) {
      WORD sample = ceiling_sine[frame % 48UL];
      samples[frame * 2UL] = sample;
      samples[frame * 2UL + 1UL] = sample;
    } else {
      samples[frame * 2UL] =
          ((frame / 60UL) & 1UL) ? (WORD)0x1800 : (WORD)-0x1800;
      samples[frame * 2UL + 1UL] =
          ((frame / 40UL) & 1UL) ? (WORD)0x1000 : (WORD)-0x1000;
    }
  }
}

int main(int argc, char **argv)
{
  const char *output_path = "RAM:zzax-duplex.wav";
  ULONG seconds = DEFAULT_SECONDS;
  ULONG target_frames;
  ULONG timeout_ticks;
  ULONG ticks = 0;
  ULONG audio_id = AHI_INVALID_ID;
  ULONG actual_mix_freq = 0;
  ULONG control_result = AHIE_UNKNOWN;
  ULONG stop_result = AHIE_UNKNOWN;
  BOOL aborted = FALSE;
  BOOL output_written = FALSE;
  BOOL ceiling_mode = FALSE;
  BOOL paula_cross_mode = FALSE;
  BOOL sound_loaded = FALSE;
  BOOL started = FALSE;
  int result = RETURN_FAIL;
  struct MsgPort *port = NULL;
  struct AHIRequest *request = NULL;
  BYTE device_open = -1;
  struct AHIAudioCtrl *audioctrl = NULL;
  struct capture_context context;
  struct Hook hook;
  WORD *tone = NULL;
  struct AHISampleInfo tone_info;
  struct MsgPort *paula_port = NULL;
  BYTE *paula_tone = NULL;
  struct IOAudio *paula_io = NULL;
  BYTE paula_device_open = -1;
  BOOL paula_started = FALSE;
  static UBYTE paula_channel[] = { PAULA_LEFT_MASK };

  memset(&context, 0, sizeof(context));
  memset(&hook, 0, sizeof(hook));
  memset(&tone_info, 0, sizeof(tone_info));

  if (argc > 1) output_path = argv[1];
  if (argc > 2) {
    LONG parsed = atol(argv[2]);
    if (parsed < 1 || parsed > (LONG)MAX_SECONDS) {
      printf("Usage: ZZAXDuplexTest [output [seconds 1-%u "
             "[ceiling|paula-cross]]]\n", (unsigned int)MAX_SECONDS);
      goto cleanup;
    }
    seconds = (ULONG)parsed;
  }
  if (argc > 3) {
    if (strcmp(argv[3], "ceiling") == 0)
      ceiling_mode = TRUE;
    else if (strcmp(argv[3], "paula-cross") == 0)
      paula_cross_mode = TRUE;
    else {
      printf("ERROR: mode must be 'ceiling' or 'paula-cross'\n");
      goto cleanup;
    }
  }
  if (argc > 4) {
    printf("Usage: ZZAXDuplexTest [output [seconds 1-%u "
           "[ceiling|paula-cross]]]\n", (unsigned int)MAX_SECONDS);
    goto cleanup;
  }

  target_frames = seconds * MIX_FREQ;
  timeout_ticks = seconds * 100UL + 250UL;
  context.capacity_frames = target_frames;
  context.buffer = AllocVec(target_frames * FRAME_BYTES,
                            MEMF_PUBLIC | MEMF_CLEAR);
  if (!paula_cross_mode)
    tone = AllocVec(TONE_FRAMES * FRAME_BYTES, MEMF_PUBLIC | MEMF_CLEAR);
  if (!context.buffer || (!paula_cross_mode && !tone)) {
    printf("ERROR: unable to allocate audio buffers\n");
    goto cleanup;
  }
  if (!paula_cross_mode) {
    make_tone(tone, ceiling_mode);
  } else {
    paula_tone = AllocVec(sizeof(paula_sine), MEMF_CHIP);
    if (!paula_tone) {
      printf("ERROR: unable to allocate Paula chip-memory tone\n");
      goto cleanup;
    }
    CopyMem(paula_sine, paula_tone, sizeof(paula_sine));

    paula_port = CreateMsgPort();
    if (!paula_port) {
      printf("ERROR: unable to create Paula message port\n");
      goto cleanup;
    }
    paula_io = (struct IOAudio *)
        CreateIORequest(paula_port, sizeof(struct IOAudio));
    if (!paula_io) {
      printf("ERROR: unable to create Paula IOAudio request\n");
      goto cleanup;
    }
    paula_io->ioa_Request.io_Message.mn_Node.ln_Pri = 0;
    paula_io->ioa_Request.io_Flags = ADIOF_NOWAIT;
    paula_io->ioa_AllocKey = 0;
    paula_io->ioa_Data = paula_channel;
    paula_io->ioa_Length = sizeof(paula_channel);
    paula_device_open = OpenDevice((CONST_STRPTR)AUDIONAME, 0,
                                   (struct IORequest *)paula_io, 0);
    if (paula_device_open) {
      printf("ERROR: unable to allocate Paula left channel (%d)\n",
             (int)paula_device_open);
      goto cleanup;
    }
    if ((ULONG)paula_io->ioa_Request.io_Unit != PAULA_LEFT_MASK) {
      printf("ERROR: audio.device allocated unexpected channel mask %u\n",
             (unsigned int)(ULONG)paula_io->ioa_Request.io_Unit);
      goto cleanup;
    }

    paula_io->ioa_Request.io_Command = CMD_WRITE;
    paula_io->ioa_Request.io_Flags =
        ADIOF_PERVOL | ADIOF_WRITEMESSAGE;
    paula_io->ioa_WriteMsg.mn_ReplyPort = paula_port;
    paula_io->ioa_Data = (UBYTE *)paula_tone;
    paula_io->ioa_Length = sizeof(paula_sine);
    paula_io->ioa_Period = PAULA_PERIOD;
    paula_io->ioa_Volume = PAULA_VOLUME;
    paula_io->ioa_Cycles = 0;
    BeginIO((struct IORequest *)paula_io);
    WaitPort(paula_port);
    if (GetMsg(paula_port) != &paula_io->ioa_WriteMsg) {
      printf("ERROR: Paula CMD_WRITE failed to start (%d)\n",
             (int)paula_io->ioa_Request.io_Error);
      goto cleanup;
    }
    paula_started = TRUE;
    printf("paula_start=PASS channel=left period=%u\n",
           (unsigned int)PAULA_PERIOD);
    Delay(10);
  }

  port = CreateMsgPort();
  if (!port) {
    printf("ERROR: unable to create AHI message port\n");
    goto cleanup;
  }

  request = (struct AHIRequest *)
      CreateIORequest(port, sizeof(struct AHIRequest));
  if (!request) {
    printf("ERROR: unable to create AHI request\n");
    goto cleanup;
  }

  request->ahir_Version = 4;
  device_open = OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT,
                           (struct IORequest *)request, 0);
  if (device_open) {
    printf("ERROR: unable to open %s version 4 (%d)\n",
           AHINAME, (int)device_open);
    goto cleanup;
  }
  AHIBase = (struct Library *)request->ahir_Std.io_Device;

  audio_id = find_mode();
  if (audio_id == AHI_INVALID_ID) {
    printf("ERROR: full-duplex mode \"%s\" was not found\n", MODE_NAME);
    goto cleanup;
  }

  hook.h_Entry = (ULONG (*)())record_hook;
  hook.h_Data = &context;

  {
    struct TagItem alloc_tags[] = {
      { AHIA_AudioID, audio_id },
      { AHIA_MixFreq, MIX_FREQ },
      { AHIA_Channels, 1 },
      { AHIA_Sounds, 1 },
      { AHIA_RecordFunc, (ULONG)&hook },
      { AHIA_UserData, (ULONG)&context },
      { TAG_DONE, 0 }
    };

    audioctrl = AHI_AllocAudioA(alloc_tags);
  }
  if (!audioctrl) {
    printf("ERROR: AHI_AllocAudioA failed; close other AHI/MHI clients\n");
    goto cleanup;
  }

  if (!paula_cross_mode) {
    tone_info.ahisi_Type = AHIST_S16S;
    tone_info.ahisi_Address = tone;
    tone_info.ahisi_Length = TONE_FRAMES;
    if (AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, &tone_info, audioctrl) !=
        AHIE_OK) {
      printf("ERROR: AHI_LoadSound failed\n");
      goto cleanup;
    }
    sound_loaded = TRUE;
  }

  /* One AudioCtrl owns every AHI direction used here. Paula-cross
   * records only; its source is the independently allocated left
   * audio.device channel. */
  {
    struct TagItem start_tags[] = {
      { AHIC_Input, 0 },
      { AHIC_MixFreq_Query, (ULONG)&actual_mix_freq },
      { AHIC_Play, paula_cross_mode ? FALSE : TRUE },
      { AHIC_Record, TRUE },
      { TAG_DONE, 0 }
    };

    control_result = AHI_ControlAudioA(audioctrl, start_tags);
  }
  if (control_result != AHIE_OK) {
    printf("ERROR: AHI capture start failed (%u)\n",
           (unsigned int)control_result);
    goto cleanup;
  }
  started = TRUE;

  if (!paula_cross_mode) {
    AHI_SetVol(0, 0x10000L, 0x8000L, audioctrl,
               AHISF_IMM | AHISF_NODELAY);
    AHI_SetSound(0, 0, 0, TONE_FRAMES, audioctrl,
                 AHISF_IMM | AHISF_NODELAY);
    AHI_SetFreq(0, MIX_FREQ, audioctrl, AHISF_IMM | AHISF_NODELAY);
  }

  while (context.frames < target_frames && ticks < timeout_ticks) {
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      aborted = TRUE;
      break;
    }
    Delay(1);
    ticks++;
  }

  {
    struct TagItem stop_tags[] = {
      { AHIC_Play, FALSE },
      { AHIC_Record, FALSE },
      { TAG_DONE, 0 }
    };

    stop_result = AHI_ControlAudioA(audioctrl, stop_tags);
  }
  started = FALSE;

  if (context.frames) {
    output_written = (ceiling_mode || paula_cross_mode)
        ? write_raw_be(output_path, (const WORD *)context.buffer,
                       context.frames)
        : write_wave(output_path, (const WORD *)context.buffer,
                     context.frames);
  }

  printf("mode=%s\n", MODE_NAME);
  if (ceiling_mode)
    printf("capture_mode=ceiling-1khz-s16be\n");
  else if (paula_cross_mode)
    printf("capture_mode=paula-left-cross-s16be\n");
  printf("mode_id=0x%08x\n", (unsigned int)audio_id);
  printf("mix_freq=%u\n", (unsigned int)actual_mix_freq);
  printf("callbacks=%u\n", (unsigned int)context.callbacks);
  printf("captured_frames=%u\n", (unsigned int)context.frames);
  printf("target_frames=%u\n", (unsigned int)target_frames);
  printf("type_errors=%u\n", (unsigned int)context.type_errors);
  printf("start_result=%u\n", (unsigned int)control_result);
  printf("stop_result=%u\n", (unsigned int)stop_result);
  if (ceiling_mode || paula_cross_mode)
    printf("output=%s\n", output_written ? output_path : "NOT_WRITTEN");
  else
    printf("wave=%s\n", output_written ? output_path : "NOT_WRITTEN");

  if (!aborted &&
      actual_mix_freq == MIX_FREQ &&
      context.frames == target_frames &&
      context.type_errors == 0 &&
      stop_result == AHIE_OK &&
      output_written) {
    printf("result=PASS\n");
    result = RETURN_OK;
  } else {
    printf("result=FAIL%s\n", aborted ? " (aborted)" : "");
  }

cleanup:
  if (started && audioctrl) {
    struct TagItem stop_tags[] = {
      { AHIC_Play, FALSE },
      { AHIC_Record, FALSE },
      { TAG_DONE, 0 }
    };
    AHI_ControlAudioA(audioctrl, stop_tags);
  }
  if (sound_loaded && audioctrl) AHI_UnloadSound(0, audioctrl);
  if (audioctrl) AHI_FreeAudio(audioctrl);
  if (paula_started && paula_io) {
    AbortIO((struct IORequest *)paula_io);
    WaitPort(paula_port);
    GetMsg(paula_port);
    paula_started = FALSE;
  }
  if (!paula_device_open && paula_io)
    CloseDevice((struct IORequest *)paula_io);
  if (paula_io) DeleteIORequest((struct IORequest *)paula_io);
  if (paula_port) DeleteMsgPort(paula_port);
  if (!device_open && request)
    CloseDevice((struct IORequest *)request);
  if (paula_tone) FreeVec(paula_tone);
  if (request) DeleteIORequest((struct IORequest *)request);
  if (port) DeleteMsgPort(port);
  if (tone) FreeVec(tone);
  if (context.buffer) FreeVec(context.buffer);

  return result;
}

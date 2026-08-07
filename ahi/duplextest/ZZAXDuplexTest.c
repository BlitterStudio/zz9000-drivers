/*
 * ZZ9000AX one-control AHI full-duplex validation tool
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <devices/ahi.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <proto/ahi.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <utility/hooks.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODE_NAME "ZZ9000AX:16 bit Stereo++"
#define MIX_FREQ 48000UL
#define DEFAULT_SECONDS 5UL
#define MAX_SECONDS 10UL
#define TONE_FRAMES 4800UL
#define FRAME_BYTES 4UL

static const char version[] __attribute__((used)) =
    "\0$VER: ZZAXDuplexTest 1.1 (07.08.2026)";

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

static void make_tone(WORD *samples)
{
  ULONG frame;

  for (frame = 0; frame < TONE_FRAMES; frame++) {
    samples[frame * 2UL] =
        ((frame / 60UL) & 1UL) ? (WORD)0x1800 : (WORD)-0x1800;
    samples[frame * 2UL + 1UL] =
        ((frame / 40UL) & 1UL) ? (WORD)0x1000 : (WORD)-0x1000;
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
  BOOL wave_written = FALSE;
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

  memset(&context, 0, sizeof(context));
  memset(&hook, 0, sizeof(hook));
  memset(&tone_info, 0, sizeof(tone_info));

  if (argc > 1) output_path = argv[1];
  if (argc > 2) {
    LONG parsed = atol(argv[2]);
    if (parsed < 1 || parsed > (LONG)MAX_SECONDS) {
      printf("Usage: ZZAXDuplexTest [output.wav [seconds 1-%u]]\n",
             (unsigned int)MAX_SECONDS);
      goto cleanup;
    }
    seconds = (ULONG)parsed;
  }

  target_frames = seconds * MIX_FREQ;
  timeout_ticks = seconds * 100UL + 250UL;
  context.capacity_frames = target_frames;
  context.buffer = AllocVec(target_frames * FRAME_BYTES,
                            MEMF_PUBLIC | MEMF_CLEAR);
  tone = AllocVec(TONE_FRAMES * FRAME_BYTES, MEMF_PUBLIC | MEMF_CLEAR);
  if (!context.buffer || !tone) {
    printf("ERROR: unable to allocate audio buffers\n");
    goto cleanup;
  }
  make_tone(tone);

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

  tone_info.ahisi_Type = AHIST_S16S;
  tone_info.ahisi_Address = tone;
  tone_info.ahisi_Length = TONE_FRAMES;
  if (AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, &tone_info, audioctrl) !=
      AHIE_OK) {
    printf("ERROR: AHI_LoadSound failed\n");
    goto cleanup;
  }
  sound_loaded = TRUE;

  /*
   * This is the full-duplex gate: one AHIAudioCtrl and one control operation
   * start playback and recording together. AHIRecord plus a separate player
   * is two exclusive low-level allocations and is deliberately not this test.
   */
  {
    struct TagItem start_tags[] = {
      { AHIC_Input, 0 },
      { AHIC_MixFreq_Query, (ULONG)&actual_mix_freq },
      { AHIC_Play, TRUE },
      { AHIC_Record, TRUE },
      { TAG_DONE, 0 }
    };

    control_result = AHI_ControlAudioA(audioctrl, start_tags);
  }
  if (control_result != AHIE_OK) {
    printf("ERROR: simultaneous play+record start failed (%u)\n",
           (unsigned int)control_result);
    goto cleanup;
  }
  started = TRUE;

  AHI_SetVol(0, 0x10000L, 0x8000L, audioctrl,
             AHISF_IMM | AHISF_NODELAY);
  AHI_SetSound(0, 0, 0, TONE_FRAMES, audioctrl,
               AHISF_IMM | AHISF_NODELAY);
  AHI_SetFreq(0, MIX_FREQ, audioctrl, AHISF_IMM | AHISF_NODELAY);

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
    wave_written = write_wave(output_path, (const WORD *)context.buffer,
                              context.frames);
  }

  printf("mode=%s\n", MODE_NAME);
  printf("mode_id=0x%08x\n", (unsigned int)audio_id);
  printf("mix_freq=%u\n", (unsigned int)actual_mix_freq);
  printf("callbacks=%u\n", (unsigned int)context.callbacks);
  printf("captured_frames=%u\n", (unsigned int)context.frames);
  printf("target_frames=%u\n", (unsigned int)target_frames);
  printf("type_errors=%u\n", (unsigned int)context.type_errors);
  printf("start_result=%u\n", (unsigned int)control_result);
  printf("stop_result=%u\n", (unsigned int)stop_result);
  printf("wave=%s\n", wave_written ? output_path : "NOT_WRITTEN");

  if (!aborted &&
      actual_mix_freq == MIX_FREQ &&
      context.frames == target_frames &&
      context.type_errors == 0 &&
      stop_result == AHIE_OK &&
      wave_written) {
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
  if (!device_open && request)
    CloseDevice((struct IORequest *)request);
  if (request) DeleteIORequest((struct IORequest *)request);
  if (port) DeleteMsgPort(port);
  if (tone) FreeVec(tone);
  if (context.buffer) FreeVec(context.buffer);

  return result;
}

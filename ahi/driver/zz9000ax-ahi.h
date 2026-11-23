#include <exec/semaphores.h>
#include <devices/timer.h>
#include <zz9k/abi.h>
#include <zz9k/voice.h>

#include "voice_model.h"

struct z9ax_voice;

// Driver data
struct z9ax {
  struct Task *t_mainproc;
  struct Library *ahi_base;
  struct Process *worker_process;
  struct Interrupt irq;
  struct Interrupt irq_fabric_token;
  uint32_t hw_addr;
  uint32_t audio_buf_addr;
  uint32_t record_buf_addr;
  uint32_t audio_hw_buf_addr;
  uint32_t audio_rx_hw_buf_addr;
  uint32_t buf_offset;
  uint16_t play_sequence;
  uint16_t record_sequence;
  int8_t mainproc_signal;
  int8_t worker_signal;
  int8_t enable_signal;
  uint32_t mix_freq;
  /* Software player clock, independent of the fixed 20 ms transport.
   * Remainder/player_freq is the fractional frame carried to the next tick. */
  uint32_t player_frames_left;
  uint32_t player_remainder;
  uint32_t player_freq;
  int32_t monitor_volume, input_gain, output_volume;
  uint16_t disable_cnt;
  uint8_t zorro_version;
  struct AHIAudioCtrlDrv *audioctrl;
  struct AHIRecordMessage record_message;
  uint16_t play_stop;
  uint16_t record_stop;
  uint32_t play_transport_generation; /* Stop epoch guarding Start acquire */
  uint8_t flags;
  uint8_t irq_installed;
  uint8_t fabric_token_installed;
  uint8_t record_capable;
  uint8_t tx_status_capable;
  /* Audio-fabric lease mode (AHI migration). fabric_mode: the card
   * advertised ZZ9K_CAP_AUDIO_FABRIC plus the audio service's
   * FABRIC_RATE flag at allocate, so playback runs as a direct-ring
   * lease producer (card-side conversion at mix_freq) instead of the
   * legacy register path. lease_held tracks the current grant; Stop
   * surrenders it and increments play_transport_generation so a
   * blocking Start/recovery acquire cannot undo the completed Stop. */
  uint8_t fabric_mode;
  uint8_t lease_held;
  uint8_t lease_source_be; /* firmware accepted m68k S16BE source */
  uint8_t lease_acquire_in_progress; /* serializes Start/recovery install */
  uint8_t lease_acquire_uncertain; /* timed-out acquire may have applied */
  uint8_t lease_release_pending; /* blocks acquire until confirmed */
  uint8_t lease_release_in_progress; /* serializes mailbox release */
  uint32_t lease_release_slot;
  uint32_t lease_release_generation;
  struct timeval lease_retry_deadline; /* elapsed-time recovery backoff */
  ZZ9KAudioRingSession lease_session;
  uint32_t z2_direct_ring_base; /* negotiated generation-2 carve-out */
  uint32_t z2_direct_ring_size;
  uint8_t *lease_accum;       /* whole-period staging accumulator */
  uint32_t lease_accum_fill;  /* bytes buffered in lease_accum */
  /* timer.device (UNIT_MICROHZ) lease-pacing timer; NULL in legacy
   * mode, created by the worker when fabric_mode is set. */
  struct MsgPort *lease_timer_port;
  struct timerequest *lease_timer_req;
  struct timeval lease_timer_deadline;
  int8_t lease_timer_signal;
  /* Hardware-mixing mode (Z9AX_VOICE_*); voice is NULL otherwise. */
  uint8_t voice_mode;
  struct z9ax_voice *voice;
};

/* Hardware-mixing modes ($CAFE0002 and $CAFE0003). ahi.device does no mixing
 * there: the driver maps every AHI channel to a voice of the card's voice
 * engine (see docs/ahi-voice-mixing-spec.md). Everything below is owned by
 * the voice worker process, except the call FIFO inside the model and the
 * fields marked for other tasks. */
#define Z9AX_VOICE_NONE    0U
#define Z9AX_VOICE_NEAREST 1U
#define Z9AX_VOICE_LINEAR  2U

#define Z9AX_VOICE_CMD_NONE  0U
#define Z9AX_VOICE_CMD_START 1U
#define Z9AX_VOICE_CMD_STOP  2U

struct z9ax_voice {
  ZZ9KVoiceSession session;
  ZZ9KVoiceStatus status;      /* last status read */
  uint8_t session_open;
  uint8_t session_lost;        /* the card closed the session */
  uint8_t playing;
  uint8_t linear;
  uint8_t stereo_ok;           /* firmware advertises S8S/S16S voice samples */
  uint8_t zorro;
  uint8_t reason;              /* Z9AX_VOICE_REASON_* when the open failed */
  uint8_t pad;
  uint32_t open_status;        /* ZZ9K_STATUS_* of the failed step */
  uint32_t channels;
  uint32_t sounds;             /* AHIA_Sounds */
  uint32_t lead;               /* granted lead, engine frames */
  uint32_t lead_request;
  uint32_t bank_request;
  uint32_t start_frame;        /* engine frame of the first Start */
  uint32_t resyncs;            /* the model fell behind and was re-anchored */
  volatile uint32_t cmd;       /* set by Start and Stop for the worker */
  volatile uint32_t cmd_result;
  struct Task *cmd_task;
  int8_t cmd_signal;
  uint32_t unload_pending[VM_MAX_SOUNDS / 32U]; /* UnloadSound, any task */
  /* LoadSound's copy into the bank. The session belongs to the worker that
   * opened it, so the worker sends the mailbox requests: the client's task
   * fills the request under upload_lock and waits for the answer. */
  struct SignalSemaphore upload_lock; /* one LoadSound at a time */
  volatile uint8_t up_pending;        /* a request waits for the worker */
  struct Task *up_task;               /* the waiting task */
  int8_t up_signal;
  const uint8_t *up_data;             /* the rest of the sample */
  uint32_t up_length;                 /* bytes still to copy */
  uint32_t up_offset;                 /* bank offset of up_data */
  volatile uint32_t up_status;        /* ZZ9K_STATUS_* of the copy */
  struct AHIEffChannelInfo *channelinfo;        /* SetEffect, any task */
  uint32_t ring_full;          /* writes that took fewer records than asked */
  uint32_t status_failures;
  VmModel model;
};

// TW: Driver base includes hardware address and zorro version besides library base.
// Driver base
struct z9ax_base {
  struct Library ahisub_base;
  struct z9ax *owner;
  struct z9ax *allocating;
  uint32_t hw_addr;
  uint32_t hw_size;
  uint8_t zorro_version;
  uint8_t flags;
};

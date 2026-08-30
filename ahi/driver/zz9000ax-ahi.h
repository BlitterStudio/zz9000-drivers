#include <devices/timer.h>
#include <zz9k/abi.h>

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
  uint8_t lease_acquire_in_progress; /* serializes Start/recovery install */
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

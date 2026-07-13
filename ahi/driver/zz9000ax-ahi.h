// Driver data
struct z9ax {
  struct Task *t_mainproc;
  struct Library *ahi_base;
  struct Process *worker_process;
  struct Interrupt irq;
  uint32_t hw_addr;
  uint32_t audio_buf_addr;
  uint32_t record_buf_addr;
  uint32_t audio_hw_buf_addr;
  uint32_t audio_rx_hw_buf_addr;
  uint32_t buf_offset;
  uint16_t record_sequence;
  int8_t mainproc_signal;
  int8_t worker_signal;
  int8_t enable_signal;
  uint32_t mix_freq;
  int32_t monitor_volume, input_gain, output_volume;
  uint16_t disable_cnt;
  uint8_t zorro_version;
  struct AHIAudioCtrlDrv *audioctrl;
  uint16_t play_stop;
  uint16_t record_stop;
  struct AHIRecordMessage record_message;
  uint8_t flags;
  uint8_t irq_installed;
  uint8_t record_capable;
};

// TW: Driver base includes hardware address and zorro version besides library base.
// Driver base
struct z9ax_base {
  struct Library ahisub_base;
  uint32_t hw_addr;
  uint32_t hw_size;
  uint8_t zorro_version;
  uint8_t flags;
};

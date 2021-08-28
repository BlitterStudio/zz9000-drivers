struct z9ax {
	struct Task *t_mainproc;
	struct Library *ahi_base;
	struct Process *worker_process;
  struct Interrupt irq;
  uint32_t hw_addr;
  uint32_t audio_buf_addr;
	int8_t mainproc_signal;
	int8_t worker_signal;
	int8_t enable_signal;
	uint32_t mix_freq;
	int32_t monitor_volume, input_gain, output_volume;
	uint16_t disable_cnt;
	struct AHIAudioCtrlDrv *audioctrl;
};

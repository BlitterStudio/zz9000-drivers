struct z9ax {
	struct Task *t_mainproc;
	struct Library *ahi_base;
	struct Process *worker_process;
  struct Interrupt irq;
	int8_t mainproc_signal;
	int8_t worker_signal;
	int8_t play_signal;
	int8_t enable_signal;
	uint32_t mix_freq;
	int32_t monitor_volume, input_gain, output_volume;
	uint16_t buffer_type;
	uint32_t buffer_size;
	uint16_t disable_cnt;
	struct AHIAudioCtrlDrv *audioctrl;
};

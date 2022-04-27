#include <exec/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <utility/hooks.h>

#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <proto/timer.h>

#include <devices/timer.h>

#include <stdio.h>
#include <math.h>
#include <limits.h>

static const char version[] = "$VER: axtest 1.0\n\r";
static const char procname[] = "axtest";

#define REG_ZZ_CONFIG        0x04
#define REG_ZZ_AUDIO_SWAB    0x70
#define REG_ZZ_AUDIO_SCALE   0x74
#define REG_ZZ_AUDIO_PARAM   0x76
#define REG_ZZ_AUDIO_VAL     0x78
#define REG_ZZ_DECODER_PARAM 0x7A
#define REG_ZZ_DECODER_VAL   0x7C
#define REG_ZZ_DECODE        0x7E
#define REG_ZZ_AUDIO_CONFIG  0xF4

#define ZZ_BYTES_PER_PERIOD 3840
#define AUDIO_BUFSZ ZZ_BYTES_PER_PERIOD*8 // TODO: query from hardware
#define WORKER_PRIORITY 19 // 127
#define INTB_EXTER (13)  /* External interrupt */
#define INTB_VERTB (5)

long __oslibversion = 37;

struct Device *TimerBase;

struct z9ax {
	struct Task *t_mainproc;
	struct Process *worker_process;
  struct Interrupt irq;
  uint32_t hw_addr;
  uint32_t audio_buf_addr;
  uint32_t audio_hw_buf_addr;
	int8_t mainproc_signal;
	int8_t enable_signal;
	int8_t worker_signal;
  uint8_t zorro_version;
  size_t num_samples;
};

struct timeval time_start;

void time_reset() {
	GetSysTime(&time_start);
}

uint32_t time_get_ms() {
	struct timeval time_end;

	GetSysTime(&time_end);
	SubTime(&time_end, &time_start);

	return (time_end.tv_secs * 1000 + time_end.tv_micro / 1000);
}

uint32_t dev_isr(struct z9ax* ax asm("a1")) {
  USHORT status = *(USHORT*)(ax->hw_addr+0x04);

  // audio interrupt signal set?
  if (status & 2) {
    // ack/clear audio interrupt
    *(USHORT*)(ax->hw_addr+0x04) = 8|32;
    Signal(ax->t_mainproc, SIGBREAKF_CTRL_C);
  }

  if (status == 2) {
    return 1;
  } else {
    return 0;
  }
}

void init_interrupt(struct z9ax* ax) {
  ax->irq.is_Node.ln_Type = NT_INTERRUPT;
  ax->irq.is_Node.ln_Pri = 126;
  ax->irq.is_Node.ln_Name = "ZZ9000AX TEST";
  ax->irq.is_Data = ax;
  ax->irq.is_Code = (void*)dev_isr;

  Forbid();
  AddIntServer(INTB_EXTER, &ax->irq);
  Permit();
}

void enable_zz_irq(struct z9ax* ax) {
  // enable HW interrupt
  USHORT hw_config = 1;
  *(volatile USHORT*)(ax->hw_addr+REG_ZZ_AUDIO_CONFIG) = hw_config;
}

void disable_zz_irq(struct z9ax* ax) {
  // enable HW interrupt
  USHORT hw_config = 0;
  *(volatile USHORT*)(ax->hw_addr+REG_ZZ_AUDIO_CONFIG) = hw_config;
}

void destroy_interrupt(struct z9ax* ax) {
  Forbid();
  RemIntServer(INTB_EXTER, &ax->irq);
  Permit();
}

int main(int argc, char* argv[]) {
  struct ConfigDev* cd;
  struct z9ax ax;
  uint32_t hw_addr = 0x40000000;
  int zorro = 3;
  struct IORequest timereq;

  OpenDevice("timer.device", 0, &timereq, 0);
  TimerBase = timereq.io_Device;
  printf("TimerBase: %p\n", TimerBase);

  if ((ExpansionBase = (struct ExpansionBase*) OpenLibrary((STRPTR)"expansion.library", 0))) {
    // Find Z2 or Z3 model of MNT ZZ9000
    if ((cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x4))) {
      // ZORRO 3
      zorro = 3;
      fprintf(stderr, "ZZ9000 Zorro 3 Version detected.\n");
    }
    else if ((cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x3))) {
      // ZORRO 2
      zorro = 2;
      fprintf(stderr, "ZZ9000 Zorro 2 Version detected.\n");
    } else {
      fprintf(stderr, "Error: ZZ9000 not detected.\n");
      return 1;
    }
  } else {
    fprintf(stderr, "Error: Can't open expansion.library.\n");
    return 1;
  }

  CloseLibrary((struct Library*)ExpansionBase);
  hw_addr = (uint32_t)cd->cd_BoardAddr;
  int ax_present = *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_CONFIG));
  if (ax_present) {
    fprintf(stderr, "ZZ9000AX detected.\n");
  } else {
    fprintf(stderr, "Error: ZZ9000AX not detected.\n");
    return 1;
  }

  ax.hw_addr = hw_addr;
  ax.audio_hw_buf_addr = hw_addr + 0;

  // set tx buffer address to 0 MB offset (beginning of framebuffer)
  *((volatile uint16_t*)(hw_addr+0x76)) = 0;
  *((volatile uint16_t*)(hw_addr+0x78)) = 0x0000;
  *((volatile uint16_t*)(hw_addr+0x76)) = 1;
  *((volatile uint16_t*)(hw_addr+0x78)) = 0x0000;

  ax.t_mainproc = FindTask(NULL);

  disable_zz_irq(&ax);
  init_interrupt(&ax);

  time_reset();
  enable_zz_irq(&ax);
  for (int i=0; i<1024; i++) {
    Wait(SIGBREAKF_CTRL_C);
    uint32_t ms = time_get_ms();
    time_reset();
    printf("[irq] %d:\t%u ms\n", i, ms);
  }
  disable_zz_irq(&ax);

  destroy_interrupt(&ax);
  CloseDevice(&timereq);

  return RETURN_OK;
}

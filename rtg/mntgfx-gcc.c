#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/input.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/initializers.h>
#include <clib/debug_protos.h>
#include <devices/inputevent.h>
#include <string.h>
#include <stdint.h>

#include "mntgfx-gcc.h"
#include "zz9000.h"

#define STR(s) #s
#define XSTR(s) STR(s)

struct GFXBase {
	struct Library libNode;
	BPTR segList;
	struct ExecBase* sysBase;
	struct ExpansionBase* expansionBase;
};

#define KPrintF(...)
#define __saveds__

#define DEVICE_VERSION 1
#define DEVICE_REVISION 9
#define DEVICE_PRIORITY 0
#define DEVICE_ID_STRING "$VER ZZ9000.card " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE
#define DEVICE_NAME "ZZ9000.card"
#define DEVICE_DATE "(30 July 2021)"

int __attribute__((no_reorder)) _start()
{
		return -1;
}

asm("romtag:                                    \n"
		"       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
		"       dc.l    romtag                  \n"
		"       dc.l    endcode                 \n"
		"       dc.b    "XSTR(RTF_AUTOINIT)"    \n"
		"       dc.b    "XSTR(DEVICE_VERSION)"  \n"
		"       dc.b    "XSTR(NT_LIBRARY)"      \n"
		"       dc.b    "XSTR(DEVICE_PRIORITY)" \n"
		"       dc.l    _device_name            \n"
		"       dc.l    _device_id_string       \n"
		"       dc.l    _auto_init_tables       \n"
		"endcode:                               \n");

char device_name[] = DEVICE_NAME;
char device_id_string[] = DEVICE_ID_STRING;

__saveds struct GFXBase* OpenLib(__REGA6(struct GFXBase *gfxbase));
BPTR __saveds CloseLib(__REGA6(struct GFXBase *gfxbase));
BPTR __saveds ExpungeLib(__REGA6(struct GFXBase *exb));
ULONG ExtFuncLib(void);
__saveds struct GFXBase* InitLib(__REGA6(struct ExecBase *sysbase),
																 __REGA0(BPTR seglist),
																 __REGD0(struct GFXBase *exb));

#define CLOCK_HZ 100000000

static struct GFXBase *_gfxbase;
const char *gfxname = "ZZ9000";
char dummies[128];

// Place scratch area right after framebuffer? Might be a horrible idea.
#define Z3_GFXDATA_ADDR  (0x3200000 - 0x10000)
#define Z3_TEMPLATE_ADDR (0x3210000 - 0x10000)
#define ZZVMODE_800x600 1
#define ZZVMODE_720x576 6

struct ExecBase *SysBase;
static LONG zorro_version = 0;
static LONG hwrev = 0;
static LONG fwrev_major = 0;
static LONG fwrev_minor = 0;
static LONG fwrev = 0;
static LONG scandoubler_800x600 = 0;

#ifdef DMARTG
static volatile struct GFXData *gfxdata;
MNTZZ9KRegs* registers;
#endif
MNTZZ9KRegs* registers;

void fix_vsync(MNTZZ9KRegs* registers) {
	// video control op: vsync
	*(volatile uint16_t*)((uint32_t)registers + 0x1000) = 0;
	*(volatile uint16_t*)((uint32_t)registers + 0x1002) = 1;
	*(volatile uint16_t*)((uint32_t)registers + 0x1004) = 5; // OP_VSYNC
	*(volatile uint16_t*)((uint32_t)registers + 0x1004) = 0;
	*(volatile uint16_t*)((uint32_t)registers + 0x1002) = 0;
}

uint16_t rtg_to_mnt[21] = {
	MNTVA_COLOR_8BIT,		// 0x00 -- None
	MNTVA_COLOR_8BIT,		// 0x01 -- 8BPP CLUT
	MNTVA_COLOR_NO_USE,		// 0x02 -- 24BPP RGB
	MNTVA_COLOR_NO_USE,		// 0x03 -- 24BPP BGR
	MNTVA_COLOR_NO_USE,		// 0x04 -- 16BPP R5G6B5PC
	MNTVA_COLOR_15BIT,		// 0x05 -- 15BPP R5G5B5PC
	MNTVA_COLOR_NO_USE,		// 0x06 -- 32BPP ARGB
	MNTVA_COLOR_NO_USE,		// 0x07 -- 32BPP ABGR
	MNTVA_COLOR_32BIT,		// 0x08 -- 32BPP RGBA
	MNTVA_COLOR_32BIT,		// 0x09 -- 32BPP BGRA
	MNTVA_COLOR_16BIT565,	// 0x0A -- 16BPP R5G6B5
	MNTVA_COLOR_15BIT,		// 0x0B -- 15BPP R5G5B5
	MNTVA_COLOR_NO_USE,		// 0x0C -- 16BPP B5G6R5PC
	MNTVA_COLOR_15BIT,		// 0x0D -- 15BPP B5G5R5PC
	MNTVA_COLOR_NO_USE,		// 0x0E -- YUV 4:2:2
	MNTVA_COLOR_NO_USE,		// 0x0F -- YUV 4:1:1
	MNTVA_COLOR_NO_USE,		// 0x10 -- YUV 4:1:1PC
	MNTVA_COLOR_NO_USE,		// 0x11 -- YUV 4:2:2 (Duplicate for some reason)
	MNTVA_COLOR_NO_USE,		// 0x12 -- YUV 4:2:2PC
	MNTVA_COLOR_NO_USE,		// 0x13 -- YUV 4:2:2 Planar
	MNTVA_COLOR_NO_USE,		// 0x14 -- YUV 4:2:2PC Planar
};

static inline void zzwrite16(volatile uint16_t* reg, uint16_t value) {
	*reg = value;
}

static inline void zzwrite32(volatile uint16_t* reg, uint32_t value) {
	uint16_t *v = (uint16_t *)&value;
	reg[0] = v[0];
	reg[1] = v[1];
}

#define ZZWRITE16(a, b) *a = b;

#define ZZWRITE32(b, c) \
	zzwrite16(b##_hi, ((uint16_t *)&c)[0]); \
	zzwrite16(b##_lo, ((uint16_t *)&c)[1]);

// useful for debugging
void waitclick() {
#define CIAAPRA ((volatile uint8_t*)0xbfe001)
	// bfe001 http://amigadev.elowar.com/read/ADCD_2.1/Hardware_Manual_guide/node012E.html
	while (!(*CIAAPRA & (1<<6))) {
		// wait for left mouse button pressed
	}
	while ((*CIAAPRA & (1<<6))) {
		// wait for left mouse button released
	}
}


__saveds struct GFXBase* __attribute__((used)) InitLib(__REGA6(struct ExecBase *sysbase),
													   __REGA0(BPTR seglist),
													   __REGD0(struct GFXBase *exb))
{
	_gfxbase = exb;
	SysBase = *(struct ExecBase **)4L;
	return _gfxbase;
}

__saveds struct GFXBase* __attribute__((used)) OpenLib(__REGA6(struct GFXBase *gfxbase))
{
	gfxbase->libNode.lib_OpenCnt++;
	gfxbase->libNode.lib_Flags &= ~LIBF_DELEXP;

	return gfxbase;
}

BPTR __saveds __attribute__((used)) CloseLib(__REGA6(struct GFXBase *gfxbase))
{
	gfxbase->libNode.lib_OpenCnt--;

	if (!gfxbase->libNode.lib_OpenCnt) {
		if (gfxbase->libNode.lib_Flags & LIBF_DELEXP) {
			return (ExpungeLib(gfxbase));
		}
	}
	return 0;
}

BPTR __saveds __attribute__((used)) ExpungeLib(__REGA6(struct GFXBase *exb))
{
	BPTR seglist;
	struct ExecBase *SysBase = *(struct ExecBase **)4L;

	if(!exb->libNode.lib_OpenCnt) {
		ULONG negsize, possize, fullsize;
		UBYTE *negptr = (UBYTE *)exb;

		seglist = exb->segList;

		Remove((struct Node *)exb);

		negsize	 = exb->libNode.lib_NegSize;
		possize  = exb->libNode.lib_PosSize;
		fullsize = negsize + possize;
		negptr  -= negsize;

		FreeMem(negptr, fullsize);
		return(seglist);
	}

	exb->libNode.lib_Flags |= LIBF_DELEXP;
	return 0;
}

ULONG ExtFuncLib(void)
{
	return 0;
}

#define LOADLIB(a, b) if ((a = (struct a*)OpenLibrary((STRPTR)b,0L))==NULL) { \
		KPrintF((STRPTR)"ZZ9000.card: Failed to load %s.\n", b); \
		return 0; \
	} \


int __attribute__((used)) FindCard(__REGA0(struct BoardInfo* b)) {
	struct ConfigDev* cd = NULL;
	struct ExpansionBase *ExpansionBase = NULL;
	struct DOSBase *DOSBase = NULL;
	struct IntuitionBase *IntuitionBase = NULL;
	struct ExecBase *SysBase = *(struct ExecBase **)4L;

	LOADLIB(ExpansionBase, "expansion.library");
	LOADLIB(DOSBase, "dos.library");
	LOADLIB(IntuitionBase, "intuition.library");

	zorro_version = 0;
	if ((cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x4))) zorro_version = 3;
	else if ((cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x3))) zorro_version = 2;
	
	// Find Z3 or Z2 model
	if (zorro_version>=2) {
		
		b->MemoryBase = (uint8_t*)(cd->cd_BoardAddr)+0x10000;
		if (zorro_version==2) {
			b->MemorySize = cd->cd_BoardSize-0x20000;
		} else {
			// 13.8 MB for Z3 (safety, will be expanded later)
			// one full HD screen @8bit ~ 2MB
			b->MemorySize = 0x3000000 - 0x10000;
#ifdef DMARTG
			gfxdata = (struct GFXData*)(((uint32_t)b->MemoryBase) + (uint32_t)Z3_GFXDATA_ADDR);
			memset((void *)gfxdata, 0x00, sizeof(struct GFXData));
#endif
		}
		b->RegisterBase = (void *)(cd->cd_BoardAddr);
#ifdef DMARTG
		registers = (MNTZZ9KRegs *)b->RegisterBase;
#endif
		hwrev = ((uint16_t*)b->RegisterBase)[0];
		fwrev = ((uint16_t*)b->RegisterBase)[0xC0/2];
		fwrev_major = fwrev >> 8;
		fwrev_minor = fwrev & 0xFF;

		KPrintF(device_id_string);
		KPrintF("ZZ9000.card: MNT ZZ9000 found. Zorro version %ld.\n", zorro_version);
		KPrintF("ZZ9000.card: HW Revision: %ld.\n", hwrev);
		KPrintF("ZZ9000.card: FW Revision Major: %ld.\n", fwrev_major);
		KPrintF("ZZ9000.card: FW Revision Minor: %ld.\n", fwrev_minor);

		if (fwrev_major <= 1 && fwrev_minor < 6) {
			char *alert = "\x00\x14\x14ZZ9000.card v1.9 needs at least firmware (BOOT.bin) v1.9.\x00\x00";
			DisplayAlert(RECOVERY_ALERT, (APTR)alert, 52);
			return 0;
		}

		MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;
		BPTR f;
		if ((f = Open((APTR)"ENV:ZZ9000-VCAP-800x600", MODE_OLDFILE))) {
			Close(f);
			KPrintF("ZZ9000.card: 800x600 60hz scandoubler mode.\n");
			scandoubler_800x600 = 1;
			registers->videocap_vmode = ZZVMODE_800x600; // 60hz
		} else {
			KPrintF("ZZ9000.card: 720x576 50hz scandoubler mode.\n");
			scandoubler_800x600 = 0;
			registers->videocap_vmode = ZZVMODE_720x576; // 50hz
		}
		
		return 1;
	} else {
		KPrintF("ZZ9000.card: MNT ZZ9000 not found!\n");
		return 0;
	}
}

int __attribute__((used)) InitCard(__REGA0(struct BoardInfo* b)) {
	int i;

	b->CardBase = (struct CardBase *)_gfxbase;
	b->ExecBase = SysBase;
	b->BoardName = "ZZ9000";
	b->BoardType = 14;
	b->PaletteChipType = PCT_S3ViRGE;
	b->GraphicsControllerType = GCT_S3ViRGE;

	b->Flags = BIF_INDISPLAYCHAIN | BIF_GRANTDIRECTACCESS | BIF_HARDWARESPRITE | BIF_FLICKERFIXER | BIF_VGASCREENSPLIT | BIF_PALETTESWITCH;
	b->RGBFormats = 1 | 2 | 512 | 1024 | 2048;
	b->SoftSpriteFlags = 0;
	b->BitsPerCannon = 8;

	for(i = 0; i < MAXMODES; i++) {
		b->MaxHorValue[i] = 8192;
		b->MaxVerValue[i] = 8192;
		b->MaxHorResolution[i] = 8192;
		b->MaxVerResolution[i] = 8192;
		b->PixelClockCount[i] = 1;
	}

	b->MemoryClock = CLOCK_HZ;

	//b->AllocCardMem = (void *)NULL;
	//b->FreeCardMem = (void *)NULL;
	b->SetSwitch = (void *)SetSwitch;
	b->SetColorArray = (void *)SetColorArray;
	b->SetDAC = (void *)SetDAC;
	b->SetGC = (void *)SetGC;
	b->SetPanning = (void *)SetPanning;
	b->CalculateBytesPerRow = (void *)CalculateBytesPerRow;
	b->CalculateMemory = (void *)CalculateMemory;
	b->GetCompatibleFormats = (void *)GetCompatibleFormats;
	b->SetDisplay = (void *)SetDisplay;

	b->ResolvePixelClock = (void *)ResolvePixelClock;
	b->GetPixelClock = (void *)GetPixelClock;
	b->SetClock = (void *)SetClock;

	b->SetMemoryMode = (void *)SetMemoryMode;
	b->SetWriteMask = (void *)SetWriteMask;
	b->SetClearMask = (void *)SetClearMask;
	b->SetReadPlane = (void *)SetReadPlane;

	b->WaitVerticalSync = (void *)WaitVerticalSync;
	//b->SetInterrupt = (void *)NULL;

	b->WaitBlitter = (void *)WaitBlitter;

	//b->ScrollPlanar = (void *)NULL;
	//b->UpdatePlanar = (void *)NULL;

	b->BlitPlanar2Chunky = (void *)BlitPlanar2Chunky;
	b->BlitPlanar2Direct = (void *)BlitPlanar2Direct;

	b->FillRect = (void *)FillRect;
	b->InvertRect = (void *)InvertRect;
	b->BlitRect = (void *)BlitRect;
	b->BlitTemplate = (void *)BlitTemplate;
	b->BlitPattern = (void *)BlitPattern;
	b->DrawLine = (void *)DrawLine;
	b->BlitRectNoMaskComplete = (void *)BlitRectNoMaskComplete;
	//b->EnableSoftSprite = (void *)NULL;

	//b->AllocCardMemAbs = (void *)NULL;
	b->SetSplitPosition = (void *)SetSplitPosition;
	//b->ReInitMemory = (void *)NULL;
	//b->WriteYUVRect = (void *)NULL;
	b->GetVSyncState = (void *)GetVSyncState;
	//b->GetVBeamPos = (void *)NULL;
	//b->SetDPMSLevel = (void *)NULL;
	//b->ResetChip = (void *)NULL;
	//b->GetFeatureAttrs = (void *)NULL;
	//b->AllocBitMap = (void *)NULL;
	//b->FreeBitMap = (void *)NULL;
	//b->GetBitMapAttr = (void *)NULL;

	b->SetSprite = (void *)SetSprite;
	b->SetSpritePosition = (void *)SetSpritePosition;
	b->SetSpriteImage = (void *)SetSpriteImage;
	b->SetSpriteColor = (void *)SetSpriteColor;

	//b->CreateFeature = (void *)NULL;
	//b->SetFeatureAttrs = (void *)NULL;
	//b->DeleteFeature = (void *)NULL;

	return 1;
}

// None of these five really have to do anything.
void SetDAC (__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) { }
void WaitBlitter (__REGA0(struct BoardInfo *b)) { }
void SetClock (__REGA0(struct BoardInfo *b)) { }
void SetMemoryMode (__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) { }
void SetWriteMask (__REGA0(struct BoardInfo *b), __REGD0(UBYTE mask)) { }
void SetClearMask (__REGA0(struct BoardInfo *b), __REGD0(UBYTE mask)) { }
void SetReadPlane (__REGA0(struct BoardInfo *b), __REGD0(UBYTE plane)) { }

// Optional dummy read for tricking the 68k cache on processors with occasional garbage output on screen
#ifdef DUMMY_CACHE_READ
	#define dmy_cache memcpy(dummies, (uint32_t *)(uint32_t)0x7F00000, 4);
#else
	#define dmy_cache
#endif

void init_modeline(MNTZZ9KRegs* registers, uint16_t w, uint16_t h, uint8_t colormode, uint8_t scalemode) {
	//int hmax, vmax, hstart, hend, vstart, vend;
	uint16_t mode = 0;
	//uint16_t polarity = 0;

	if (w == 1280 && h == 720) {
		mode = 0;
	} else if (w == 800 && h == 600) {
		mode = 1;
	} else if (w == 640 && h == 480) {
		mode = 2;
	} else if (w == 1024 && h == 768) {
		mode = 3;
	} else if (w == 1280 && h == 1024) {
		mode = 4;
	} else if (w == 1920 && h == 1080) {
		mode = 5;
	} else if (w == 720 && h == 576) {
		mode = 6;
	} else if (w == 640 && h == 512) {
		mode = 9;
	} else if (w == 1600 && h == 1200) {
		mode = 10;
	} else if (w == 2560 && h == 1440) {
		mode = 11;
	}

	zzwrite16(&registers->mode, mode|(colormode<<8)|(scalemode<<12));
}

void SetGC (__REGA0(struct BoardInfo *b), __REGA1(struct ModeInfo *mode_info), __REGD0(BOOL border)) {
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;
	uint16_t scale = 0;
	uint16_t w;
	uint16_t h;
	uint16_t colormode;

	b->ModeInfo = mode_info;
	b->Border = border;

	if (mode_info->Width < 320 || mode_info->Height < 200)
		return;

	colormode = rtg_to_mnt[b->RGBFormat];

	if (mode_info->Height >= 480 || mode_info->Width >= 640) {
		scale = 0;

		w = mode_info->Width;
		h = mode_info->Height;
	} else {
		// small doublescan modes are scaled 2x
		// and output as 640x480 wrapped in 800x600 sync
		scale = 3;

		w = 2 * mode_info->Width;
		h = 2 * mode_info->Height;
		if (h < 480)
			h=480;
	}

	init_modeline(registers, w, h, colormode, scale);
}

int setswitch = -1;
UWORD SetSwitch (__REGA0(struct BoardInfo *b), __REGD0(UWORD enabled)) {
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (enabled == 0) {
		// capture 24 bit amiga video to 0xe00000
		zzwrite16(&registers->pan_ptr_hi, 0xe0);

		if (scandoubler_800x600) {
			// slightly adjusted centering
			zzwrite16(&registers->pan_ptr_lo, 0x0bd0);
		} else {
			zzwrite16(&registers->pan_ptr_lo, 0x0000);
		}

		int w = 720;
		int h = 576;
		int colormode = MNTVA_COLOR_32BIT;
		int scalemode = 2; // vertical line doubling

		if (scandoubler_800x600) {
			w = 800;
			h = 600;
		}

		init_modeline(registers, w, h, colormode, scalemode);

		// firmware will remember the selected mode
		*(volatile uint16_t*)((uint32_t)registers + 0x1006) = 1; // capture mode

	} else {
		// rtg mode
		*(volatile uint16_t*)((uint32_t)registers + 0x1006) = 0; // capture mode

		SetGC(b, b->ModeInfo, b->Border);
	}

	// FIXME
	for (volatile int i=0; i<100; i++) {
		fix_vsync(registers);
	}

	return 1 - enabled;
}

void SetPanning (__REGA0(struct BoardInfo *b), __REGA1(UBYTE *addr), __REGD0(UWORD width), __REGD1(WORD x_offset), __REGD2(WORD y_offset), __REGD7(RGBFTYPE format)) {
	b->XOffset = x_offset;
	b->YOffset = y_offset;

#ifdef DMARTG
	dmy_cache
	gfxdata->offset[0] = ((uint32_t)addr - (uint32_t)b->MemoryBase);

	gfxdata->x[0] = x_offset;
	gfxdata->y[0] = y_offset;
	gfxdata->x[1] = width;
	gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8)rtg_to_mnt[format & 0xFF];
	zzwrite16(&registers->blitter_dma_op, OP_PAN);
#else
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;
	uint32_t offset = ((uint32_t)addr - (uint32_t)b->MemoryBase);

	zzwrite16(&registers->blitter_x1, b->XOffset);
	zzwrite16(&registers->blitter_y1, b->YOffset);
	zzwrite16(&registers->blitter_x2, width);
	zzwrite16(&registers->blitter_colormode, rtg_to_mnt[format & 0xFF]);
	zzwrite32(&registers->pan_ptr_hi, offset);
#endif
}

void SetColorArray (__REGA0(struct BoardInfo *b), __REGD0(UWORD start), __REGD1(UWORD num)) {
	if (!b->CLUT)
		return;
	
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;
	int j = start + num;

	if (start >= 256) {
		// Select secondary palette if start index is above 255
		*(volatile uint16_t*)((uint32_t)registers + 0x1000) = 1;
		*(volatile uint16_t*)((uint32_t)registers + 0x1004) = 18; // OP_PALETTE_SEL
		*(volatile uint16_t*)((uint32_t)registers + 0x1004) = 0; // NOP
	}
	
	for(int i = start; i < j; i++) {
		unsigned long xrgb = ((uint32_t)(i & 0xFF) << 24) | ((uint32_t)b->CLUT[(i & 0xFF)].Red << 16) | ((uint32_t)b->CLUT[(i & 0xFF)].Green << 8) | ((uint32_t)b->CLUT[(i & 0xFF)].Blue);

		*(volatile uint16_t*)((uint32_t)registers + 0x1000) = xrgb >> 16;
		*(volatile uint16_t*)((uint32_t)registers + 0x1002) = xrgb & 0xFFFF;
		*(volatile uint16_t*)((uint32_t)registers + 0x1004) = 3; // OP_PALETTE
		*(volatile uint16_t*)((uint32_t)registers + 0x1004) = 0; // NOP
	}

	if (start >= 256) {
		// Reset selected palette to 0
		*(volatile uint16_t*)((uint32_t)registers + 0x1000) = 0;
		*(volatile uint16_t*)((uint32_t)registers + 0x1004) = 18; // OP_PALETTE_SEL
		*(volatile uint16_t*)((uint32_t)registers + 0x1004) = 0; // NOP
	}
}

uint16_t calc_pitch_bytes(uint16_t w, uint16_t colormode) {
	uint16_t pitch = w;

	if (colormode == MNTVA_COLOR_15BIT) {
		pitch = w<<1;
	} else {
		pitch = w<<colormode;
	}
	return pitch;
}

uint16_t pitch_to_shift(uint16_t p) {
	if (p == 8192) return 13;
	if (p == 4096) return 12;
	if (p == 2048) return 11;
	if (p == 1024) return 10;
	if (p == 512)  return 9;
	if (p == 256)  return 8;
	return 0;
}

UWORD CalculateBytesPerRow (__REGA0(struct BoardInfo *b), __REGD0(UWORD width), __REGD7(RGBFTYPE format)) {
	if (!b)
		return 0;

	return calc_pitch_bytes(width, rtg_to_mnt[format]);
}

APTR CalculateMemory (__REGA0(struct BoardInfo *b), __REGA1(unsigned long addr), __REGD7(RGBFTYPE format)) {
	if (addr > (uint32_t)b->MemoryBase && addr < (((uint32_t)b->MemoryBase) + b->MemorySize)) {
		addr = (addr + 0x1000) & 0xFFFFF000;
	}

	return (APTR)addr;
}

ULONG GetCompatibleFormats (__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) {
	return 0xFFFFFFFF;
}

UWORD SetDisplay (__REGA0(struct BoardInfo *b), __REGD0(UWORD enabled)) {
	return 1;
}

LONG ResolvePixelClock (__REGA0(struct BoardInfo *b), __REGA1(struct ModeInfo *mode_info), __REGD0(ULONG pixel_clock), __REGD7(RGBFTYPE format)) {
	mode_info->PixelClock = CLOCK_HZ;
	mode_info->pll1.Clock = 0;
	mode_info->pll2.ClockDivide = 1;

	return 0;
}

ULONG GetPixelClock (__REGA0(struct BoardInfo *b), __REGA1(struct ModeInfo *mode_info), __REGD0(ULONG index), __REGD7(RGBFTYPE format)) {
	return CLOCK_HZ;
}

void WaitVerticalSync (__REGA0(struct BoardInfo *b), __REGD0(BOOL toggle)) {
	uint32_t vblank_state = ((volatile uint16_t*)b->RegisterBase)[0x800];

	while(vblank_state != 0) {
		vblank_state = ((volatile uint16_t*)b->RegisterBase)[0x800];
	}

	while(vblank_state == 0) {
		vblank_state = ((volatile uint16_t*)b->RegisterBase)[0x800];
	}
}

BOOL GetVSyncState(__REGA0(struct BoardInfo *b), __REGD0(BOOL toggle)) {
	uint32_t vblank_state = ((uint16_t*)b->RegisterBase)[0x800];

	return vblank_state;
}

void FillRect (__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(ULONG color), __REGD5(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!r) return;
	if (w<1 || h<1) return;

#ifdef DMARTG
	dmy_cache
	gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

	gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)rtg_to_mnt[r->RGBFormat];
	gfxdata->mask = mask;

	gfxdata->rgb[0] = color;
	gfxdata->x[0] = x;
	gfxdata->x[1] = w;
	gfxdata->y[0] = y;
	gfxdata->y[1] = h;

	zzwrite16(&registers->blitter_dma_op, OP_FILLRECT);
#else
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;
	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	zzwrite32(&registers->blitter_dst_hi, offset);

	zzwrite32(&registers->blitter_rgb_hi, color);
	zzwrite16(&registers->blitter_row_pitch, r->BytesPerRow >> 2);
	zzwrite16(&registers->blitter_colormode, rtg_to_mnt[r->RGBFormat]);
	zzwrite16(&registers->blitter_x1, x);
	zzwrite16(&registers->blitter_y1, y);
	zzwrite16(&registers->blitter_x2, w);
	zzwrite16(&registers->blitter_y2, h);
	zzwrite16(&registers->blitter_op_fillrect, mask);
#endif
}

void InvertRect (__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!b || !r)
		return;

#ifdef DMARTG
	dmy_cache
	gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
	gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

	gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)rtg_to_mnt[r->RGBFormat];
	gfxdata->mask = mask;

	gfxdata->x[0] = x;
	gfxdata->x[1] = w;
	gfxdata->y[0] = y;
	gfxdata->y[1] = h;

	zzwrite16(&registers->blitter_dma_op, OP_INVERTRECT);
#else
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;
	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);

	zzwrite32(&registers->blitter_dst_hi, offset);
	zzwrite16(&registers->blitter_row_pitch, r->BytesPerRow >> 2);
	zzwrite16(&registers->blitter_colormode, rtg_to_mnt[r->RGBFormat]);

	zzwrite16(&registers->blitter_x1, x);
	zzwrite16(&registers->blitter_y1, y);
	zzwrite16(&registers->blitter_x2, w);
	zzwrite16(&registers->blitter_y2, h);

	zzwrite16(&registers->blitter_op_invertrect, mask);
#endif
}

void BlitRect (__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD dx), __REGD3(WORD dy), __REGD4(WORD w), __REGD5(WORD h), __REGD6(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (w<1 || h<1) return;
	if (!r) return;

#ifdef DMARTG
	dmy_cache
	gfxdata->x[0] = dx;
	gfxdata->x[1] = w;
	gfxdata->x[2] = x;
	gfxdata->y[0] = dy;
	gfxdata->y[1] = h;
	gfxdata->y[2] = y;

	gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	gfxdata->offset[GFXDATA_SRC] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

	gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)rtg_to_mnt[r->RGBFormat];
	gfxdata->mask = mask;

	zzwrite16(&registers->blitter_dma_op, OP_COPYRECT);
#else
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	zzwrite16(&registers->blitter_y1, dy);
	zzwrite16(&registers->blitter_y2, h);
	zzwrite16(&registers->blitter_y3, y);

	zzwrite16(&registers->blitter_x1, dx);
	zzwrite16(&registers->blitter_x2, w);
	zzwrite16(&registers->blitter_x3, x);

	zzwrite16(&registers->blitter_row_pitch, r->BytesPerRow >> 2);
	zzwrite16(&registers->blitter_colormode, rtg_to_mnt[r->RGBFormat] | (mask << 8));

	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	zzwrite32(&registers->blitter_src_hi, offset);
	zzwrite32(&registers->blitter_dst_hi, offset);

	zzwrite16(&registers->blitter_op_copyrect, 1);
#endif
}

void BlitRectNoMaskComplete (__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *rs), __REGA2(struct RenderInfo *rt), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD dx), __REGD3(WORD dy), __REGD4(WORD w), __REGD5(WORD h), __REGD6(UBYTE minterm), __REGD7(RGBFTYPE format)) {
	if (w<1 || h<1) return;
	if (!rs || !rt) return;

#ifdef DMARTG
	dmy_cache

	gfxdata->x[0] = dx;
	gfxdata->x[1] = w;
	gfxdata->x[2] = x;
	gfxdata->y[0] = dy;
	gfxdata->y[1] = h;
	gfxdata->y[2] = y;

	gfxdata->offset[GFXDATA_DST] = ((uint32_t)rt->Memory - (uint32_t)b->MemoryBase);
	gfxdata->offset[GFXDATA_SRC] = ((uint32_t)rs->Memory - (uint32_t)b->MemoryBase);
	gfxdata->pitch[GFXDATA_DST] = (rt->BytesPerRow >> 2);
	gfxdata->pitch[GFXDATA_SRC] = (rs->BytesPerRow >> 2);

	gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)rtg_to_mnt[rt->RGBFormat];
	gfxdata->minterm = minterm;

	zzwrite16(&registers->blitter_dma_op, OP_COPYRECT_NOMASK);
#else
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	zzwrite16(&registers->blitter_y1, dy);
	zzwrite16(&registers->blitter_y2, h);
	zzwrite16(&registers->blitter_y3, y);

	zzwrite16(&registers->blitter_x1, dx);
	zzwrite16(&registers->blitter_x2, w);
	zzwrite16(&registers->blitter_x3, x);

	zzwrite16(&registers->blitter_colormode, rtg_to_mnt[rt->RGBFormat] | (minterm << 8));

	zzwrite16(&registers->blitter_src_pitch, rs->BytesPerRow >> 2);
	uint32_t offset = ((uint32_t)rs->Memory - (uint32_t)b->MemoryBase);
	zzwrite32(&registers->blitter_src_hi, offset);

	zzwrite16(&registers->blitter_row_pitch, rt->BytesPerRow >> 2);
	offset = ((uint32_t)rt->Memory - (uint32_t)b->MemoryBase);
	zzwrite32(&registers->blitter_dst_hi, offset);

	zzwrite16(&registers->blitter_op_copyrect, 2);
#endif
}

void BlitTemplate (__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGA2(struct Template *t), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!r) return;
	if (w<1 || h<1) return;
	if (!t) return;

#ifndef DMARTG
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;
	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	zzwrite32(&registers->blitter_dst_hi, offset);
#endif

	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	if (zorro_version != 3) {
		zz_template_addr = b->MemorySize;
	}

	memcpy((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), t->Memory, t->BytesPerRow * h);

#ifdef DMARTG
	dmy_cache
	gfxdata->x[0] = x;
	gfxdata->x[1] = w;
	gfxdata->x[2] = t->XOffset;
	gfxdata->y[0] = y;
	gfxdata->y[1] = h;

	gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
	gfxdata->pitch[GFXDATA_DST] = r->BytesPerRow;
	gfxdata->pitch[GFXDATA_SRC] = t->BytesPerRow;

	gfxdata->rgb[0] = t->FgPen;
	gfxdata->rgb[1] = t->BgPen;

	gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)rtg_to_mnt[r->RGBFormat];
	gfxdata->u8_user[GFXDATA_U8_DRAWMODE] = t->DrawMode;
	gfxdata->mask = mask;

	zzwrite16(&registers->blitter_dma_op, OP_RECT_TEMPLATE);
#else
	zzwrite32(&registers->blitter_src_hi, zz_template_addr);

	zzwrite32(&registers->blitter_rgb_hi, t->FgPen);
	zzwrite32(&registers->blitter_rgb2_hi, t->BgPen);

	zzwrite16(&registers->blitter_src_pitch, t->BytesPerRow);
	zzwrite16(&registers->blitter_row_pitch, r->BytesPerRow);
	zzwrite16(&registers->blitter_colormode, rtg_to_mnt[r->RGBFormat] | (t->DrawMode << 8));
	zzwrite16(&registers->blitter_x1, x);
	zzwrite16(&registers->blitter_y1, y);
	zzwrite16(&registers->blitter_x2, w);
	zzwrite16(&registers->blitter_y2, h);
	zzwrite16(&registers->blitter_x3, t->XOffset);

	zzwrite16(&registers->blitter_op_filltemplate, mask);
#endif
}

void BlitPattern (__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGA2(struct Pattern *pat), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!r) return;
	if (w<1 || h<1) return;
	if (!pat) return;

#ifndef DMARTG
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	zzwrite32(&registers->blitter_dst_hi, offset);
#endif

	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	if (zorro_version != 3) {
		zz_template_addr = b->MemorySize;
	}

	memcpy((uint8_t*)(((uint32_t)b->MemoryBase) + zz_template_addr), pat->Memory, 2 * (1 << pat->Size));

#ifdef DMARTG
	dmy_cache
	gfxdata->x[0] = x;
	gfxdata->x[1] = w;
	gfxdata->x[2] = pat->XOffset;
	gfxdata->y[0] = y;
	gfxdata->y[1] = h;
	gfxdata->y[2] = pat->YOffset;

	gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
	gfxdata->pitch[GFXDATA_DST] = r->BytesPerRow;

	gfxdata->rgb[0] = pat->FgPen;
	gfxdata->rgb[1] = pat->BgPen;

	gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)rtg_to_mnt[r->RGBFormat];
	gfxdata->u8_user[GFXDATA_U8_DRAWMODE] = pat->DrawMode;
	gfxdata->user[0] = (1 << pat->Size);
	gfxdata->mask = mask;

	zzwrite16(&registers->blitter_dma_op, OP_RECT_PATTERN);
#else
	zzwrite32(&registers->blitter_src_hi, zz_template_addr);

	zzwrite32(&registers->blitter_rgb_hi, pat->FgPen);
	zzwrite32(&registers->blitter_rgb2_hi, pat->BgPen);

	zzwrite16(&registers->blitter_user1, mask);
	zzwrite16(&registers->blitter_row_pitch, r->BytesPerRow);
	zzwrite16(&registers->blitter_colormode, rtg_to_mnt[r->RGBFormat] | (pat->DrawMode << 8));
	zzwrite16(&registers->blitter_x1, x);
	zzwrite16(&registers->blitter_y1, y);
	zzwrite16(&registers->blitter_x2, w);
	zzwrite16(&registers->blitter_y2, h);
	zzwrite16(&registers->blitter_x3, pat->XOffset);
	zzwrite16(&registers->blitter_y3, pat->YOffset);

	zzwrite16(&registers->blitter_op_filltemplate, (1 << pat->Size) | 0x8000);
#endif
}

void DrawLine (__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGA2(struct Line *l), __REGD0(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!l || !r)
		return;

#ifdef DMARTG
	dmy_cache
	gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
	gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

	gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)rtg_to_mnt[r->RGBFormat];
	gfxdata->u8_user[GFXDATA_U8_DRAWMODE] = l->DrawMode;
	gfxdata->u8_user[GFXDATA_U8_LINE_PATTERN_OFFSET] = l->PatternShift;
	gfxdata->u8_user[GFXDATA_U8_LINE_PADDING] = l->pad;

	gfxdata->rgb[0] = l->FgPen;
	gfxdata->rgb[1] = l->BgPen;

	gfxdata->x[0] = l->X;
	gfxdata->x[1] = l->dX;
	gfxdata->y[0] = l->Y;
	gfxdata->y[1] = l->dY;

	gfxdata->user[0] = l->Length;
	gfxdata->user[1] = l->LinePtrn;
	gfxdata->user[2] = ((l->PatternShift << 8) | l->pad);

	gfxdata->mask = mask;

	zzwrite16(&registers->blitter_dma_op, OP_DRAWLINE);
#else
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);

	zzwrite32(&registers->blitter_dst_hi, offset);
	zzwrite16(&registers->blitter_row_pitch, r->BytesPerRow >> 2);

	zzwrite16(&registers->blitter_colormode, rtg_to_mnt[r->RGBFormat] | (l->DrawMode << 8));

	zzwrite32(&registers->blitter_rgb_hi, l->FgPen);

	zzwrite32(&registers->blitter_rgb2_hi, l->BgPen);

	zzwrite16(&registers->blitter_x1, l->X);
	zzwrite16(&registers->blitter_y1, l->Y);
	zzwrite16(&registers->blitter_x2, l->dX);
	zzwrite16(&registers->blitter_y2, l->dY);
	zzwrite16(&registers->blitter_user1, l->Length);
	zzwrite16(&registers->blitter_x3, l->LinePtrn);
	zzwrite16(&registers->blitter_y3, l->PatternShift | (l->pad << 8));

	zzwrite16(&registers->blitter_op_draw_line, mask);
#endif
}

void BlitPlanar2Chunky (__REGA0(struct BoardInfo *b), __REGA1(struct BitMap *bm), __REGA2(struct RenderInfo *r), __REGD0(SHORT x), __REGD1(SHORT y), __REGD2(SHORT dx), __REGD3(SHORT dy), __REGD4(SHORT w), __REGD5(SHORT h), __REGD6(UBYTE minterm), __REGD7(UBYTE mask)) {
	if (!b || !r)
		return;

#ifndef DMARTG
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
#endif
	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	uint16_t zz_mask = mask;
	uint8_t cur_plane = 0x01;

	uint32_t plane_size = bm->BytesPerRow * bm->Rows;

	if (plane_size * bm->Depth > 0xFFFF && zorro_version != 3) {
		b->BlitPlanar2ChunkyDefault(b, bm, r, x, y, dx, dy, w, h, minterm, mask);
		return;
	}

	uint16_t line_size = (w >> 3) + 2;
	uint32_t output_plane_size = line_size * h;

	if (zorro_version != 3) {
		zz_template_addr = b->MemorySize;
	}

#ifdef DMARTG
	gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
	gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
	gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);
	gfxdata->pitch[GFXDATA_SRC] = line_size;

	gfxdata->mask = mask;
	gfxdata->minterm = minterm;
#else
	zzwrite32(&registers->blitter_dst_hi, offset);
	zzwrite16(&registers->blitter_row_pitch, r->BytesPerRow >> 2);
	zzwrite16(&registers->blitter_colormode, rtg_to_mnt[r->RGBFormat] | (minterm << 8));
	zzwrite32(&registers->blitter_src_hi, zz_template_addr);
	zzwrite16(&registers->blitter_src_pitch, line_size);
#endif

	for (int16_t i = 0; i < bm->Depth; i++) {
		uint16_t x_offset = (x >> 3);
		if ((uint32_t)bm->Planes[i] == 0xFFFFFFFF) {
			memset((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), 0xFF, output_plane_size);
		}
		else if (bm->Planes[i] != NULL) {
			uint8_t* bmp_mem = (uint8_t*)bm->Planes[i] + (y * bm->BytesPerRow) + x_offset;
			uint8_t* zz_dest = (uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr);
			for (int16_t y_line = 0; y_line < h; y_line++) {
				memcpy(zz_dest, bmp_mem, line_size);
				zz_dest += line_size;
				bmp_mem += bm->BytesPerRow;
			}
		}
		else {
			zz_mask &= (cur_plane ^ 0xFF);
		}
		cur_plane <<= 1;
		zz_template_addr += output_plane_size;
	}

#ifdef DMARTG
	gfxdata->x[0] = (x & 0x07);
	gfxdata->x[1] = dx;
	gfxdata->x[2] = w;
	gfxdata->y[1] = dy;
	gfxdata->y[2] = h;

	gfxdata->user[0] = zz_mask;
	gfxdata->user[1] = bm->Depth;

	zzwrite16(&registers->blitter_dma_op, OP_P2C);
#else
	zzwrite16(&registers->blitter_x1, x & 0x07);
	zzwrite16(&registers->blitter_x2, dx);
	zzwrite16(&registers->blitter_y2, dy);
	zzwrite16(&registers->blitter_x3, w);
	zzwrite16(&registers->blitter_y3, h);

	zzwrite16(&registers->blitter_user2, zz_mask);

	zzwrite16(&registers->blitter_op_p2c, mask | bm->Depth << 8);
#endif
}

void BlitPlanar2Direct (__REGA0(struct BoardInfo *b), __REGA1(struct BitMap *bm), __REGA2(struct RenderInfo *r), __REGA3(struct ColorIndexMapping *clut), __REGD0(SHORT x), __REGD1(SHORT y), __REGD2(SHORT dx), __REGD3(SHORT dy), __REGD4(SHORT w), __REGD5(SHORT h), __REGD6(UBYTE minterm), __REGD7(UBYTE mask)) {
	if (!b || !r)
		return;

#ifndef DMARTG
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
#endif
	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	uint16_t zz_mask = mask;
	uint8_t cur_plane = 0x01;

	uint32_t plane_size = bm->BytesPerRow * bm->Rows;

	if (plane_size * bm->Depth > 0xFFFF && zorro_version != 3) {
		b->BlitPlanar2DirectDefault(b, bm, r, clut, x, y, dx, dy, w, h, minterm, mask);
		return;
	}

	uint16_t line_size = (w >> 3) + 2;
	uint32_t output_plane_size = line_size * h;

	if (zorro_version != 3) {
		zz_template_addr = b->MemorySize;
	}

#ifdef DMARTG
	gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
	gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
	gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);
	gfxdata->pitch[GFXDATA_SRC] = line_size;
	gfxdata->rgb[0] = clut->ColorMask;

	gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)rtg_to_mnt[r->RGBFormat];
	gfxdata->mask = mask;
	gfxdata->minterm = minterm;
#else
	zzwrite32(&registers->blitter_dst_hi, offset);
	zzwrite16(&registers->blitter_row_pitch, r->BytesPerRow >> 2);
	zzwrite16(&registers->blitter_colormode, rtg_to_mnt[r->RGBFormat] | (minterm << 8));
	zzwrite32(&registers->blitter_src_hi, zz_template_addr);
	zzwrite16(&registers->blitter_src_pitch, line_size);
	zzwrite32(&registers->blitter_rgb_hi, clut->ColorMask);
#endif

	memcpy((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), clut->Colors, (256 << 2));
	zz_template_addr += (256 << 2);

	for (int16_t i = 0; i < bm->Depth; i++) {
		uint16_t x_offset = (x >> 3);
		if ((uint32_t)bm->Planes[i] == 0xFFFFFFFF) {
			memset((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), 0xFF, output_plane_size);
		}
		else if (bm->Planes[i] != NULL) {
			uint8_t* bmp_mem = (uint8_t*)bm->Planes[i] + (y * bm->BytesPerRow) + x_offset;
			uint8_t* zz_dest = (uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr);
			for (int16_t y_line = 0; y_line < h; y_line++) {
				memcpy(zz_dest, bmp_mem, line_size);
				zz_dest += line_size;
				bmp_mem += bm->BytesPerRow;
			}
		}
		else {
			zz_mask &= (cur_plane ^ 0xFF);
		}
		cur_plane <<= 1;
		zz_template_addr += output_plane_size;
	}

#ifdef DMARTG
	gfxdata->x[0] = (x & 0x07);
	gfxdata->x[1] = dx;
	gfxdata->x[2] = w;
	gfxdata->y[1] = dy;
	gfxdata->y[2] = h;

	gfxdata->user[0] = zz_mask;
	gfxdata->user[1] = bm->Depth;

	zzwrite16(&registers->blitter_dma_op, OP_P2D);
#else
	zzwrite16(&registers->blitter_x1, x & 0x07);
	zzwrite16(&registers->blitter_x2, dx);
	zzwrite16(&registers->blitter_y2, dy);
	zzwrite16(&registers->blitter_x3, w);
	zzwrite16(&registers->blitter_y3, h);

	zzwrite16(&registers->blitter_user2, zz_mask);

	zzwrite16(&registers->blitter_op_p2d, mask | bm->Depth << 8);
#endif
}

void SetSprite (__REGA0(struct BoardInfo *b), __REGD0(BOOL what), __REGD7(RGBFTYPE format)) {
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	zzwrite16(&registers->sprite_bitmap, 1);
}

void SetSpritePosition (__REGA0(struct BoardInfo *b), __REGD0(WORD x), __REGD1(WORD y), __REGD7(RGBFTYPE format)) {
#ifdef DMARTG
	dmy_cache
	gfxdata->x[0] = x;
	gfxdata->y[0] = y;

	zzwrite16(&registers->blitter_dma_op, OP_SPRITE_XY);
#else
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;

	zzwrite16(&registers->blitter_x1, x);
	zzwrite16(&registers->blitter_y1, y);
	zzwrite16(&registers->sprite_y, 1);
#endif
}

void SetSpriteImage (__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) {
	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	if (zorro_version != 3) {
		zz_template_addr = b->MemorySize;
	}

	uint16_t data_size = ((b->MouseWidth >> 3) * 2) * (b->MouseHeight);
	if (b->MouseWidth > 16)
		memcpy((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), b->MouseImage+4, data_size);
	else
		memcpy((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), b->MouseImage+2, data_size);

#ifdef DMARTG
	dmy_cache
	gfxdata->offset[1] = zz_template_addr;
	gfxdata->x[0] = b->XOffset;
	gfxdata->x[1] = b->MouseWidth;
	gfxdata->y[0] = b->YOffset;
	gfxdata->y[1] = b->MouseHeight;

	zzwrite16(&registers->blitter_dma_op, OP_SPRITE_BITMAP);
#else
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	zzwrite32(&registers->blitter_src_hi, zz_template_addr);

	zzwrite16(&registers->blitter_x1, b->XOffset);
	zzwrite16(&registers->blitter_x2, b->MouseWidth);
	zzwrite16(&registers->blitter_y1, b->YOffset);
	zzwrite16(&registers->blitter_y2, b->MouseHeight);

	zzwrite16(&registers->sprite_bitmap, 0);
#endif
}

void SetSpriteColor (__REGA0(struct BoardInfo *b), __REGD0(UBYTE idx), __REGD1(UBYTE R), __REGD2(UBYTE G), __REGD3(UBYTE B), __REGD7(RGBFTYPE format)) {
#ifdef DMARTG
	dmy_cache
	((char *)&gfxdata->rgb[0])[0] = B;
	((char *)&gfxdata->rgb[0])[1] = G;
	((char *)&gfxdata->rgb[0])[2] = R;
	((char *)&gfxdata->rgb[0])[3] = 0x00;
	gfxdata->u8offset = idx + 1;

	zzwrite16(&registers->blitter_dma_op, OP_SPRITE_COLOR);
#else
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;

	zzwrite16(&registers->blitter_user1, R);
	zzwrite16(&registers->blitter_user2, B | (G << 8));
	zzwrite16(&registers->sprite_colors, idx + 1);
#endif
}

void SetSplitPosition (__REGA0(struct BoardInfo *b),__REGD0(SHORT pos)) {
	b->YSplit = pos;
	uint32_t offset = ((uint32_t)b->VisibleBitMap->Planes[0]) - ((uint32_t)b->MemoryBase);
#ifdef DMARTG
	gfxdata->offset[0] = offset;
	gfxdata->y[0] = pos;
	zzwrite16(&registers->blitter_dma_op, OP_SET_SPLIT_POS);
#else
	zzwrite32(&registers->blitter_src_hi, offset);
	zzwrite16(&registers->blitter_set_split_pos, pos);
#endif
}

static uint32_t device_vectors[] = {
	(uint32_t)OpenLib,
	(uint32_t)CloseLib,
	(uint32_t)ExpungeLib,
	0,
	(uint32_t)FindCard,
	(uint32_t)InitCard,
	-1
};

struct InitTable
{
	ULONG LibBaseSize;
	APTR  FunctionTable;
	APTR  DataTable;
	APTR  InitLibTable;
};

const uint32_t auto_init_tables[4] = {
	sizeof(struct Library),
	(uint32_t)device_vectors,
	0,
	(uint32_t)InitLib,
};

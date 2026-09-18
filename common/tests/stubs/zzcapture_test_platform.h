/* Minimal AmigaOS call surface for executing the real ZZCapture utility.
 * ULONG holds host pointers in tag lists; hardware words remain UWORD and
 * pixel storage remains uint32_t. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZCAPTURE_TEST_PLATFORM_H
#define ZZCAPTURE_TEST_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uintptr_t ULONG;
typedef const char *CONST_STRPTR;
struct Library { int unused; };
struct Device { int unused; };
struct MsgPort { int unused; };
struct IORequest { struct Device *io_Device; };
struct timerequest { struct IORequest tr_node; };
struct EClockVal { uint32_t ev_hi, ev_lo; };
struct Task { void *tc_SPLower, *tc_SPUpper; };
struct BitMap { unsigned Depth, BytesPerRow; UBYTE *Planes[8]; };
struct RastPort { struct BitMap *BitMap; };
struct ViewPort { ULONG mode; };
struct Screen { int Width, Height; struct RastPort RastPort; struct ViewPort ViewPort; };
struct Window { void *UserPort; };
struct Message { int unused; };
struct IntuiMessage { ULONG Class; UWORD Code; };
struct GfxBase { unsigned ChipRevBits0; };
struct IntuitionBase { struct Screen *FirstScreen; };
struct DisplayInfo { ULONG PropertyFlags; unsigned RedBits, GreenBits, BlueBits; };
struct DimensionInfo { unsigned MaxDepth; };
struct ZZ9000Board { ULONG address; unsigned zorro_version; };

#define SIGBREAKF_CTRL_C 0x1000UL
#define TIMERNAME "timer.device"
#define UNIT_ECLOCK 2
#define IDCMP_RAWKEY 1UL
#define GFXF_AA_ALICE 1U
#define GFXF_AA_LISA 2U
#define SUPER_KEY 0x20UL
#define SUPERLACE_KEY 0x21UL
#define NTSC_MONITOR_ID 0x1000UL
#define PAL_MONITOR_ID 0x2000UL
#define DTAG_DISP 1UL
#define DTAG_DIMS 2UL
#define DIPF_IS_FOREIGN 1UL
#define DIPF_IS_HAM 2UL
#define DIPF_IS_DUALPF 4UL
#define DIPF_IS_EXTRAHALFBRITE 8UL
#define DIPF_IS_SCANDBL 16UL
#define DIPF_IS_PAL 32UL
#define DIPF_IS_LACE 64UL
#define JAM1 0
#define CUSTOMSCREEN 1
#define FALSE 0
#define TRUE 1
#define MEMF_CHIP 1UL
#define MEMF_CLEAR 2UL
#define ZZ_REG_FW_VERSION 0
enum { TAG_END, SA_Type, SA_DisplayID, SA_Width, SA_Height, SA_Depth,
    SA_Interleaved, SA_Quiet, SA_ShowTitle, SA_Behind, SA_AutoScroll,
    SA_ErrorCode, WA_CustomScreen, WA_Left, WA_Top, WA_Width, WA_Height,
    WA_Borderless, WA_Backdrop, WA_RMBTrap, WA_NoCareRefresh, WA_Activate,
    WA_IDCMP };

ULONG zz9000_read_reg32(ULONG base, ULONG reg);
UWORD zz9000_read_reg16(ULONG base, ULONG reg);
void zz9000_write_reg16(ULONG base, ULONG reg, UWORD value);
int zz9000_find_board(struct ZZ9000Board *found);
struct Task *FindTask(CONST_STRPTR name);
ULONG SetSignal(ULONG signals, ULONG mask);
void *GetMsg(void *port);
void ReplyMsg(struct Message *message);
void Delay(ULONG ticks);
void WaitTOF(void);
struct MsgPort *CreateMsgPort(void);
void DeleteMsgPort(struct MsgPort *port);
void *CreateIORequest(struct MsgPort *port, ULONG size);
void DeleteIORequest(struct IORequest *request);
int OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *request, ULONG flags);
void CloseDevice(struct IORequest *request);
ULONG ReadEClock(struct EClockVal *value);
void *OpenLibrary(CONST_STRPTR name, ULONG version);
void CloseLibrary(struct Library *library);
ULONG ModeNotAvailable(ULONG id);
ULONG GetDisplayInfoData(void *handle, UBYTE *data, ULONG size, ULONG tag, ULONG id);
struct Screen *OpenScreenTags(void *unused, ...);
struct Window *OpenWindowTags(void *unused, ...);
void CloseWindow(struct Window *closed);
int CloseScreen(struct Screen *closed);
void *AllocMem(ULONG size, ULONG flags);
void FreeMem(void *memory, ULONG size);
void SetPointer(struct Window *win, UWORD *data, unsigned height, unsigned width, int x, int y);
void LoadRGB32(struct ViewPort *vp, const ULONG *palette);
ULONG GetVPModeID(struct ViewPort *vp);
void SetAPen(struct RastPort *rp, unsigned pen);
void RectFill(struct RastPort *rp, int x0, int y0, int x1, int y1);
void SetDrMd(struct RastPort *rp, unsigned mode);
void Move(struct RastPort *rp, int x, int y);
void Text(struct RastPort *rp, CONST_STRPTR text, ULONG length);
void WaitBlit(void);
void ScreenToFront(struct Screen *front);
void ActivateWindow(struct Window *active);
int zzcapture_test_printf(const char *format, ...);
int zzcapture_test_puts(const char *text);
int zzcapture_test_fflush(FILE *file);
#define printf zzcapture_test_printf
#define puts zzcapture_test_puts
#define fflush zzcapture_test_fflush
#endif

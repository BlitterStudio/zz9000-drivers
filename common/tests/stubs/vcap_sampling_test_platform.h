/*
 * Faithful host-side AmigaOS surface for executing the real vcap_sampling.c
 * under VCAP_SAMPLING_TEST_IO.
 *
 * The key faithfulness point: ViewPort.Modes is the real 16-bit UWORD mode
 * word, NOT the 32-bit ModeID (whose full monitor ID exceeds the 16-bit
 * width). The 32-bit ModeID is a separate association resolved by
 * GetVPModeID. The earlier throwaway sampling smoke modelled ViewPort with
 * a ULONG that already held the full 32-bit ID, masking this.
 *
 * Standalone host type convention (reuse zzcapture_test_platform.h): host
 * ULONG is uintptr_t, hardware words remain UWORD.
 *
 * Mode-ID and allocation bits match the native SDK; other constants name
 * isolated host surface operations.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef VCAP_SAMPLING_TEST_PLATFORM_H
#define VCAP_SAMPLING_TEST_PLATFORM_H

#ifndef ZZ_TEST_EXEC_TYPES_H
#define ZZ_TEST_EXEC_TYPES_H
#endif
#include <stdint.h>

typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uintptr_t ULONG;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;

/* Minimal host structures; the mode word retains its native UWORD width. */
struct BitMap {
    unsigned Depth;
    unsigned BytesPerRow;
    UBYTE *Planes[8];
};
struct RastPort {
    struct BitMap *BitMap;
};
struct ViewPort {
    UWORD Modes;      /* 16-bit mode word; NOT the 32-bit ModeID. */
    uint32_t ModeID;  /* 32-bit ModeID returned by GetVPModeID. */
};
struct Screen {
    unsigned Width, Height;
    struct RastPort RastPort;
    struct ViewPort ViewPort;
};
struct Window {
    unsigned Width, Height;
    ULONG Flags;
    struct MsgPort *UserPort;
    struct RastPort *RPort;
};
struct MsgPort {
    ULONG mp_SigBit;
};
struct Message {
    int unused;
};
struct IntuiMessage {
    ULONG Class;
    UWORD Code;
};
struct DisplayInfo {
    ULONG PropertyFlags;
    unsigned RedBits, GreenBits, BlueBits;
    ULONG NotAvailable;
};
struct DimensionInfo {
    unsigned MaxDepth;
};

/* Host OS-operation constants. */
#define SIGBREAKF_CTRL_C 0x1000UL
#define IDCMP_RAWKEY 0x100UL
#define IDCMP_INACTIVEWINDOW 0x8000UL
#define WFLG_WINDOWACTIVE 0x0100UL
#define CUSTOMSCREEN 1UL
#define JAM1 0

/* 32-bit ModeID parts (the monitor standard occupies high bits). */
#define NTSC_MONITOR_ID 0x00011000UL
#define PAL_MONITOR_ID  0x00021000UL
/* 32-bit ModeID key bits (LACE/SUPERHIRES in the low bits). */
#define MONITOR_ID_MASK 0xFFFF1000UL
#define LACE 0x0004UL

#define SUPERHIRES 0x0020UL
/* SuperHires / SuperHiresLace display keys. */
#define SUPER_KEY 0x00008020UL
#define SUPERLACE_KEY 0x00008024UL

#define SA_Type 1UL
#define SA_DisplayID 2UL
#define SA_Width 3UL
#define SA_Height 4UL
#define SA_Depth 5UL
#define SA_Interleaved 6UL
#define SA_Quiet 7UL
#define SA_ShowTitle 8UL
#define SA_Behind 9UL
#define SA_AutoScroll 10UL
#define SA_ErrorCode 11UL
#define TAG_END 0UL
#define WA_CustomScreen 100UL
#define WA_Left 101UL
#define WA_Top 102UL
#define WA_Width 103UL
#define WA_Height 104UL
#define WA_Borderless 105UL
#define WA_Backdrop 106UL
#define WA_RMBTrap 107UL
#define WA_NoCareRefresh 108UL
#define WA_Activate 109UL
#define WA_IDCMP 110UL
#define TRUE 1
#define FALSE 0
#define DTAG_DISP 100UL
#define DTAG_DIMS 101UL
#define DIPF_IS_FOREIGN (1UL << 0)
#define DIPF_IS_HAM (1UL << 1)
#define DIPF_IS_DUALPF (1UL << 2)
#define DIPF_IS_EXTRAHALFBRITE (1UL << 3)
#define DIPF_IS_SCANDBL (1UL << 4)
#define DIPF_IS_PAL (1UL << 5)
#define DIPF_IS_LACE (1UL << 6)
#define MEMF_CHIP (1UL << 1)
#define MEMF_CLEAR (1UL << 16)

/* OS functions. The test driver (vcap_sampling_screen_test.c) defines them. */
void *GetMsg(void *port);
void ReplyMsg(struct Message *message);
ULONG SetSignal(ULONG oldmask, ULONG newmask);
ULONG GetDisplayInfoData(void *handle, UBYTE *data, ULONG size, ULONG tag,
    ULONG id);
ULONG ModeNotAvailable(ULONG id);
ULONG GetVPModeID(struct ViewPort *viewport);
struct Screen *OpenScreenTags(void *base, ...);
struct Window *OpenWindowTags(void *base, ...);
int CloseScreen(struct Screen *screen);
void CloseWindow(struct Window *window);
void *AllocMem(ULONG bytes, ULONG flags);
void FreeMem(void *ptr, ULONG bytes);
void SetPointer(struct Window *window, void *pointer, UWORD hotx,
    UWORD hoty, UWORD width, UWORD height);
void LoadRGB32(struct ViewPort *viewport, const ULONG *colors);
void WaitBlit(void);
void ScreenToFront(struct Screen *screen);
void ActivateWindow(struct Window *window);
void WaitTOF(void);
void SetAPen(struct RastPort *rp, unsigned pen);
void SetDrMd(struct RastPort *rp, unsigned mode);
void RectFill(struct RastPort *rp, int x0, int y0, int x1, int y1);
void Move(struct RastPort *rp, int x, int y);
void Text(struct RastPort *rp, const char *text, ULONG length);
void Wait(ULONG signals);
void Delay(ULONG ticks);

#endif

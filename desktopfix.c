/*
 * DesktopFix INIT v12
 *
 * Fixes icon background redraw corruption at 32bpp (Millions of Colors)
 * on 68k Macs running Mac OS 7.6 through 8.1.
 *
 * The bug: FillCRgn at 32bpp fails to properly render the desktop pixel
 * pattern, producing corrupted/rainbow pixels behind icon labels/masks.
 *
 * The fix: Tail-patch FillCRgn (trap 0xAA12). After the original runs,
 * re-render the pattern tile correctly by reading the PixPat's tile data
 * directly and writing 32bpp pixels to the framebuffer. This bypasses
 * QuickDraw's broken 32bpp pattern rendering entirely.
 *
 * v1-v9: Patched EraseRect - wrong trap entirely
 * v10: Found FillCRgn, solid color fill - wrong for patterned desktops
 * v11: PtInRgn region fill, corruption detection attempts
 * v12: Read PixPat tile data, render pattern ourselves at 32bpp
 * v13: Clean build - diagnostics removed, confirmed fix
 * v14: Also patch EraseRect (0xA8A3) for icon text rename corruption
 *
 * (c) 2026 - Fixing Apple's homework 30 years later
 */

#include <Quickdraw.h>
#include <Memory.h>
#include <Resources.h>
#include <OSUtils.h>
#include <Windows.h>
#include <Gestalt.h>
#include "ShowInitIcon.h"
#include "Retro68Runtime.h"

/* Trap numbers */
#define kFillCRgnTrap   0xAA12
#define kEraseRectTrap  0xA8A3

/* Low-memory globals */
#define LM_MBarHeight   (*(short *)0x0BAA)
#define LM_WindowList   (*(WindowPeek *)0x09D6)

/* typedefs for original traps - pascal calling convention */
typedef pascal void (*FillCRgnProcPtr)(RgnHandle rgn, PixPatHandle pp);
typedef pascal void (*EraseRectProcPtr)(const Rect *r);

/* Saved original trap addresses */
static FillCRgnProcPtr gOldFillCRgn = NULL;
static EraseRectProcPtr gOldEraseRect = NULL;

/* Recursion guard */
static short gInPatch = 0;

/* Cached screen pixmap info for fast direct access */
static Ptr gScreenBase = NULL;
static long gScreenRowBytes = 0;
static short gScreenWidth = 0;
static short gScreenHeight = 0;
static short gScreenReady = 0;

/*
 * Cache the screen pixmap parameters from the main GDevice.
 */
static Boolean EnsureScreenInfo(void)
{
    GDHandle mainDev;
    PixMapHandle pmh;
    PixMapPtr pm;

    if (gScreenReady)
        return true;

    mainDev = GetMainDevice();
    if (!mainDev || !*mainDev)
        return false;

    pmh = (**mainDev).gdPMap;
    if (!pmh || !*pmh)
        return false;

    pm = *pmh;
    if (pm->pixelSize != 32)
        return false;

    gScreenBase = pm->baseAddr;
    gScreenRowBytes = pm->rowBytes & 0x3FFF;
    gScreenWidth = pm->bounds.right - pm->bounds.left;
    gScreenHeight = pm->bounds.bottom - pm->bounds.top;
    gScreenReady = 1;
    return true;
}

/*
 * Render a PixPat pattern tile directly to the framebuffer inside a region.
 * Reads the pattern's tile data and color table, converts to 32bpp,
 * and writes pixels directly, bypassing QuickDraw entirely.
 *
 * Only handles type 1 (color pixel pattern) at 8bpp with CLUT.
 * Uses PtInRgn to respect exact region shape.
 */
static void RenderPatternInRgn(RgnHandle rgn, PixPatHandle pp)
{
    PixMapHandle patMapH;
    PixMapPtr patMap;
    Handle patDataH;
    Ptr patData;
    CTabHandle ctab;
    short tileW, tileH, tileRowBytes, tileDepth;
    Rect bbox;
    short x, y, tx, ty;
    short left, top, right, bottom;
    unsigned long *rowPtr;
    Point pt;
    unsigned char pixVal;
    ColorSpec *cs;
    unsigned long pixel32;
    char patMapState, patDataState, ctabState;

    if (!pp || !*pp)
        return;

    /* Only handle type 1 (color pixel pattern) */
    if ((**pp).patType != 1)
        return;

    patMapH = (**pp).patMap;
    patDataH = (**pp).patData;
    if (!patMapH || !*patMapH || !patDataH || !*patDataH)
        return;

    /* Lock handles to prevent movement during rendering */
    patMapState = HGetState((Handle)patMapH);
    HLock((Handle)patMapH);
    patDataState = HGetState(patDataH);
    HLock(patDataH);

    patMap = *patMapH;
    patData = *patDataH;

    tileW = patMap->bounds.right - patMap->bounds.left;
    tileH = patMap->bounds.bottom - patMap->bounds.top;
    tileRowBytes = patMap->rowBytes & 0x3FFF;
    tileDepth = patMap->pixelSize;

    if (tileW <= 0 || tileH <= 0 || tileDepth != 8) {
        HSetState((Handle)patMapH, patMapState);
        HSetState(patDataH, patDataState);
        return;
    }

    ctab = patMap->pmTable;
    if (!ctab || !*ctab) {
        HSetState((Handle)patMapH, patMapState);
        HSetState(patDataH, patDataState);
        return;
    }

    ctabState = HGetState((Handle)ctab);
    HLock((Handle)ctab);

    /* Get region bounding box and clip to screen */
    bbox = (**rgn).rgnBBox;
    left = bbox.left;
    top = bbox.top;
    right = bbox.right;
    bottom = bbox.bottom;

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > gScreenWidth) right = gScreenWidth;
    if (bottom > gScreenHeight) bottom = gScreenHeight;

    if (left < right && top < bottom) {
        for (y = top; y < bottom; y++) {
            rowPtr = (unsigned long *)(gScreenBase + (long)y * gScreenRowBytes);
            pt.v = y;
            ty = y % tileH;
            if (ty < 0) ty += tileH;

            for (x = left; x < right; x++) {
                pt.h = x;
                if (PtInRgn(pt, rgn)) {
                    tx = x % tileW;
                    if (tx < 0) tx += tileW;

                    /* Read 8bpp pixel value from pattern tile */
                    pixVal = *((unsigned char *)patData + ty * tileRowBytes + tx);

                    /* Look up in color table and convert to 32bpp */
                    cs = &(**ctab).ctTable[pixVal];
                    pixel32 = ((unsigned long)(cs->rgb.red >> 8) << 16) |
                              ((unsigned long)(cs->rgb.green >> 8) << 8) |
                              (unsigned long)(cs->rgb.blue >> 8);

                    rowPtr[x] = pixel32;
                }
            }
        }
    }

    /* Restore handle states */
    HSetState((Handle)ctab, ctabState);
    HSetState((Handle)patMapH, patMapState);
    HSetState(patDataH, patDataState);
}

/*
 * Render a PixPat pattern tile directly to the framebuffer inside a rect.
 * Same as RenderPatternInRgn but for rectangles (no PtInRgn needed).
 */
static void RenderPatternInRect(const Rect *r, PixPatHandle pp)
{
    PixMapHandle patMapH;
    PixMapPtr patMap;
    Handle patDataH;
    Ptr patData;
    CTabHandle ctab;
    short tileW, tileH, tileRowBytes, tileDepth;
    short x, y, tx, ty;
    short left, top, right, bottom;
    unsigned long *rowPtr;
    unsigned char pixVal;
    ColorSpec *cs;
    unsigned long pixel32;
    char patMapState, patDataState, ctabState;

    if (!pp || !*pp)
        return;

    if ((**pp).patType != 1)
        return;

    patMapH = (**pp).patMap;
    patDataH = (**pp).patData;
    if (!patMapH || !*patMapH || !patDataH || !*patDataH)
        return;

    patMapState = HGetState((Handle)patMapH);
    HLock((Handle)patMapH);
    patDataState = HGetState(patDataH);
    HLock(patDataH);

    patMap = *patMapH;
    patData = *patDataH;

    tileW = patMap->bounds.right - patMap->bounds.left;
    tileH = patMap->bounds.bottom - patMap->bounds.top;
    tileRowBytes = patMap->rowBytes & 0x3FFF;
    tileDepth = patMap->pixelSize;

    if (tileW <= 0 || tileH <= 0 || tileDepth != 8) {
        HSetState((Handle)patMapH, patMapState);
        HSetState(patDataH, patDataState);
        return;
    }

    ctab = patMap->pmTable;
    if (!ctab || !*ctab) {
        HSetState((Handle)patMapH, patMapState);
        HSetState(patDataH, patDataState);
        return;
    }

    ctabState = HGetState((Handle)ctab);
    HLock((Handle)ctab);

    /* Clip to screen */
    left = r->left;
    top = r->top;
    right = r->right;
    bottom = r->bottom;

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > gScreenWidth) right = gScreenWidth;
    if (bottom > gScreenHeight) bottom = gScreenHeight;

    if (left < right && top < bottom) {
        for (y = top; y < bottom; y++) {
            rowPtr = (unsigned long *)(gScreenBase + (long)y * gScreenRowBytes);
            ty = y % tileH;
            if (ty < 0) ty += tileH;

            for (x = left; x < right; x++) {
                tx = x % tileW;
                if (tx < 0) tx += tileW;

                pixVal = *((unsigned char *)patData + ty * tileRowBytes + tx);
                cs = &(**ctab).ctTable[pixVal];
                pixel32 = ((unsigned long)(cs->rgb.red >> 8) << 16) |
                          ((unsigned long)(cs->rgb.green >> 8) << 8) |
                          (unsigned long)(cs->rgb.blue >> 8);

                rowPtr[x] = pixel32;
            }
        }
    }

    HSetState((Handle)ctab, ctabState);
    HSetState((Handle)patMapH, patMapState);
    HSetState(patDataH, patDataState);
}

/*
 * Check if the current GrafPort is WMgrCPort.
 */
static Boolean IsWMgrDraw(void)
{
    GrafPtr currentPort;
    CGrafPtr wmPort;

    GetPort(&currentPort);
    wmPort = *(CGrafPtr *)0x0D2C;

    return (currentPort == (GrafPtr)wmPort);
}

/*
 * Check if a rect overlaps any window's structure region,
 * excluding the last window (the desktop window).
 */
static Boolean IsRectInAnyWindowStruc(const Rect *r)
{
    WindowPeek win;

    win = LM_WindowList;
    while (win) {
        if (!win->nextWindow)
            break;
        if (win->strucRgn && *win->strucRgn) {
            if (RectInRgn(r, win->strucRgn))
                return true;
        }
        win = win->nextWindow;
    }
    return false;
}

/*
 * Patched FillCRgn - v12
 *
 * After FillCRgn runs (possibly producing corruption at 32bpp),
 * re-render the pattern from the PixPat tile data directly to
 * the framebuffer. This completely bypasses QuickDraw's broken
 * 32bpp pattern rendering.
 *
 * Only fires for small regions drawn through WMgrCPort that
 * don't overlap any window's structure region.
 */
pascal void PatchedFillCRgn(RgnHandle rgn, PixPatHandle pp)
{
    Rect bbox;

    if (gInPatch) {
        gOldFillCRgn(rgn, pp);
        return;
    }

    gInPatch = 1;

    /* Call the original FillCRgn */
    gOldFillCRgn(rgn, pp);

    /* Only fix small desktop regions drawn through WMgrCPort */
    if (EnsureScreenInfo() && rgn && *rgn && IsWMgrDraw()) {
        bbox = (**rgn).rgnBBox;

        if ((bbox.right - bbox.left) >= 4 &&
            (bbox.bottom - bbox.top) >= 4 &&
            (bbox.right - bbox.left) <= 250 &&
            (bbox.bottom - bbox.top) <= 250 &&
            bbox.top >= LM_MBarHeight &&
            !IsRectInAnyWindowStruc(&bbox)) {

            /* Re-render the pattern correctly at 32bpp */
            RenderPatternInRgn(rgn, pp);
        }
    }

    gInPatch = 0;
}

/*
 * Patched EraseRect - v14
 *
 * After EraseRect runs, re-render the port's background PixPat
 * directly to the framebuffer. Fixes icon text rename corruption
 * at 32bpp where EraseRect fails to properly tile the desktop pattern.
 *
 * Same guards as FillCRgn patch: WMgrCPort, size limits, menu bar,
 * window structure overlap.
 */
pascal void PatchedEraseRect(const Rect *r)
{
    CGrafPtr wmPort;
    GrafPtr currentPort;
    PixPatHandle bkPat;

    if (gInPatch) {
        gOldEraseRect(r);
        return;
    }

    gInPatch = 1;

    /* Call the original EraseRect */
    gOldEraseRect(r);

    /* Only fix small desktop rects drawn through WMgrCPort */
    if (EnsureScreenInfo() && r && IsWMgrDraw()) {
        if ((r->right - r->left) >= 2 &&
            (r->bottom - r->top) >= 2 &&
            (r->right - r->left) <= 250 &&
            (r->bottom - r->top) <= 250 &&
            r->top >= LM_MBarHeight &&
            !IsRectInAnyWindowStruc(r)) {

            /* Get the background PixPat from WMgrCPort */
            wmPort = *(CGrafPtr *)0x0D2C;
            if (wmPort && wmPort->bkPixPat) {
                RenderPatternInRect(r, wmPort->bkPixPat);
            }
        }
    }

    gInPatch = 0;
}

/*
 * INIT entry point
 */
void _start(void)
{
    long qdVersion;
    Handle self;

    RETRO68_RELOCATE();
    Retro68CallConstructors();

    ShowInitIcon(128, true);

    if (Gestalt(gestaltQuickdrawVersion, &qdVersion) != noErr)
        goto bail;

    if (qdVersion < gestalt32BitQD)
        goto bail;

    gOldFillCRgn = (FillCRgnProcPtr)GetToolTrapAddress(kFillCRgnTrap);
    SetToolTrapAddress((ProcPtr)PatchedFillCRgn, kFillCRgnTrap);

    gOldEraseRect = (EraseRectProcPtr)GetToolTrapAddress(kEraseRectTrap);
    SetToolTrapAddress((ProcPtr)PatchedEraseRect, kEraseRectTrap);

    self = Get1Resource('INIT', 128);
    if (self) {
        HLock(self);
        DetachResource(self);
    }

    return;

bail:
    Retro68FreeGlobals();
}

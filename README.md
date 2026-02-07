# DesktopFix INIT

**Fixing Apple's homework 30 years later.**

A system extension (INIT) for 68k Macs that fixes desktop pattern corruption at 32bpp (Millions of Colors) on Mac OS 7.6 through 8.1. The bug produces garbled rainbow pixels behind desktop icon labels, masks, and text editing areas.

![Before and after](https://img.shields.io/badge/status-working-brightgreen)

## The Bug

At 32bpp color depth, QuickDraw's `FillCRgn` and `EraseRect` traps fail to properly tile the desktop `PixPat` pattern. Instead of the familiar Mac OS watermark background, you get corrupted rainbow/garbage pixels behind:

- Icon label backgrounds (the region behind icon text and masks)
- Icon text editing areas (when renaming files, the border and exposed areas corrupt)

This bug exists in the shipping Mac OS source code but was **never encountered on real hardware**. The Quadra 800's onboard video has 1MB of VRAM - not enough for 32bpp at any usable resolution. The "Millions of Colors" option simply never appeared in the Monitors control panel.

So why does it matter now? **QEMU.** The QEMU Quadra 800 emulation provides 4MB of VRAM, unlocking 32bpp mode and exposing this never-tested code path for the first time in 30 years. If you run Mac OS 7.6-8.1 in QEMU at Millions of Colors, you hit this bug immediately.

## The Fix

DesktopFix installs as a standard INIT at boot (shows a wrench icon in the startup parade). It tail-patches two QuickDraw traps:

### FillCRgn (Trap $AA12) - Icon Backgrounds

After the original `FillCRgn` runs (and produces corruption), DesktopFix re-renders the pattern correctly by:

1. Reading the `PixPat`'s tile bitmap directly (`patData` - raw 8bpp pixel indices)
2. Looking up each pixel index in the color table (`pmTable->ctTable[pixVal]`)
3. Converting the 16-bit-per-channel `RGBColor` to 32bpp framebuffer format
4. Writing pixels directly to the screen framebuffer via the main `GDevice`'s `PixMap`
5. Using `PtInRgn` to respect the exact region shape (not just the bounding box)

This completely bypasses QuickDraw's broken 32bpp pattern rendering.

**Guards** to avoid painting over things that aren't the desktop:
- Only fires when drawing through `WMgrCPort` (Window Manager color port, low-mem `$0D2C`)
- Region bounding box must be 1-250px in each dimension (the initial full-desktop draw works fine - only small redraws corrupt)
- Must be below the menu bar (`LM_MBarHeight`)
- Must not overlap any window's structure region (walks the `WindowList`, excludes the last/desktop window)
- Recursion guard prevents infinite loops

### EraseRect (Trap $A8A3) - Text Rename Areas

Same approach, but for rectangles. When you rename a desktop icon, the Finder erases the text editing area and its surroundings. At 32bpp, this produces the same corruption.

Key difference: the Finder does text editing in **its own port**, not `WMgrCPort`. So this patch reads the background `PixPat` from whatever `CGrafPort` is current (checking `portVersion & $C000` to confirm it's a color port) rather than requiring `WMgrCPort`.

## The Journey (v1-v14)

This wasn't a straight path. Finding the right trap to patch took extensive diagnostic work with colored fills:

| Version | Approach | Result |
|---------|----------|--------|
| v1-v9 | Patched `EraseRect` ($A8A3) | Wrong trap - `EraseRect` handles menus/titlebars, not icon backgrounds |
| v10 | Found `FillCRgn` ($AA12), solid color fill | Right trap! But solid purple - desktop is a tiled *pattern*, not a solid color |
| v11 | `PtInRgn` region fill, corruption detection | Region-accurate but corruption detection unreliable (rainbow pixels defy simple heuristics) |
| v12 | Read `PixPat` tile data, render pattern at 32bpp | The breakthrough - bypass QuickDraw entirely and render the pattern ourselves |
| v13 | Clean build, diagnostics removed | Confirmed working for icon backgrounds |
| v14 | Added `EraseRect` patch for text rename | Both bugs fixed - `EraseRect` uses current port's `bkPixPat`, not `WMgrCPort` |

Some highlights from the debugging saga:

- **Nuclear red diagnostic**: Painting `EraseRect` bright red proved it handles menus and titlebars, NOT icon backgrounds. Nine versions wasted on the wrong trap.
- **Yellow breakthrough**: Painting `FillCRgn` yellow turned the *entire desktop* yellow including all icon areas. That was the moment we knew.
- **The pattern realization**: Multiple versions tried to fill with a sampled "desktop color" - solid purple, sampled adjacent pixels, color distance thresholds. The user finally pointed out: *"are you forgetting that it's not a 'desktop color'? it's literally a tiled image."* Reading the `PixPat` tile data directly was the answer.
- **Port discovery**: The `EraseRect` patch initially only checked `WMgrCPort` and did nothing. A magenta diagnostic (no port filter) proved `EraseRect` IS called for text areas - just through the Finder's own `CGrafPort`, not `WMgrCPort`.

## Technical Details

### Why the Bug Exists

The Mac OS desktop background is drawn using a `PixPat` (pixel pattern) - typically the Mac OS watermark pattern at 8bpp with a CLUT (Color Lookup Table). When QuickDraw needs to fill a region with this pattern at 32bpp, it has to:

1. Read the 8bpp tile pixel indices
2. Look up colors in the CLUT
3. Convert to 32bpp
4. Write to the framebuffer

Step 3 is where things go wrong. The 32bpp rendering path in QuickDraw was never tested on real hardware because no shipping 68k Mac had enough VRAM for 32bpp at a usable resolution. QEMU's generous 4MB VRAM allocation exposes the bug.

### How the Fix Works

Instead of trying to detect and correct corruption after the fact (which proved unreliable - rainbow pixels have no consistent signature), DesktopFix takes the "just do it right" approach:

```
Original FillCRgn runs -> produces garbage at 32bpp
                       -> DesktopFix overwrites with correct pixels
```

The pattern rendering reads the `PixPat` structure:
- `patType` must be 1 (color pixel pattern, as opposed to type 0 old-style or type 2 dithered RGB)
- `patMap` -> `PixMapHandle` with tile dimensions, row bytes, and pixel depth (must be 8bpp)
- `patData` -> `Handle` to the raw tile pixel data (8bpp indices)
- `patMap->pmTable` -> `CTabHandle` with the CLUT for index-to-RGB conversion

For each pixel in the region/rect:
```c
tx = x % tileW;                              // tile-relative X
ty = y % tileH;                              // tile-relative Y
pixVal = patData[ty * tileRowBytes + tx];     // 8bpp index
cs = &ctab->ctTable[pixVal];                  // CLUT lookup
pixel32 = (cs->rgb.red >> 8) << 16 |         // convert to 32bpp
          (cs->rgb.green >> 8) << 8 |
          (cs->rgb.blue >> 8);
framebuffer[y][x] = pixel32;                  // direct write
```

### What Won't Work (Lessons Learned)

- **GetCPixel**: Also broken at 32bpp. Returns black. Can't use it to sample existing pixels.
- **Corruption detection**: Rainbow corruption has no consistent color signature. White checks, black checks, color distance thresholds, local variance analysis - all produce both false positives and false negatives.
- **Solid color fill**: The desktop is a tiled pattern. Filling with any single color (even sampled from adjacent pixels) looks wrong.
- **EraseRgn trap $A8A9**: This is the WRONG trap number. The correct EraseRgn is $A8D4. Using $A8A9 causes a DOUBLE MMU FAULT crash.

## Building

Requires the [Retro68](https://github.com/autc04/Retro68) cross-compiler toolchain for 68k Macs.

```bash
# Set up build directory (first time only)
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake

# Build
cd build && make
```

This produces `DesktopFix.dsk` (HFS disk image containing the INIT) and `DesktopFix.bin` (MacBinary).

## Installing

### On an HFS disk image (for QEMU)

Using [hfsutils](https://www.mars.org/home/rob/proj/hfs/):

```bash
# Extract from build disk
hmount build/DesktopFix.dsk
hcopy -m ":DesktopFix" DesktopFix.bin
humount

# Install to OS disk image
hmount your-os-disk.hda
hcopy -m DesktopFix.bin ":System Folder:Extensions:DesktopFix"
humount
```

### On a real Mac

Copy `DesktopFix` to your `System Folder:Extensions` folder and restart.

## QEMU Setup

For reference, the QEMU command to run a Quadra 800 at 32bpp:

```bash
qemu-system-m68k -M q800 -m 128 \
    -bios /path/to/Q800.ROM \
    -display gtk -g 800x600x24 \
    -drive file=pram.img,format=raw,if=mtd \
    -device scsi-hd,scsi-id=0,drive=hd0 \
    -drive file=your-os-disk.hda,media=disk,format=raw,if=none,id=hd0 \
    -device nubus-virtio-mmio,romfile=declrom \
    -device virtio-tablet-device \
    -nic user,model=dp83932
```

Note: `-g 800x600x24` requests 32bpp (the `24` refers to 24 bits of color data, stored in 32-bit pixels). This is what triggers the bug. At 256 Colors or Thousands of Colors, the desktop renders fine.

## Compatibility

- **Mac OS**: 7.6, 7.6.1, 8.0, 8.1 (the versions where this pattern rendering bug exists)
- **Hardware**: Any 68k Mac with 32-bit Color QuickDraw (checks via Gestalt at startup)
- **Emulation**: Tested on QEMU `q800` machine type
- **Safe**: Does nothing at color depths other than 32bpp (the INIT checks `pixelSize == 32` at startup)

## License

Public domain. This is a bugfix for 30-year-old software running on emulated hardware. Do whatever you want with it.

## Discussion

- [68kmla forum thread](https://68kmla.org/bb/threads/quadra-700-desktop-icon-corruption-24bit-in-7-6-8-1.51634/)

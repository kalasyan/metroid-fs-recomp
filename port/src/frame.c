// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — the per-frame "hardware" timeline.
//
// On the GBA the main loop ends every frame with a Halt; the VBlank IRQ wakes
// it up. Here the Halt calls Port_RunFrame(), which plays out one frame of
// hardware events in order:
//
//   1. VBlank: VBlank-timed DMA + the game's VBlank IRQ handler (it uploads
//      OAM, scroll registers, palettes... for the next displayed frame).
//   2. Visible scanlines 0..159 rendered by the software PPU. Before each
//      line, the previous line's HBlank events run (HBlank DMA, HBlank IRQ)
//      plus the VCount-match IRQ, so raster effects work.
//   3. Sound: play the frame's audio (DirectSound FIFOs + PSG).
//   4. Present the frame, read input, wait for the next tick (before that,
//      the extras read the game state: rumble, second screen).
//
// With the widescreen option the frame is wider than 240 px (widescreen.c).
#include "port.h"
#include "port_game.h"

#include "../ppu/include/cpu/mode1.h"
#include "../ppu/include/virtuappu.h"

enum {
    DSTAT_IN_VBLANK = 1 << 0,
    DSTAT_IN_HBLANK = 1 << 1,
    DSTAT_VCOUNT_MATCH = 1 << 2,
    DSTAT_IRQ_VBLANK = 1 << 3,
    DSTAT_IRQ_HBLANK = 1 << 4,
    DSTAT_IRQ_VCOUNT = 1 << 5,

    IRQ_VBLANK = 1 << 0,
    IRQ_HBLANK = 1 << 1,
    IRQ_VCOUNT = 1 << 2,
};

// Index in sIntrTable for each IRQ (order of the checks in asm/intr_main.s).
enum {
    INTR_SLOT_VBLANK = 1,
    INTR_SLOT_HBLANK = 2,
    INTR_SLOT_VCOUNT = 3,
};

static uint64_t sFrameCount;
static bool sInFrame;

uint64_t Port_FrameCount(void)
{
    return sFrameCount;
}

static bool IrqEnabled(uint16_t irqBit, uint16_t dispstatBit)
{
    if (!(Port_IoRead16(IO_IME) & 1))
        return false;
    if (!(Port_IoRead16(IO_IE) & irqBit))
        return false;
    return (Port_IoRead16(IO_DISPSTAT) & dispstatBit) != 0;
}

static void RaiseIrq(int slot)
{
    if (sIntrTable[slot] != NULL)
        sIntrTable[slot]();
}

static void SetVCount(int line)
{
    uint16_t dispstat = Port_IoRead16(IO_DISPSTAT);
    const int lyc = dispstat >> 8;

    Port_IoWrite16(IO_VCOUNT, (uint16_t)line);
    dispstat &= ~(DSTAT_IN_VBLANK | DSTAT_VCOUNT_MATCH);
    if (line >= GBA_SCREEN_HEIGHT && line < 227)
        dispstat |= DSTAT_IN_VBLANK;
    if (line == lyc)
        dispstat |= DSTAT_VCOUNT_MATCH;
    Port_IoWrite16(IO_DISPSTAT, dispstat);

    if (line == lyc && IrqEnabled(IRQ_VCOUNT, DSTAT_IRQ_VCOUNT))
        RaiseIrq(INTR_SLOT_VCOUNT);
}

// Busy-wait loops poll VCOUNT outside of Port_RunFrame. Let the virtual beam
// move through the lines (without IRQs or DMA: this only unblocks the loop).
unsigned short Port_PollVCount(void)
{
    uint16_t line = Port_IoRead16(IO_VCOUNT);

    if (!sInFrame) {
        line = (uint16_t)((line + 1) % 228);
        Port_IoWrite16(IO_VCOUNT, line);
    }
    return line;
}

static void HBlank(int line)
{
    if (line < GBA_SCREEN_HEIGHT)
        Port_DmaOnHBlank();
    if (IrqEnabled(IRQ_HBLANK, DSTAT_IRQ_HBLANK))
        RaiseIrq(INTR_SLOT_HBLANK);
}

// Called by the PPU before it snapshots the I/O registers for `line`.
static void PreLine(int line)
{
    if (line > 0)
        HBlank(line - 1);
    SetVCount(line);
}

void Port_PpuInit(void)
{
    static PPUMemory sPpu;
    VirtuaPPUMode1GbaMemory mem = {
        .io_mem = gPortIo,
        .vram = gPortVram,
        .bg_palette = (uint16_t*)gPortPalram,
        .obj_palette = (uint16_t*)(gPortPalram + 0x200),
        .oam_mem = (uint16_t*)gPortOam,
    };

    virtuappu_mode1_bind_gba_memory(&mem);
    sPpu.frame_width = GBA_SCREEN_WIDTH;
    sPpu.frame_pitch = GBA_SCREEN_WIDTH;
    sPpu.mode = 1;
    virtuappu_mode1_set_frame_geometry(&sPpu);
    virtuappu_mode1_pre_line_callback = PreLine;
}

const uint32_t* Port_PpuRenderFrame(int width)
{
    static PPUMemory sPpu;
    const uint16_t dispcnt = Port_IoRead16(IO_DISPCNT);
    const int gbaMode = dispcnt & 7;

    sPpu.frame_width = (uint16_t)width;
    sPpu.frame_pitch = (uint16_t)width;
    switch (gbaMode) {
    case 0:
        sPpu.mode = 1;
        break;
    case 1:
    case 2:
        sPpu.mode = 2;
        break;
    default: {
        static int sWarned;
        if (!sWarned) {
            Port_Log("PPU: display mode %d not supported yet", gbaMode);
            sWarned = 1;
        }
        sPpu.mode = 1;
        break;
    }
    }

    virtuappu_mode1_render_frame(&sPpu);
    return virtuappu_frame_buffer;
}

static void UpdateKeys(void)
{
    uint16_t pressed = (Port_HostPollKeys() | Port_WarpKeys()) & PORT_KEY_MASK;
    // KEYINPUT is active-low.
    Port_IoWrite16(IO_KEYINPUT, (uint16_t)(~pressed & PORT_KEY_MASK));
}

void Port_RunFrame(void)
{
    const uint32_t* frame;
    int width;

    if (sInFrame) {
        // A VBlank/HBlank handler waiting for VBlank would recurse forever.
        Port_Log("Port_RunFrame re-entered; ignoring");
        return;
    }
    sInFrame = true;

    // 1. VBlank (line 160).
    HBlank(GBA_SCREEN_HEIGHT - 1);
    SetVCount(GBA_SCREEN_HEIGHT);
    Port_DmaOnVBlank();
    if (IrqEnabled(IRQ_VBLANK, DSTAT_IRQ_VBLANK))
        RaiseIrq(INTR_SLOT_VBLANK);
    for (int line = GBA_SCREEN_HEIGHT + 1; line < 228; line++) {
        HBlank(line - 1);
        SetVCount(line);
    }

    // 2. Draw the frame. PreLine() replays scanline 0..159 events.
    frame = Port_WideRenderFrame(&width);

    // 3. Sound.
    Port_ApuRunFrame();

    // 4. Rumble and second screen, then present + input + pacing.
    Port_ExtrasFrame();
    if (!Port_HostPresent(frame, width, GBA_SCREEN_HEIGHT))
        Port_RequestQuit();
    UpdateKeys();
    Port_FlushSave();
    Port_HostWaitFrame();

    sFrameCount++;
    sInFrame = false;

    if (Port_QuitRequested())
        Port_ExitToMain();
}

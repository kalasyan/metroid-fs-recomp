// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — save states.
//
// The machine state is:
//   - the GBA memory regions (EWRAM, IWRAM, I/O, palettes, VRAM, OAM). Save
//     memory (SRAM) is not included: loading a state never touches the
//     player's real save file;
//   - the "mf_state" section: every game global (IWRAM_DATA) and the port's
//     own hardware state (PORT_STATE: DMA channels, sound chip).
//
// States are only taken and restored at the end of AgbMain's loop (see
// Port_HostFrameBoundary), where the C stack is always the same, so nothing
// on it needs saving.
//
// Natively many of those variables are pointers: to game code and data (in
// this executable / libmain.so) or into the player's ROM (sound data, read in
// place). Both move between runs (ASLR, malloc), so the state records where
// they were and Port_StateLoad slides every pointer-sized value that falls in
// one of those ranges. Random game data that happens to look like such an
// address is practically impossible: it would need the exact upper 32 bits of
// a 64-bit host address.
#define _GNU_SOURCE
#include "port.h"
#include "port_assets.h"

#include <link.h>
#include <stdint.h>

#define STATE_MAGIC "MFSTATE\0\0"
#define STATE_VERSION 1

// Bounds of the section, made by the linker.
extern unsigned char __start_mf_state[];
extern unsigned char __stop_mf_state[];

typedef struct Region {
    unsigned char* data;
    size_t size;
} Region;

typedef struct StateHeader {
    char magic[8];
    uint32_t version;
    uint32_t headerSize;
    char region[4];
    uint32_t sectionSize;
    // Layout of this build: a state from another build would restore
    // variables into the wrong places.
    uint64_t moduleSize;
    uint64_t ewramOffset;  // gPortEwram - module base
    uint64_t sectionOffset;
    uint64_t codeOffset;   // a function, Port_StateSave - module base
    // Where things were when the state was saved (for relocation).
    uint64_t moduleBase;
    uint64_t romBase;
    uint64_t romSize;
    uint64_t frameCount;
} StateHeader;

static size_t Regions(Region* out)
{
    size_t n = 0;
    out[n++] = (Region){ gPortEwram, sizeof(gPortEwram) };
    out[n++] = (Region){ gPortIwram, sizeof(gPortIwram) };
    out[n++] = (Region){ gPortIo, sizeof(gPortIo) };
    out[n++] = (Region){ gPortPalram, sizeof(gPortPalram) };
    out[n++] = (Region){ gPortVram, sizeof(gPortVram) };
    out[n++] = (Region){ gPortOam, sizeof(gPortOam) };
    out[n++] = (Region){ __start_mf_state, (size_t)(__stop_mf_state - __start_mf_state) };
    return n;
}

// ---- where this build is loaded ---------------------------------------------

typedef struct ModuleRange {
    uintptr_t probe; // an address inside the module
    uintptr_t lo, hi;
} ModuleRange;

static int FindModule(struct dl_phdr_info* info, size_t size, void* data)
{
    ModuleRange* m = data;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    (void)size;

    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD)
            continue;
        const uintptr_t start = info->dlpi_addr + ph->p_vaddr;
        const uintptr_t end = start + ph->p_memsz;
        if (start < lo)
            lo = start;
        if (end > hi)
            hi = end;
    }
    if (m->probe >= lo && m->probe < hi) {
        m->lo = lo;
        m->hi = hi;
        return 1;
    }
    return 0;
}

static ModuleRange Module(void)
{
    static ModuleRange sModule;
    if (sModule.hi == 0) {
        sModule.probe = (uintptr_t)gPortEwram;
        if (!dl_iterate_phdr(FindModule, &sModule))
            Port_Fatal("savestate: can't find the game's own module");
    }
    return sModule;
}

static void FillHeader(StateHeader* h)
{
    const ModuleRange m = Module();
    size_t romSize = 0;
    const uint8_t* rom = Port_RomData(&romSize);

    memset(h, 0, sizeof(*h));
    memcpy(h->magic, STATE_MAGIC, sizeof(h->magic));
    h->version = STATE_VERSION;
    h->headerSize = sizeof(*h);
    strncpy(h->region, Port_RomRegionName(), sizeof(h->region));
    h->sectionSize = (uint32_t)(__stop_mf_state - __start_mf_state);
    h->moduleSize = m.hi - m.lo;
    h->ewramOffset = (uintptr_t)gPortEwram - m.lo;
    h->sectionOffset = (uintptr_t)__start_mf_state - m.lo;
    h->codeOffset = (uintptr_t)&Port_StateSave - m.lo;
    h->moduleBase = m.lo;
    h->romBase = (uintptr_t)rom;
    h->romSize = romSize;
    h->frameCount = Port_FrameCount();
}

size_t Port_StateSize(void)
{
    Region regions[8];
    const size_t n = Regions(regions);
    size_t size = sizeof(StateHeader);
    for (size_t i = 0; i < n; i++)
        size += regions[i].size;
    return size;
}

void Port_StateSave(uint8_t* buf)
{
    Region regions[8];
    const size_t n = Regions(regions);
    StateHeader h;

    FillHeader(&h);
    memcpy(buf, &h, sizeof(h));
    buf += sizeof(h);
    for (size_t i = 0; i < n; i++) {
        memcpy(buf, regions[i].data, regions[i].size);
        buf += regions[i].size;
    }
}

// Slides the pointers in [p, p + size) from the saved run's addresses to ours.
static void Relocate(unsigned char* p, size_t size, const StateHeader* saved, const StateHeader* now)
{
    const uint64_t modLo = saved->moduleBase, modHi = saved->moduleBase + saved->moduleSize;
    const uint64_t romLo = saved->romBase, romHi = saved->romBase + saved->romSize;
    const uint64_t modDelta = now->moduleBase - saved->moduleBase;
    const uint64_t romDelta = now->romBase - saved->romBase;

    if (sizeof(void*) != 8 || (modDelta == 0 && romDelta == 0))
        return;
    // Native structs keep pointers 8-byte aligned, but EWRAM/IWRAM buffers
    // hold structs at GBA offsets: look at every 4-byte boundary.
    for (size_t off = 0; off + 8 <= size; off += 4) {
        uint64_t v;
        memcpy(&v, p + off, sizeof(v));
        if (v >= modLo && v <= modHi)
            v += modDelta;
        else if (romLo != 0 && v >= romLo && v <= romHi)
            v += romDelta;
        else
            continue;
        memcpy(p + off, &v, sizeof(v));
        off += 4; // a pointer covers the next boundary too
    }
}

PortStateResult Port_StateLoad(const uint8_t* buf, size_t size)
{
    Region regions[8];
    const size_t n = Regions(regions);
    StateHeader saved, now;

    if (size < sizeof(saved))
        return PORT_STATE_BAD_FILE;
    memcpy(&saved, buf, sizeof(saved));
    if (memcmp(saved.magic, STATE_MAGIC, sizeof(saved.magic)) != 0 || saved.headerSize != sizeof(saved))
        return PORT_STATE_BAD_FILE;

    FillHeader(&now);
    if (saved.version != now.version || memcmp(saved.region, now.region, sizeof(now.region)) != 0 ||
        saved.sectionSize != now.sectionSize || saved.moduleSize != now.moduleSize ||
        saved.ewramOffset != now.ewramOffset || saved.sectionOffset != now.sectionOffset ||
        saved.codeOffset != now.codeOffset || saved.romSize != now.romSize)
        return PORT_STATE_OTHER_BUILD;
    if (size != Port_StateSize())
        return PORT_STATE_BAD_FILE;

    buf += sizeof(saved);
    for (size_t i = 0; i < n; i++) {
        memcpy(regions[i].data, buf, regions[i].size);
        Relocate(regions[i].data, regions[i].size, &saved, &now);
        buf += regions[i].size;
    }

    Port_SoundRebuildVoicegroups();
    Port_ExtrasReset();
    return PORT_STATE_OK;
}

const char* Port_StateResultText(PortStateResult result)
{
    switch (result) {
    case PORT_STATE_OK:
        return "OK";
    case PORT_STATE_BAD_FILE:
        return "not a valid save state";
    case PORT_STATE_OTHER_BUILD:
        return "made by another version of the game";
    }
    return "?";
}

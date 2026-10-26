#ifndef PORT_PLATFORM_H
#define PORT_PLATFORM_H

/*
 * mzm-android: native (PC / Android) build glue for the game code.
 *
 * On the GBA the game talks to hardware through fixed addresses
 * (0x02000000 EWRAM, 0x04000000 I/O, 0x06000000 VRAM, ...). In the native
 * build those regions are plain host buffers owned by the port layer
 * (port/src/gba_mem.c) and the *_BASE macros in include/gba/memory.h point
 * at them instead. The port's software PPU reads the same buffers.
 *
 * This header must stay free of libc includes: game translation units are
 * compiled with -nostdinc and define their own NULL/u8/u32 types.
 */

#ifdef PLATFORM_PC

typedef __UINTPTR_TYPE__ port_uptr;

extern unsigned char gPortEwram[256 * 0x400];
extern unsigned char gPortIwram[32 * 0x400];
extern unsigned char gPortIo[0x400];
extern unsigned char gPortPalram[0x400];
extern unsigned char gPortVram[96 * 0x400];
extern unsigned char gPortOam[0x400];
extern unsigned char gPortSram[64 * 0x400];
/* Nothing reads the cartridge directly in C; ROM_BASE only needs to be a
 * distinct, non-NULL address (soft-reset entry point bookkeeping). */
extern unsigned char gPortRomStub[0x100];

/* DMA: 64-bit pointers don't fit the 32-bit DMA registers, so every DMA
 * request goes through the port, which performs it (or schedules it for
 * HBlank/VBlank) with full-width pointers. */
void Port_DmaSet(unsigned int channel, const void* src, void* dst, unsigned int control);

/* BIOS SWI replacement (Halt, Stop, ...). */
void Port_Syscall(unsigned int number);

/* REG_VCOUNT read from a busy-wait loop: advances a virtual scanline and
 * returns it. */
unsigned short Port_PollVCount(void);

/* Called after the game writes save memory, so the host can persist it. */
void Port_SramWritten(void);

/* The player's ROM stays in memory: the sound engine reads its track scripts,
 * voicegroups and samples in place. Translates a GBA ROM address
 * (0x08xxxxxx) to the host copy, or returns NULL if it is outside the ROM. */
const void* Port_RomPointer(unsigned int gbaAddress);

/* The same bytes as `data` (a ROM data array), but inside the ROM copy. For
 * the few places where the game reads past the end of an array: on the GBA
 * that reads the next data in the ROM, and through this pointer it does too.
 * Returns `data` itself if it isn't ROM data. */
const void* Port_RomMirror(const void* data);
/* Same for any array of `size` bytes: data compiled into the game (not
 * copied from the ROM) is found in the ROM by its contents (it must appear
 * there only once). */
const void* Port_RomFind(const void* data, unsigned int size);

/* Game side of the sound port (port/game/audio_engine.c). */
struct Voice;
/* Builds the sound tables that hold ROM pointers (called once the ROM is
 * loaded). */
void Port_SoundInit(const unsigned char* rom, unsigned int size);
/* Host copy of the voicegroup at a GBA address (never NULL). */
struct Voice* Port_SoundVoicegroup(unsigned int gbaAddress);
/* Converts the cached voicegroups again from their addresses (after a save
 * state restored the cache's address list). */
void Port_SoundRebuildVoicegroups(void);
/* The next 16 samples sound DMA 1/2 would feed to FIFO A/B. Returns 0 when
 * the sound engine hasn't set up its buffers yet. */
int Port_SoundFifoBlock(const signed char** fifoA, const signed char** fifoB);

/* Test warp (MF_WARP, port/src/warp.c): target `index` of the list. room -1
 * means every room of the area (or every value of a mode), event -1 the
 * default event. Returns 0 past the end. The interval is how many frames
 * each target lasts (0 = stay in the only one). */
int Port_WarpTarget(int index, int* area, int* room, int* event);
unsigned int Port_WarpInterval(void);
/* MF_WARP_DIRECT=1: game modes are entered right at boot. */
int Port_WarpDirect(void);
/* MF_WARP_ABILITIES=N: ability count (-1 = all). */
int Port_WarpAbilities(void);
/* Scripted input for the current target (PORT_WARP_SCRIPT_* in port.h). */
void Port_WarpScript(int script);
/* Which way Samus faces (the room script starts that way). */
void Port_WarpFacing(int left);
/* Game side (port/game/warp.c), called from AgbMain: once after
 * InitializeGame, then every frame; and from RoomReset when a new game
 * places Samus. */
void Port_WarpBoot(void);
void Port_WarpFrame(void);
void Port_WarpRoomReset(void);

/* Game state read by the port's extras (second screen, rumble). Filled once
 * per frame by port/game/status.c, which only reads the game's globals. */
enum {
    /* Things that happened since the previous call (PortGameStatus.events). */
    PORT_EVENT_DAMAGE = 1 << 0,      /* Samus got hurt (damage = energy lost) */
    PORT_EVENT_DRAIN = 1 << 1,       /* energy lost without a hit (lava, acid, cold) */
    PORT_EVENT_BOMB = 1 << 2,        /* a bomb exploded */
    PORT_EVENT_POWER_BOMB = 1 << 3,  /* a power bomb exploded */
    PORT_EVENT_LANDING = 1 << 4,     /* hard landing after a long fall */
    PORT_EVENT_SHINESPARK = 1 << 5,  /* a shinespark hit a wall */
    PORT_EVENT_DEATH = 1 << 6,       /* Samus died */
    PORT_EVENT_ITEM = 1 << 7,        /* an item or tank was collected */
};

enum {
    /* Equipment bits (PortGameStatus.items / itemsActive). */
    PORT_ITEM_CHARGE_BEAM = 1 << 0,
    PORT_ITEM_WIDE_BEAM = 1 << 1,
    PORT_ITEM_PLASMA_BEAM = 1 << 2,
    PORT_ITEM_WAVE_BEAM = 1 << 3,
    PORT_ITEM_ICE_BEAM = 1 << 4,
    PORT_ITEM_MISSILES = 1 << 5,
    PORT_ITEM_SUPER_MISSILES = 1 << 6,
    PORT_ITEM_ICE_MISSILES = 1 << 7,
    PORT_ITEM_DIFFUSION_MISSILES = 1 << 8,
    PORT_ITEM_BOMBS = 1 << 9,
    PORT_ITEM_POWER_BOMBS = 1 << 10,
    PORT_ITEM_MORPH_BALL = 1 << 11,
    PORT_ITEM_HIGH_JUMP = 1 << 12,
    PORT_ITEM_SPEED_BOOSTER = 1 << 13,
    PORT_ITEM_SPACE_JUMP = 1 << 14,
    PORT_ITEM_SCREW_ATTACK = 1 << 15,
    PORT_ITEM_VARIA_SUIT = 1 << 16,
    PORT_ITEM_GRAVITY_SUIT = 1 << 17,
    PORT_ITEM_COUNT = 18,
};

#define PORT_MAP_SIZE 32

struct PortGameStatus {
    int playing;  /* in a room (not a menu, cutscene, demo...): events are valid */
    int paused;   /* pause screen (the map and equipment are still valid) */
    int area;     /* 0-6 (Main Deck, sectors 1-6), or -1 */
    int mapX, mapY; /* Samus' cell on the area map */
    int energy, maxEnergy;
    int missiles, maxMissiles;
    int powerBombs, maxPowerBombs;
    unsigned int items, itemsActive; /* PORT_ITEM_* (all active: Fusion has no switches) */
    int hours, minutes, seconds; /* in-game time */

    unsigned int events; /* PORT_EVENT_* */
    int damage;          /* energy lost this frame */
    int fallHeight;      /* height of a hard landing's fall (64 per block) */
    int shake;           /* screen shake strength this frame (0 = none) */
    int powerBombBlast;  /* a power bomb explosion is on screen */

    /* Area map as the pause screen draws it (explored / downloaded rooms):
     * 32x32 BG map entries (tile | flip << 10 | palette << 12), NULL when
     * there's none. The tiles (4bpp, 32 bytes each) and palettes (16 colors
     * each, BGR555) it uses; mapMinX..mapMaxY bound the cells that have a room. */
    const unsigned short* map;
    const unsigned char* mapTiles;
    unsigned int mapTileCount;
    const unsigned short* mapPalettes;
    unsigned int mapPaletteCount;
    int mapMinX, mapMinY, mapMaxX, mapMaxY;
};
/* Fills `status` from the game's globals. Never changes the game. */
void Port_GameStatus(struct PortGameStatus* status);
/* Forget the previous frame (after loading a save state), so the jump in
 * energy, poses... isn't reported as an event. */
void Port_GameStatusReset(void);

/* Widescreen (port/src/widescreen.c). Extra columns the frame has on each
 * side of the GBA screen (0 = option off). */
int Port_WideExtra(void);
/* Display column where GBA x = 0 was drawn in the last wide frame (between 0
 * and 2 * Port_WideExtra()); the game uses it to draw the sprites that are
 * visible in the extra columns. */
int Port_WideOrigin(void);
/* Game side (port/game/widescreen.c), called at the end of every in-game
 * frame: refreshes the extra tilemap columns and reports the frame. */
void Port_WideInGameFrame(int hudEnd, int drewOam);
/* Sprite status for drawing: the GBA status, plus "on screen" when the
 * sprite is in the extra columns of a wide frame. */
struct SpriteData;
unsigned short Port_WideSpriteStatus(struct SpriteData* pSprite);
/* OAM entries [from, to) belong to a sprite with a screen position. */
void Port_WideMarkCenter(int from, int to);
/* Port side: one in-game frame. `oamTags` gives the placement of each of the
 * 128 OAM entries (PORT_WIDE_OAM_*); cameraX is the left edge of the GBA
 * screen and [left, right) the part of the room the camera may show, in
 * pixels of the room. */
enum {
    PORT_WIDE_OAM_SCREEN,    /* drawn like on the GBA, clipped to its screen */
    PORT_WIDE_OAM_WORLD,     /* follows the room, visible in the extra columns */
    PORT_WIDE_OAM_HUD_LEFT,  /* anchored to the left edge of the picture */
    PORT_WIDE_OAM_HUD_RIGHT, /* anchored to the right edge */
    PORT_WIDE_OAM_CENTER,    /* centered on the frame (banners) */
};
void Port_WideSetFrame(const unsigned char* oamTags, int cameraX, int left, int right);

void Port_Log(const char* fmt, ...);
void Port_RequestQuit(void);

#endif /* PLATFORM_PC */

#endif /* PORT_PLATFORM_H */

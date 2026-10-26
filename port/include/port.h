// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — internal interface of the port layer (host side).
//
// Port code is normal hosted C: it may use libc. It must NOT include game
// headers (they redefine NULL and the base types for -nostdinc builds); the
// few game symbols the port needs are declared in port_game.h.
#ifndef PORT_H
#define PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PLATFORM_PC 1
#include "../../include/port/platform.h"

#define GBA_SCREEN_WIDTH 240
#define GBA_SCREEN_HEIGHT 160
// Widest frame the port renders (widescreen option, 2:1).
#define PORT_MAX_SCREEN_WIDTH 320

// ---- I/O register helpers (gPortIo is the 0x04000000 region) -------------
static inline uint16_t Port_IoRead16(uint32_t off)
{
    uint16_t v;
    memcpy(&v, &gPortIo[off], sizeof(v));
    return v;
}

static inline void Port_IoWrite16(uint32_t off, uint16_t v)
{
    memcpy(&gPortIo[off], &v, sizeof(v));
}

enum {
    IO_DISPCNT = 0x000,
    IO_DISPSTAT = 0x004,
    IO_VCOUNT = 0x006,
    IO_KEYINPUT = 0x130,
    IO_IE = 0x200,
    IO_IF = 0x202,
    IO_IME = 0x208,
};

// GBA KEYINPUT bit layout.
enum {
    PORT_KEY_A = 1 << 0,
    PORT_KEY_B = 1 << 1,
    PORT_KEY_SELECT = 1 << 2,
    PORT_KEY_START = 1 << 3,
    PORT_KEY_RIGHT = 1 << 4,
    PORT_KEY_LEFT = 1 << 5,
    PORT_KEY_UP = 1 << 6,
    PORT_KEY_DOWN = 1 << 7,
    PORT_KEY_R = 1 << 8,
    PORT_KEY_L = 1 << 9,
    PORT_KEY_MASK = 0x3FF,
};

// Test warp (port/src/warp.c, MF_WARP): scripted input for the current
// target, ORed with the host's keys.
enum {
    PORT_WARP_SCRIPT_NONE,
    PORT_WARP_SCRIPT_ROOM,
    PORT_WARP_SCRIPT_MENU,
    PORT_WARP_SCRIPT_PAUSE,
};
uint16_t Port_WarpKeys(void);

// ---- port subsystems ------------------------------------------------------
void Port_MemReset(void);

void Port_DmaReset(void);
void Port_DmaOnHBlank(void); // run HBlank-timed channels (one scanline)
void Port_DmaOnVBlank(void); // run VBlank-timed channels

// Sound: plays one frame of DirectSound + PSG output (port/src/apu.c).
#define PORT_AUDIO_RATE 48000
void Port_ApuReset(void);
void Port_ApuRunFrame(void);

void Port_PpuInit(void);
// ABGR8888, width x 160, pitch = width. 240 = the GBA screen.
const uint32_t* Port_PpuRenderFrame(int width);

// Widescreen (port/src/widescreen.c). The width is a setting: 240 (off) or a
// wider even number up to PORT_MAX_SCREEN_WIDTH. The game tells the port
// each in-game frame where the camera is (Port_WideSetFrame, platform.h);
// other frames (menus, cutscenes) are drawn at 240 px, centered.
void Port_WideSetWidth(int width);
int Port_WideWidth(void);
// Renders this frame's picture; *width is its width (Port_WideWidth()).
const uint32_t* Port_WideRenderFrame(int* width);

// One GBA frame: VBlank IRQ, scanline render with HBlank/VCount IRQs,
// present, input, pacing. Called from the game's Halt/VBlankIntrWait.
void Port_RunFrame(void);
uint64_t Port_FrameCount(void);

// ---- host (frontend) interface: host_sdl.c / host_headless.c ---------------
typedef struct PortHostConfig {
    int argc;
    char** argv;
} PortHostConfig;

int Port_Main(int argc, char** argv);

bool Port_HostInit(const PortHostConfig* cfg);
void Port_HostShutdown(void);
// Present a finished frame. Returns false if the user asked to quit.
bool Port_HostPresent(const uint32_t* abgr, int width, int height);
// Pressed keys, as PORT_KEY_* bits (1 = pressed).
uint16_t Port_HostPollKeys(void);
// One frame of sound: interleaved stereo, PORT_AUDIO_RATE Hz.
void Port_HostQueueAudio(const int16_t* samples, int frames);
// Block until the next 1/59.73 s tick (no-op when running unthrottled).
void Port_HostWaitFrame(void);
// The player's ROM. Returns a malloc'd copy of the whole file, or NULL if
// the user gave up (the game then quits). `error` is set on a retry after
// a ROM was rejected, so the host can tell the user why.
uint8_t* Port_HostLoadRom(size_t* size, const char* error);
// Remember a validated ROM for next time (copy it into the app's storage).
void Port_HostRomAccepted(const uint8_t* rom, size_t size);
// Persistent save data (the 64 KiB SRAM/Flash image).
void Port_HostLoadSave(uint8_t* data, size_t size);
void Port_HostWriteSave(const uint8_t* data, size_t size);
// Writes the save through the host if the game changed it.
void Port_FlushSave(void);

// Rumble for this frame (port/src/extras.c). low/high: strength 0..1 of the
// low / high frequency motors of a gamepad, sent every frame. pulse > 0: a
// vibration of that strength and length starts now (for vibrators that only
// take one-shot pulses, like a phone's).
void Port_HostRumble(float low, float high, float pulse, int pulseMs);

// Raises the "quit" request; the frame loop longjmps back to main.
void Port_RequestQuit(void);
bool Port_QuitRequested(void);

// ---- extras: rumble and second screen (port_extras.h) ---------------------------
#include "port_extras.h"
// The game state of the last frame (map included), for the second screen.
const struct PortGameStatus* Port_ExtrasStatus(void);

// ---- save states (port/src/savestate.c) -------------------------------------
// Port variables that are part of the machine state live in the same section
// as the game's globals (IWRAM_DATA), so a save state copies them together.
#define PORT_STATE __attribute__((section("mf_state")))

typedef enum PortStateResult {
    PORT_STATE_OK,
    PORT_STATE_BAD_FILE,    // not a save state, or truncated
    PORT_STATE_OTHER_BUILD, // made by another version / region of the port
} PortStateResult;

// Called by the main loop once per frame, at the only point where the game's
// C stack is always the same: the Halt at the end of AgbMain's loop. Save
// states can only be taken or restored from here.
void Port_HostFrameBoundary(void);
// Size of a save state image (constant for a given build).
size_t Port_StateSize(void);
// Writes the machine state into `buf` (Port_StateSize() bytes). Only from
// Port_HostFrameBoundary().
void Port_StateSave(uint8_t* buf);
// Restores a state written by Port_StateSave, possibly in another run of the
// same build (native pointers are relocated). Only from Port_HostFrameBoundary().
PortStateResult Port_StateLoad(const uint8_t* buf, size_t size);
const char* Port_StateResultText(PortStateResult result);

void Port_Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void Port_Fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif // PORT_H

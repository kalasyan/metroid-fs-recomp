// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — extras built on the game state: rumble (port/src/extras.c)
// and the second screen (port/src/second_screen.c). Plain C, no SDL, so the
// headless host can test them; safe to include from C++.
#ifndef PORT_EXTRAS_H
#define PORT_EXTRAS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the second screen shows.
enum {
    PORT_SCREEN2_OFF,
    PORT_SCREEN2_MAP,    // area map with Samus' position
    PORT_SCREEN2_STATUS, // energy, tanks, ammo and equipment
    PORT_SCREEN2_COUNT
};

// Once per frame, before presenting (port/src/frame.c): reads the game state,
// works out the rumble (calls Port_HostRumble) and keeps the state for the
// second screen.
void Port_ExtrasFrame(void);
// After a save state was loaded: no events from the jump in state.
void Port_ExtrasReset(void);

// Canvas size for a display of displayW x displayH: 256 wide, as tall as the
// display's shape allows (192..448).
void Port_SecondScreenSize(int displayW, int displayH, int* width, int* height);
// Draws the second screen in `mode` (PORT_SCREEN2_*): ABGR8888, pitch = width.
// width must be 256; height 192..448 (see Port_SecondScreenSize).
void Port_SecondScreenRender(uint32_t* pixels, int width, int height, int mode);

#ifdef __cplusplus
}
#endif

#endif // PORT_EXTRAS_H

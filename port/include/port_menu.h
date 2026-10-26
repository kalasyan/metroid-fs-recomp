// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — in-game options menu (Dear ImGui overlay, port/src/menu.cpp).
//
// The SDL frontend opens it at a frame boundary (see Port_HostFrameBoundary)
// and runs it in its own loop, so the game is paused and save states can be
// taken or restored right away.
#ifndef PORT_MENU_H
#define PORT_MENU_H

#include "port_config.h"

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_STATE_SLOTS 4

typedef struct PortMenuHost {
    SDL_Window* window;
    SDL_Renderer* renderer;
    PortConfig* config;
    // Settings changed: apply them (video, sound...). They are written to
    // settings.ini when the menu closes.
    void (*applyConfig)(void);
    // Save states. Return false (with a message in `error`) on failure.
    bool (*saveState)(int slot, char* error, size_t errorSize);
    bool (*loadState)(int slot, char* error, size_t errorSize);
    // Slot description ("17/10 21:30") and thumbnail; false if the slot is empty.
    bool (*stateInfo)(int slot, char* text, size_t textSize, SDL_Texture** thumbnail);
    // Touch pad geometry (window pixels) and drawing, for the layout editor.
    void (*touchButtonCircle)(int button, float* x, float* y, float* r);
    void (*moveTouchButton)(int button, float x, float y);
    void (*drawTouchPad)(int highlight);
    // Short message over the game (ASCII).
    void (*toast)(const char* text);
    void (*quit)(void);
    // Optional: how the game is paced on this screen (ASCII), for the video page.
    void (*syncStatus)(char* text, size_t size);
} PortMenuHost;

bool PortMenu_Init(const PortMenuHost* host);
void PortMenu_Shutdown(void);

void PortMenu_Open(void);
bool PortMenu_IsOpen(void);
// Feeds an SDL event to the menu (only while it is open).
void PortMenu_HandleEvent(const SDL_Event* e);
// Builds and draws one frame of the menu over whatever is already rendered.
void PortMenu_Render(void);
// Layout editor active: the host draws the whole touch pad under the menu.
bool PortMenu_EditingTouchPad(void);

#ifdef __cplusplus
}
#endif

#endif // PORT_MENU_H

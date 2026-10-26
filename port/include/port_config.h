// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — user settings (settings.ini), SDL frontend only.
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#include "port_extras.h"

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Inputs in GBA KEYINPUT bit order, then port-only actions.
enum {
    PORT_INPUT_A,
    PORT_INPUT_B,
    PORT_INPUT_SELECT,
    PORT_INPUT_START,
    PORT_INPUT_RIGHT,
    PORT_INPUT_LEFT,
    PORT_INPUT_UP,
    PORT_INPUT_DOWN,
    PORT_INPUT_R,
    PORT_INPUT_L,
    PORT_INPUT_GBA_COUNT,
    PORT_INPUT_FAST_FORWARD = PORT_INPUT_GBA_COUNT,
    PORT_INPUT_COUNT
};

enum {
    PORT_TOUCH_AUTO, // shown after the screen is touched, hidden by a gamepad
    PORT_TOUCH_ON,
    PORT_TOUCH_OFF,
};

enum {
    PORT_FILTER_NONE,
    PORT_FILTER_LCD, // pixel grid, like the GBA screen
    PORT_FILTER_CRT, // scanlines
    PORT_FILTER_COUNT
};

// Picture width (widescreen: more of the room on the sides).
enum {
    PORT_WIDE_OFF,  // 240x160, like the GBA
    PORT_WIDE_AUTO, // as wide as the screen (up to 2:1)
    PORT_WIDE_16_10,
    PORT_WIDE_16_9,
    PORT_WIDE_2_1,
    PORT_WIDE_COUNT
};

// On-screen touch buttons.
enum {
    PORT_TOUCH_UP,
    PORT_TOUCH_DOWN,
    PORT_TOUCH_LEFT,
    PORT_TOUCH_RIGHT,
    PORT_TOUCH_A,
    PORT_TOUCH_B,
    PORT_TOUCH_L,
    PORT_TOUCH_R,
    PORT_TOUCH_SELECT,
    PORT_TOUCH_START,
    PORT_TOUCH_MENU, // opens the options menu
    PORT_TOUCH_BUTTON_COUNT
};

enum {
    PORT_ANCHOR_LEFT,
    PORT_ANCHOR_CENTER,
    PORT_ANCHOR_RIGHT,
};

// Position in units of the window height, measured from the anchor (left
// edge, center or right edge, towards the middle of the screen), so the
// layout keeps its shape on any aspect ratio.
typedef struct PortTouchButton {
    int anchor;
    float x, y, r;
    int input; // PORT_INPUT_* it presses (-1: the menu button)
} PortTouchButton;

typedef struct PortBinding {
    SDL_Scancode key;
    SDL_GamepadButton button;
    SDL_GamepadAxis axis; // optional analog direction
    int axisSign;         // -1 / +1
} PortBinding;

typedef struct PortConfig {
    // [video]
    int windowScale;
    bool fullscreen;
    bool vsync;
    bool integerScaling;
    bool smoothScaling;
    int filter; // PORT_FILTER_*
    bool syncToDisplay; // game speed locked to the refresh rate (port_pacing.h)
    bool frameBlend;    // mix the last two frames on 90/144 Hz... screens
    int widescreen; // PORT_WIDE_*
    // [audio]
    int volume; // percent, 0 = muted
    // [input]
    int touchControls;
    int touchOpacity; // percent
    int stickDeadzone; // percent
    int fastForwardSpeed;
    bool fastForwardToggle; // the key switches it on/off instead of holding
    PortBinding bindings[PORT_INPUT_COUNT];
    // [touch]
    PortTouchButton touch[PORT_TOUCH_BUTTON_COUNT];
    // [rumble]
    bool rumbleGamepad; // the gamepad's motors
    bool rumbleDevice;  // the phone's / handheld's own vibrator (Android)
    int rumbleStrength; // percent
    // [second_screen]
    int secondScreen; // PORT_SCREEN2_*: AYN Thor's lower screen, a window on PC
} PortConfig;

void PortConfig_Defaults(PortConfig* cfg);
bool PortConfig_Load(PortConfig* cfg, const char* path);
bool PortConfig_Save(const PortConfig* cfg, const char* path);
const char* PortConfig_InputName(int input);
void PortConfig_DefaultBindings(PortConfig* cfg);
void PortConfig_DefaultTouchLayout(PortConfig* cfg);
const char* PortConfig_FilterName(int filter);
const char* PortConfig_SecondScreenName(int mode);
// Picture width for a widescreen setting on a screen of the given aspect
// ratio (width / height; only used by PORT_WIDE_AUTO).
int PortConfig_WideWidth(int widescreen, float screenAspect);

#ifdef __cplusplus
}
#endif

#endif // PORT_CONFIG_H

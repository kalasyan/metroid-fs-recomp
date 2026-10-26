// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — settings.ini reader/writer.
//
// Plain INI so it can be edited by hand on the device:
//
//   [video]
//   window_scale = 4
//   integer_scaling = true
//   sync_to_display = false      ; lock the game to 60/120 Hz screens
//   frame_blend = false          ; mix frames on 90/144 Hz screens
//   widescreen = off             ; off | auto | 16:10 | 16:9 | 2:1
//   [audio]
//   volume = 100                 ; percent, 0 = muted
//   [input]
//   touch_controls = auto        ; auto | on | off
//   [keyboard]
//   a = X                        ; SDL scancode names
//   [gamepad]
//   a = b                        ; SDL gamepad button names (south/east/...)
//   left = dpleft, leftx-        ; button and/or axis with a sign
//   [touch]
//   a = right 0.205 0.620 0.085 a ; anchor, x, y, radius (window heights), input
//   [rumble]
//   gamepad = true               ; gamepad motors
//   device = true                ; the phone's vibrator (Android)
//   strength = 100               ; percent
//   [second_screen]
//   mode = map                   ; off | map | status
#include "port_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* const sInputNames[PORT_INPUT_COUNT] = {
    "a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "fast_forward",
};

static const char* const sTouchNames[PORT_TOUCH_BUTTON_COUNT] = {
    "up", "down", "left", "right", "a", "b", "l", "r", "select", "start", "menu",
};

static const char* const sAnchorNames[] = { "left", "center", "right" };

static const char* const sFilterNames[PORT_FILTER_COUNT] = { "none", "lcd", "crt" };
static const char* const sWideNames[PORT_WIDE_COUNT] = { "off", "auto", "16:10", "16:9", "2:1" };

static const char* const sSecondScreenNames[PORT_SCREEN2_COUNT] = { "off", "map", "status" };

const char* PortConfig_InputName(int input)
{
    return (input >= 0 && input < PORT_INPUT_COUNT) ? sInputNames[input] : "?";
}

const char* PortConfig_FilterName(int filter)
{
    return (filter >= 0 && filter < PORT_FILTER_COUNT) ? sFilterNames[filter] : "none";
}

const char* PortConfig_SecondScreenName(int mode)
{
    return (mode >= 0 && mode < PORT_SCREEN2_COUNT) ? sSecondScreenNames[mode] : "off";
}

int PortConfig_WideWidth(int widescreen, float screenAspect)
{
    int width;

    switch (widescreen) {
    case PORT_WIDE_AUTO:
        width = (int)(160.0f * screenAspect + 0.5f);
        break;
    case PORT_WIDE_16_10: width = 256; break;
    case PORT_WIDE_16_9: width = 284; break; // 160 * 16 / 9 = 284.4
    case PORT_WIDE_2_1: width = 320; break;
    default: width = 240; break;
    }
    if (width < 240)
        width = 240;
    if (width > 320)
        width = 320;
    return width & ~1;
}

void PortConfig_DefaultTouchLayout(PortConfig* cfg)
{
    static const PortTouchButton defaults[PORT_TOUCH_BUTTON_COUNT] = {
        [PORT_TOUCH_UP] = { PORT_ANCHOR_LEFT, 0.255f, 0.55f, 0.075f, PORT_INPUT_UP },
        [PORT_TOUCH_DOWN] = { PORT_ANCHOR_LEFT, 0.255f, 0.85f, 0.075f, PORT_INPUT_DOWN },
        [PORT_TOUCH_LEFT] = { PORT_ANCHOR_LEFT, 0.105f, 0.70f, 0.075f, PORT_INPUT_LEFT },
        [PORT_TOUCH_RIGHT] = { PORT_ANCHOR_LEFT, 0.405f, 0.70f, 0.075f, PORT_INPUT_RIGHT },
        [PORT_TOUCH_A] = { PORT_ANCHOR_RIGHT, 0.205f, 0.62f, 0.085f, PORT_INPUT_A },
        [PORT_TOUCH_B] = { PORT_ANCHOR_RIGHT, 0.385f, 0.78f, 0.085f, PORT_INPUT_B },
        [PORT_TOUCH_L] = { PORT_ANCHOR_LEFT, 0.17f, 0.12f, 0.07f, PORT_INPUT_L },
        [PORT_TOUCH_R] = { PORT_ANCHOR_RIGHT, 0.17f, 0.12f, 0.07f, PORT_INPUT_R },
        [PORT_TOUCH_SELECT] = { PORT_ANCHOR_CENTER, -0.0585f, 0.94f, 0.045f, PORT_INPUT_SELECT },
        [PORT_TOUCH_START] = { PORT_ANCHOR_CENTER, 0.0585f, 0.94f, 0.045f, PORT_INPUT_START },
        [PORT_TOUCH_MENU] = { PORT_ANCHOR_CENTER, 0.0f, 0.06f, 0.04f, -1 },
    };
    memcpy(cfg->touch, defaults, sizeof(defaults));
}

void PortConfig_DefaultBindings(PortConfig* cfg)
{
    static const PortBinding defaults[PORT_INPUT_COUNT] = {
        // GBA A/B on the right face buttons, matching the GBA's layout:
        // A = east (B on Xbox, A on Nintendo-style pads), B = south.
        [PORT_INPUT_A] = { SDL_SCANCODE_X, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_AXIS_INVALID, 0 },
        [PORT_INPUT_B] = { SDL_SCANCODE_Z, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_AXIS_INVALID, 0 },
        [PORT_INPUT_SELECT] = { SDL_SCANCODE_BACKSPACE, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_AXIS_INVALID, 0 },
        [PORT_INPUT_START] = { SDL_SCANCODE_RETURN, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_AXIS_INVALID, 0 },
        [PORT_INPUT_RIGHT] = { SDL_SCANCODE_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_AXIS_LEFTX, +1 },
        [PORT_INPUT_LEFT] = { SDL_SCANCODE_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_AXIS_LEFTX, -1 },
        [PORT_INPUT_UP] = { SDL_SCANCODE_UP, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_AXIS_LEFTY, -1 },
        [PORT_INPUT_DOWN] = { SDL_SCANCODE_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_AXIS_LEFTY, +1 },
        [PORT_INPUT_R] = { SDL_SCANCODE_S, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, +1 },
        [PORT_INPUT_L] = { SDL_SCANCODE_A, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, +1 },
        [PORT_INPUT_FAST_FORWARD] = { SDL_SCANCODE_TAB, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_AXIS_INVALID, 0 },
    };

    memcpy(cfg->bindings, defaults, sizeof(defaults));
}

void PortConfig_Defaults(PortConfig* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->windowScale = 4;
    cfg->fullscreen = false;
    cfg->vsync = false;
    cfg->integerScaling = false;
    cfg->smoothScaling = false;
    cfg->volume = 100;
    cfg->touchControls = PORT_TOUCH_AUTO;
    cfg->touchOpacity = 35;
    cfg->stickDeadzone = 40;
    cfg->fastForwardSpeed = 3;
    cfg->fastForwardToggle = false;
    cfg->filter = PORT_FILTER_NONE;
    cfg->syncToDisplay = false;
    cfg->frameBlend = false;
    cfg->widescreen = PORT_WIDE_OFF;
    PortConfig_DefaultBindings(cfg);
    PortConfig_DefaultTouchLayout(cfg);
    cfg->rumbleGamepad = false;
    cfg->rumbleDevice = false;
    cfg->rumbleStrength = 100;
    cfg->secondScreen = PORT_SCREEN2_OFF;
#ifdef __ANDROID__
    cfg->fullscreen = true;
    // Only does something on devices with a second display (AYN Thor).
    cfg->secondScreen = PORT_SCREEN2_MAP;
#endif
}

static char* Trim(char* s)
{
    char* end;
    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static bool ParseBool(const char* v)
{
    return SDL_strcasecmp(v, "true") == 0 || SDL_strcasecmp(v, "yes") == 0 || SDL_strcasecmp(v, "on") == 0 ||
           strcmp(v, "1") == 0;
}

static int ParseInt(const char* v, int lo, int hi, int fallback)
{
    char* end;
    long n = strtol(v, &end, 10);
    if (end == v || n < lo || n > hi)
        return fallback;
    return (int)n;
}

static int FindInput(const char* name)
{
    for (int i = 0; i < PORT_INPUT_COUNT; i++) {
        if (SDL_strcasecmp(name, sInputNames[i]) == 0)
            return i;
    }
    return -1;
}

static int FindName(const char* const* names, int count, const char* name)
{
    for (int i = 0; i < count; i++) {
        if (SDL_strcasecmp(name, names[i]) == 0)
            return i;
    }
    return -1;
}

// "right 0.205 0.620 0.085 a"
static void ParseTouchButton(PortTouchButton* b, int button, char* value)
{
    char anchor[16], input[16] = "";
    float x, y, r;

    if (sscanf(value, "%15s %f %f %f %15s", anchor, &x, &y, &r, input) < 4)
        return;
    const int a = FindName(sAnchorNames, 3, anchor);
    if (a < 0 || x < -1.0f || x > 2.0f || y < 0.0f || y > 1.0f || r < 0.01f || r > 0.3f)
        return;
    b->anchor = a;
    b->x = x;
    b->y = y;
    b->r = r;
    if (button != PORT_TOUCH_MENU && input[0]) {
        const int in = FindInput(input);
        if (in >= 0 && in < PORT_INPUT_GBA_COUNT)
            b->input = in;
    }
}

static void ParseGamepadBinding(PortBinding* b, char* value)
{
    b->button = SDL_GAMEPAD_BUTTON_INVALID;
    b->axis = SDL_GAMEPAD_AXIS_INVALID;
    b->axisSign = 0;
    for (char* tok = strtok(value, ","); tok; tok = strtok(NULL, ",")) {
        tok = Trim(tok);
        size_t len = strlen(tok);
        if (len == 0 || SDL_strcasecmp(tok, "none") == 0)
            continue;
        if (tok[len - 1] == '+' || tok[len - 1] == '-') {
            const int sign = tok[len - 1] == '+' ? +1 : -1;
            tok[len - 1] = '\0';
            SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(tok);
            if (axis != SDL_GAMEPAD_AXIS_INVALID) {
                b->axis = axis;
                b->axisSign = sign;
            }
            continue;
        }
        SDL_GamepadButton button = SDL_GetGamepadButtonFromString(tok);
        if (button != SDL_GAMEPAD_BUTTON_INVALID)
            b->button = button;
    }
}

bool PortConfig_Load(PortConfig* cfg, const char* path)
{
    char line[512];
    char section[32] = "";
    FILE* f = fopen(path, "r");

    if (!f)
        return false;

    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        char* comment = strpbrk(s, ";#");
        if (comment)
            *comment = '\0';
        s = Trim(s);
        if (*s == '\0')
            continue;
        if (*s == '[') {
            char* close = strchr(s, ']');
            if (close) {
                *close = '\0';
                SDL_strlcpy(section, s + 1, sizeof(section));
            }
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char* key = Trim(s);
        char* value = Trim(eq + 1);

        if (strcmp(section, "video") == 0) {
            if (strcmp(key, "window_scale") == 0)
                cfg->windowScale = ParseInt(value, 1, 10, cfg->windowScale);
            else if (strcmp(key, "fullscreen") == 0)
                cfg->fullscreen = ParseBool(value);
            else if (strcmp(key, "vsync") == 0)
                cfg->vsync = ParseBool(value);
            else if (strcmp(key, "integer_scaling") == 0)
                cfg->integerScaling = ParseBool(value);
            else if (strcmp(key, "smooth_scaling") == 0)
                cfg->smoothScaling = ParseBool(value);
            else if (strcmp(key, "filter") == 0) {
                const int f = FindName(sFilterNames, PORT_FILTER_COUNT, value);
                if (f >= 0)
                    cfg->filter = f;
            } else if (strcmp(key, "sync_to_display") == 0)
                cfg->syncToDisplay = ParseBool(value);
            else if (strcmp(key, "frame_blend") == 0)
                cfg->frameBlend = ParseBool(value);
            else if (strcmp(key, "widescreen") == 0) {
                const int w = FindName(sWideNames, PORT_WIDE_COUNT, value);
                if (w >= 0)
                    cfg->widescreen = w;
                else if (ParseBool(value))
                    cfg->widescreen = PORT_WIDE_16_9;
            }
        } else if (strcmp(section, "audio") == 0) {
            if (strcmp(key, "volume") == 0)
                cfg->volume = ParseInt(value, 0, 100, cfg->volume);
        } else if (strcmp(section, "input") == 0) {
            if (strcmp(key, "touch_controls") == 0) {
                if (SDL_strcasecmp(value, "on") == 0)
                    cfg->touchControls = PORT_TOUCH_ON;
                else if (SDL_strcasecmp(value, "off") == 0)
                    cfg->touchControls = PORT_TOUCH_OFF;
                else
                    cfg->touchControls = PORT_TOUCH_AUTO;
            } else if (strcmp(key, "touch_opacity") == 0) {
                cfg->touchOpacity = ParseInt(value, 0, 100, cfg->touchOpacity);
            } else if (strcmp(key, "stick_deadzone") == 0) {
                cfg->stickDeadzone = ParseInt(value, 5, 95, cfg->stickDeadzone);
            } else if (strcmp(key, "fast_forward_speed") == 0) {
                cfg->fastForwardSpeed = ParseInt(value, 2, 10, cfg->fastForwardSpeed);
            } else if (strcmp(key, "fast_forward_toggle") == 0) {
                cfg->fastForwardToggle = ParseBool(value);
            }
        } else if (strcmp(section, "rumble") == 0) {
            if (strcmp(key, "gamepad") == 0)
                cfg->rumbleGamepad = ParseBool(value);
            else if (strcmp(key, "device") == 0)
                cfg->rumbleDevice = ParseBool(value);
            else if (strcmp(key, "strength") == 0)
                cfg->rumbleStrength = ParseInt(value, 0, 100, cfg->rumbleStrength);
        } else if (strcmp(section, "second_screen") == 0) {
            if (strcmp(key, "mode") == 0) {
                const int m = FindName(sSecondScreenNames, PORT_SCREEN2_COUNT, value);
                if (m >= 0)
                    cfg->secondScreen = m;
            }
        } else if (strcmp(section, "keyboard") == 0) {
            const int input = FindInput(key);
            if (input >= 0)
                cfg->bindings[input].key = SDL_strcasecmp(value, "none") == 0 ? SDL_SCANCODE_UNKNOWN
                                                                              : SDL_GetScancodeFromName(value);
        } else if (strcmp(section, "gamepad") == 0) {
            const int input = FindInput(key);
            if (input >= 0)
                ParseGamepadBinding(&cfg->bindings[input], value);
        } else if (strcmp(section, "touch") == 0) {
            const int button = FindName(sTouchNames, PORT_TOUCH_BUTTON_COUNT, key);
            if (button >= 0)
                ParseTouchButton(&cfg->touch[button], button, value);
        }
    }

    fclose(f);
    return true;
}

bool PortConfig_Save(const PortConfig* cfg, const char* path)
{
    static const char* const touchNames[] = { "auto", "on", "off" };
    FILE* f = fopen(path, "w");

    if (!f)
        return false;

    fprintf(f, "; Metroid Fusion (mf-android) settings\n\n");
    fprintf(f, "[video]\n");
    fprintf(f, "window_scale = %d\n", cfg->windowScale);
    fprintf(f, "fullscreen = %s\n", cfg->fullscreen ? "true" : "false");
    fprintf(f, "vsync = %s\n", cfg->vsync ? "true" : "false");
    fprintf(f, "integer_scaling = %s\n", cfg->integerScaling ? "true" : "false");
    fprintf(f, "smooth_scaling = %s\n", cfg->smoothScaling ? "true" : "false");
    fprintf(f, "filter = %s ; none | lcd | crt\n", sFilterNames[cfg->filter]);
    fprintf(f, "sync_to_display = %s ; game speed locked to 60/120 Hz screens (+0.46 %%)\n",
            cfg->syncToDisplay ? "true" : "false");
    fprintf(f, "frame_blend = %s ; mix frames on 90/144 Hz screens\n", cfg->frameBlend ? "true" : "false");
    fprintf(f, "widescreen = %s ; off | auto | 16:10 | 16:9 | 2:1\n\n", sWideNames[cfg->widescreen]);

    fprintf(f, "[audio]\n");
    fprintf(f, "volume = %d ; percent, 0 = muted\n\n", cfg->volume);

    fprintf(f, "[input]\n");
    fprintf(f, "touch_controls = %s ; auto | on | off\n", touchNames[cfg->touchControls]);
    fprintf(f, "touch_opacity = %d\n", cfg->touchOpacity);
    fprintf(f, "stick_deadzone = %d\n", cfg->stickDeadzone);
    fprintf(f, "fast_forward_speed = %d\n", cfg->fastForwardSpeed);
    fprintf(f, "fast_forward_toggle = %s\n\n", cfg->fastForwardToggle ? "true" : "false");

    fprintf(f, "[rumble]\n");
    fprintf(f, "gamepad = %s\n", cfg->rumbleGamepad ? "true" : "false");
    fprintf(f, "device = %s ; the phone's own vibrator (Android)\n", cfg->rumbleDevice ? "true" : "false");
    fprintf(f, "strength = %d ; percent\n\n", cfg->rumbleStrength);

    fprintf(f, "[second_screen]\n");
    fprintf(f, "mode = %s ; off | map | status\n\n", sSecondScreenNames[cfg->secondScreen]);

    fprintf(f, "[keyboard]\n");
    for (int i = 0; i < PORT_INPUT_COUNT; i++) {
        const char* name = SDL_GetScancodeName(cfg->bindings[i].key);
        fprintf(f, "%s = %s\n", sInputNames[i], (name && *name) ? name : "none");
    }

    fprintf(f, "\n[gamepad]\n");
    for (int i = 0; i < PORT_INPUT_COUNT; i++) {
        const PortBinding* b = &cfg->bindings[i];
        const char* button = b->button != SDL_GAMEPAD_BUTTON_INVALID ? SDL_GetGamepadStringForButton(b->button) : NULL;
        const char* axis = b->axis != SDL_GAMEPAD_AXIS_INVALID ? SDL_GetGamepadStringForAxis(b->axis) : NULL;
        fprintf(f, "%s = ", sInputNames[i]);
        if (button)
            fprintf(f, "%s", button);
        if (axis)
            fprintf(f, "%s%s%c", button ? ", " : "", axis, b->axisSign < 0 ? '-' : '+');
        if (!button && !axis)
            fprintf(f, "none");
        fputc('\n', f);
    }

    fprintf(f, "\n[touch]\n");
    for (int i = 0; i < PORT_TOUCH_BUTTON_COUNT; i++) {
        const PortTouchButton* b = &cfg->touch[i];
        fprintf(f, "%s = %s %.3f %.3f %.3f", sTouchNames[i], sAnchorNames[b->anchor], b->x, b->y, b->r);
        if (b->input >= 0)
            fprintf(f, " %s", sInputNames[b->input]);
        fputc('\n', f);
    }

    fclose(f);
    return true;
}

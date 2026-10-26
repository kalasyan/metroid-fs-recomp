// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — SDL3 frontend (desktop and Android).
//
// Video: the 240x160 frame is uploaded to a streaming texture and scaled to
// the window (aspect-correct, optional integer scaling / smooth filter / LCD
// or CRT effect).
// Audio: the port's 48 kHz output goes to an SDL audio stream.
// Input: keyboard, gamepads (the AYN Thor's built-in controls show up as one)
// and, on touch screens, an on-screen pad drawn over the game.
// Menu: Esc / F1, Select+Start, the Android back button or the touch menu
// button open the options menu (port/src/menu.cpp) with save states.
// Everything configurable lives in settings.ini in the app's pref folder.
#include "port.h"
#include "port_config.h"
#include "port_assets.h"
#include "port_menu.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>

#define GBA_FRAME_NS 16742706ull // 280896 cycles / 16.777216 MHz
#define THUMB_BYTES (GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 4)

static SDL_Window* sWindow;
static SDL_Renderer* sRenderer;
static SDL_Texture* sTexture;
static SDL_Gamepad* sGamepad;
static SDL_AudioStream* sAudio;
static char* sPrefPath;
static char sRegionSuffix[8]; // "" for US, "_eu" / "_jp" otherwise
static char sSavePath[1024];
static char sConfigPath[1024];
static const char* sRomArgPath; // mf [path/to/rom.gba]

static PortConfig sConfig;
static bool sFastForwardHeld;
static bool sFastForwardOn; // toggle mode
static bool sTouchVisible;
static uint64_t sNextFrameNs;
static uint32_t sLastFrame[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];

static bool sMenuRequested; // opened at the next frame boundary
static bool sMenuReady;
static bool sInputLatch;    // ignore the game's input until everything is released
static int sStateSlot = 1;  // F5 / F8
static int sPendingState;   // 0, or +slot (save) / -slot (load), from hotkeys

static char sToast[96];
static uint64_t sToastUntilNs;

static SDL_Texture* sMask;         // LCD/CRT pattern, multiplied over the game
static SDL_Texture* sEffectTarget; // the game, scaled up for the pattern
static int sMaskW, sMaskH, sMaskFilter;

static SDL_Texture* sThumbs[PORT_STATE_SLOTS + 1];
static bool sThumbsValid[PORT_STATE_SLOTS + 1];

static void Toast(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(sToast, sizeof(sToast), fmt, ap);
    va_end(ap);
    sToastUntilNs = SDL_GetTicksNS() + 2000000000ull;
}

// ---- layout -------------------------------------------------------------------

static void OutputSize(int* w, int* h)
{
    if (!SDL_GetCurrentRenderOutputSize(sRenderer, w, h))
        SDL_GetWindowSizeInPixels(sWindow, w, h);
}

// Where the GBA screen goes in the window (pixels).
static SDL_FRect GameRect(void)
{
    int w, h;
    OutputSize(&w, &h);
    float scale = SDL_min((float)w / GBA_SCREEN_WIDTH, (float)h / GBA_SCREEN_HEIGHT);
    if (sConfig.integerScaling && scale >= 1.0f)
        scale = SDL_floorf(scale);
    const float gw = SDL_floorf(GBA_SCREEN_WIDTH * scale), gh = SDL_floorf(GBA_SCREEN_HEIGHT * scale);
    return (SDL_FRect){ SDL_floorf((w - gw) * 0.5f), SDL_floorf((h - gh) * 0.5f), gw, gh };
}

// ---- touch pad ----------------------------------------------------------------

#define MAX_FINGERS 10
static struct {
    SDL_FingerID id;
    bool down;
    float x, y; // window pixels
} sFingers[MAX_FINGERS];

static void TouchButtonCircle(int i, float* x, float* y, float* r)
{
    const PortTouchButton* b = &sConfig.touch[i];
    int w, h;
    OutputSize(&w, &h);
    *r = b->r * (float)h;
    *y = b->y * (float)h;
    switch (b->anchor) {
    case PORT_ANCHOR_LEFT:
        *x = b->x * (float)h;
        break;
    case PORT_ANCHOR_RIGHT:
        *x = (float)w - b->x * (float)h;
        break;
    default:
        *x = (float)w * 0.5f + b->x * (float)h;
        break;
    }
}

// Layout editor: put button i's center at (x, y), anchored to the nearest
// third of the screen.
static void MoveTouchButton(int i, float x, float y)
{
    PortTouchButton* b = &sConfig.touch[i];
    int w, h;
    OutputSize(&w, &h);
    if (h <= 0)
        return;
    x = SDL_clamp(x, 0.0f, (float)w);
    y = SDL_clamp(y, 0.0f, (float)h);
    if (x < w / 3.0f) {
        b->anchor = PORT_ANCHOR_LEFT;
        b->x = x / h;
    } else if (x > w * 2.0f / 3.0f) {
        b->anchor = PORT_ANCHOR_RIGHT;
        b->x = (w - x) / h;
    } else {
        b->anchor = PORT_ANCHOR_CENTER;
        b->x = (x - w * 0.5f) / h;
    }
    b->y = y / h;
}

// Touch button under (x, y), or -1. Generous hit area so thumbs can roll
// between buttons.
static bool TouchHit(int i, float px, float py)
{
    float x, y, r;
    TouchButtonCircle(i, &x, &y, &r);
    const float dx = px - x, dy = py - y;
    return dx * dx + dy * dy <= (r * 1.35f) * (r * 1.35f);
}

static uint16_t TouchKeys(void)
{
    uint16_t keys = 0;

    if (!sTouchVisible)
        return 0;
    for (int f = 0; f < MAX_FINGERS; f++) {
        if (!sFingers[f].down)
            continue;
        for (int i = 0; i < PORT_TOUCH_BUTTON_COUNT; i++) {
            const int input = sConfig.touch[i].input;
            if (input >= 0 && input < PORT_INPUT_GBA_COUNT && TouchHit(i, sFingers[f].x, sFingers[f].y))
                keys |= (uint16_t)(1u << input);
        }
    }
    return keys;
}

static void DrawCircle(float cx, float cy, float r)
{
    // Filled circle from horizontal spans; cheap enough for ~10 buttons.
    for (float dy = -r; dy <= r; dy += 1.0f) {
        const float dx = SDL_sqrtf(SDL_max(r * r - dy * dy, 0.0f));
        SDL_RenderLine(sRenderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void DrawRing(float cx, float cy, float r)
{
    for (int i = 0; i < 48; i++) {
        const float a0 = (float)i * SDL_PI_F * 2.0f / 48.0f, a1 = (float)(i + 1) * SDL_PI_F * 2.0f / 48.0f;
        SDL_RenderLine(sRenderer, cx + r * SDL_cosf(a0), cy + r * SDL_sinf(a0), cx + r * SDL_cosf(a1),
                       cy + r * SDL_sinf(a1));
    }
}

static void DrawButtonLabel(int i, float x, float y, float r)
{
    static const char* const labels[PORT_INPUT_GBA_COUNT] = { "A", "B", "SEL", "STA", ">", "<", "^", "v", "R", "L" };
    const int input = sConfig.touch[i].input;
    const char* text = input >= 0 ? labels[input] : "=";
    const float scale = SDL_max(1.0f, SDL_floorf(r / 12.0f));
    const float tw = (float)SDL_strlen(text) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale;

    SDL_SetRenderScale(sRenderer, scale, scale);
    SDL_RenderDebugText(sRenderer, (x - tw * 0.5f) / scale, (y - 4.0f * scale) / scale, text);
    SDL_SetRenderScale(sRenderer, 1.0f, 1.0f);
}

// highlight >= 0: layout editor, draws every button with outlines and labels.
static void DrawTouchPad(uint16_t pressed, int highlight)
{
    const bool editing = highlight >= -1 && PortMenu_EditingTouchPad();
    const int opacity = editing ? SDL_max(sConfig.touchOpacity, 25) : sConfig.touchOpacity;

    if (opacity <= 0)
        return;
    SDL_SetRenderDrawBlendMode(sRenderer, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < PORT_TOUCH_BUTTON_COUNT; i++) {
        float x, y, r;
        const int input = sConfig.touch[i].input;
        const bool down = input >= 0 && (pressed & (1u << input)) != 0;
        const Uint8 alpha = (Uint8)SDL_min(255.0f, opacity * (down ? 2.0f : 1.0f) * 255.0f / 100.0f);
        TouchButtonCircle(i, &x, &y, &r);
        SDL_SetRenderDrawColor(sRenderer, 255, 255, 255, alpha);
        DrawCircle(x, y, r);
        if (i == PORT_TOUCH_MENU || editing) {
            SDL_SetRenderDrawColor(sRenderer, 255, 255, 255, (Uint8)SDL_min(255, alpha * 3));
            DrawButtonLabel(i, x, y, r);
        }
        if (editing) {
            if (i == highlight)
                SDL_SetRenderDrawColor(sRenderer, 255, 200, 40, 255);
            else
                SDL_SetRenderDrawColor(sRenderer, 255, 255, 255, 160);
            DrawRing(x, y, r);
            DrawRing(x, y, r + 1.0f);
        }
    }
}

static void MenuDrawTouchPad(int highlight)
{
    DrawTouchPad(0, highlight);
}

// ---- video --------------------------------------------------------------------

static void OpenAudio(void)
{
    const SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, PORT_AUDIO_RATE };
    sAudio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (sAudio)
        SDL_ResumeAudioStreamDevice(sAudio);
    else
        Port_Log("no sound: %s", SDL_GetError()); // the game still runs
}

static void ApplyVideoConfig(void)
{
    SDL_SetRenderVSync(sRenderer, sConfig.vsync ? 1 : 0);
    SDL_SetTextureScaleMode(sTexture, sConfig.smoothScaling ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    if (sAudio)
        SDL_SetAudioStreamGain(sAudio, sConfig.volume / 100.0f);
}

// The LCD/CRT effect. The game is drawn k times its size into sEffectTarget
// (k = output pixels per GBA pixel, rounded down), a k x k pattern per GBA pixel
// is multiplied over it, and the result is scaled up to the game rect with
// linear filtering. Every GBA pixel gets exactly the same pattern, so the grid
// stays even at any scale: drawing the pattern straight at the output size
// gave gaps of one or two pixels at 6.75x (1080p), which beat into bigger
// blocks over the picture.
static int EffectScale(const SDL_FRect* dst)
{
    const int k = (int)SDL_floorf(dst->h / GBA_SCREEN_HEIGHT + 0.01f);
    return SDL_clamp(k, 1, 12);
}

// Brightness of row/column i of a k-pixel cell.
static float LcdProfile(int i, int k)
{
    if (i == k - 1)
        return 0.70f; // the gap between two LCD cells
    if (k >= 6 && (i == 0 || i == k - 2))
        return 0.92f; // soft edges on big cells
    return 1.0f;
}

static void UpdateMask(int k)
{
    const int w = GBA_SCREEN_WIDTH * k, h = GBA_SCREEN_HEIGHT * k;
    if (sMask && sMaskW == w && sMaskH == h && sMaskFilter == sConfig.filter)
        return;
    if (sMask)
        SDL_DestroyTexture(sMask);
    if (sEffectTarget)
        SDL_DestroyTexture(sEffectTarget);
    if (sEffectTarget)
        SDL_DestroyTexture(sEffectTarget);
    sMask = sEffectTarget = NULL;
    sMaskW = w;
    sMaskH = h;
    sMaskFilter = sConfig.filter;
    // Too small for a pattern: one pixel of gap or scanline would be half
    // of each GBA pixel.
    if (k < (sConfig.filter == PORT_FILTER_LCD ? 3 : 2))
        return;

    uint32_t tile[12 * 12];
    for (int y = 0; y < k; y++) {
        for (int x = 0; x < k; x++) {
            float r = 1.0f, g = 1.0f, b = 1.0f;
            if (sConfig.filter == PORT_FILTER_LCD) {
                r = g = b = LcdProfile(x, k) * LcdProfile(y, k);
            } else if (sConfig.filter == PORT_FILTER_CRT) {
                // Scanlines plus a faint aperture grille.
                const float line = 0.68f + 0.32f * SDL_sinf(SDL_PI_F * (y + 0.5f) / k);
                r = g = b = line;
                switch (x % 3) {
                case 0: g *= 0.9f; b *= 0.9f; break;
                case 1: r *= 0.9f; b *= 0.9f; break;
                default: r *= 0.9f; g *= 0.9f; break;
                }
            }
            tile[y * k + x] = 0xFF000000u | (uint32_t)(b * 255.0f) << 16 | (uint32_t)(g * 255.0f) << 8 |
                              (uint32_t)(r * 255.0f);
        }
    }
    uint32_t* px = malloc((size_t)w * h * 4);
    if (!px)
        return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[(size_t)y * w + x] = tile[(y % k) * k + x % k];
    sMask = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    sEffectTarget = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w, h);
    if (sMask && sEffectTarget) {
        SDL_UpdateTexture(sMask, NULL, px, w * 4);
        SDL_SetTextureBlendMode(sMask, SDL_BLENDMODE_MOD);
        SDL_SetTextureScaleMode(sMask, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(sEffectTarget, SDL_BLENDMODE_NONE);
    } else {
        if (sMask)
            SDL_DestroyTexture(sMask);
        if (sEffectTarget)
            SDL_DestroyTexture(sEffectTarget);
        sMask = sEffectTarget = NULL;
    }
    free(px);
}

// Where the game picture is drawn: straight into `dst`, or into the effect
// target (returns it) when an effect is on.
static SDL_Texture* BeginEffect(const SDL_FRect* dst, SDL_FRect* gameDst)
{
    *gameDst = *dst;
    if (sConfig.filter == PORT_FILTER_NONE)
        return NULL;
    const int k = EffectScale(dst);
    UpdateMask(k);
    if (!sEffectTarget || !SDL_SetRenderTarget(sRenderer, sEffectTarget))
        return NULL;
    *gameDst = (SDL_FRect){ 0, 0, (float)sMaskW, (float)sMaskH };
    return sEffectTarget;
}

static void EndEffect(SDL_Texture* target, const SDL_FRect* dst)
{
    if (!target)
        return;
    SDL_RenderTexture(sRenderer, sMask, NULL, NULL);
    SDL_SetRenderTarget(sRenderer, NULL);
    const bool exact = (int)dst->w == sMaskW && (int)dst->h == sMaskH;
    SDL_SetTextureScaleMode(target, exact ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    SDL_RenderTexture(sRenderer, target, NULL, dst);
}

static void DrawToast(void)
{
    int w, h;
    if (sToast[0] == '\0' || SDL_GetTicksNS() > sToastUntilNs)
        return;
    OutputSize(&w, &h);
    const float scale = SDL_max(1.0f, SDL_floorf(h / 240.0f));
    const float tw = (float)SDL_strlen(sToast) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale;
    SDL_SetRenderDrawBlendMode(sRenderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sRenderer, 0, 0, 0, 170);
    SDL_RenderFillRect(sRenderer, &(SDL_FRect){ 8 * scale, 8 * scale, tw + 8 * scale, 16 * scale });
    SDL_SetRenderDrawColor(sRenderer, 255, 255, 255, 255);
    SDL_SetRenderScale(sRenderer, scale, scale);
    SDL_RenderDebugText(sRenderer, 12, 12, sToast);
    SDL_SetRenderScale(sRenderer, 1.0f, 1.0f);
}

// Game screen (+ effect), touch pad and messages. The caller presents.
static void DrawScene(bool touchPad)
{
    const SDL_FRect dst = GameRect();

    SDL_SetRenderDrawColor(sRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sRenderer);
    SDL_FRect game;
    SDL_Texture* effect = BeginEffect(&dst, &game);
    SDL_RenderTexture(sRenderer, sTexture, NULL, &game);
    EndEffect(effect, &dst);
    if (touchPad && sTouchVisible)
        DrawTouchPad(TouchKeys(), -2);
    DrawToast();
}

// ---- save states -----------------------------------------------------------------

static void StatePath(int slot, char* out, size_t size)
{
    SDL_snprintf(out, size, "%sstate%s_%d.mzs", sPrefPath ? sPrefPath : "", sRegionSuffix, slot);
}

// File: the port's state image followed by a 240x160 thumbnail (ABGR8888).
static bool SaveState(int slot, char* error, size_t errorSize)
{
    char path[1100];
    const size_t size = Port_StateSize();
    uint8_t* buf = malloc(size + THUMB_BYTES);

    if (!buf) {
        SDL_snprintf(error, errorSize, "out of memory");
        return false;
    }
    Port_StateSave(buf);
    memcpy(buf + size, sLastFrame, THUMB_BYTES);
    StatePath(slot, path, sizeof(path));
    const bool ok = SDL_SaveFile(path, buf, size + THUMB_BYTES);
    free(buf);
    if (!ok) {
        SDL_snprintf(error, errorSize, "%s", SDL_GetError());
        return false;
    }
    sThumbsValid[slot] = false;
    sStateSlot = slot;
    return true;
}

static bool LoadState(int slot, char* error, size_t errorSize)
{
    char path[1100];
    size_t len = 0;
    StatePath(slot, path, sizeof(path));
    void* buf = SDL_LoadFile(path, &len);
    if (!buf) {
        SDL_snprintf(error, errorSize, "ranura vacia");
        return false;
    }
    const size_t size = Port_StateSize();
    const PortStateResult r = Port_StateLoad(buf, len < size ? len : size);
    if (r == PORT_STATE_OK && len >= size + THUMB_BYTES) {
        // Show the saved picture until the game draws the next frame.
        memcpy(sLastFrame, (uint8_t*)buf + size, THUMB_BYTES);
        SDL_UpdateTexture(sTexture, NULL, sLastFrame, GBA_SCREEN_WIDTH * 4);
    }
    SDL_free(buf);
    if (r != PORT_STATE_OK) {
        Port_Log("state %s: %s", path, Port_StateResultText(r));
        SDL_snprintf(error, errorSize, "%s",
                     r == PORT_STATE_OTHER_BUILD ? "es de otra version del juego" : "archivo no valido");
        return false;
    }
    sStateSlot = slot;
    if (sAudio)
        SDL_ClearAudioStream(sAudio);
    return true;
}

static bool StateInfo(int slot, char* text, size_t textSize, SDL_Texture** thumbnail)
{
    char path[1100];
    SDL_PathInfo info;
    SDL_DateTime dt;

    *thumbnail = NULL;
    StatePath(slot, path, sizeof(path));
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) {
        sThumbsValid[slot] = false;
        return false;
    }
    if (SDL_TimeToDateTime(info.modify_time, &dt, true))
        SDL_snprintf(text, textSize, "%02d/%02d/%04d %02d:%02d", dt.day, dt.month, dt.year, dt.hour, dt.minute);
    else
        SDL_snprintf(text, textSize, "?");

    if (!sThumbsValid[slot]) {
        const size_t size = Port_StateSize();
        size_t len = 0;
        void* buf = SDL_LoadFile(path, &len);
        if (buf && len >= size + THUMB_BYTES) {
            if (!sThumbs[slot])
                sThumbs[slot] = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                                  GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);
            if (sThumbs[slot]) {
                SDL_UpdateTexture(sThumbs[slot], NULL, (uint8_t*)buf + size, GBA_SCREEN_WIDTH * 4);
                SDL_SetTextureScaleMode(sThumbs[slot], SDL_SCALEMODE_LINEAR);
            }
        }
        SDL_free(buf);
        sThumbsValid[slot] = true;
    }
    *thumbnail = sThumbs[slot];
    return true;
}

// ---- menu -----------------------------------------------------------------------

static void MenuQuit(void)
{
    Port_RequestQuit();
}

static void MenuToast(const char* text)
{
    Toast("%s", text);
}

static void MenuApplyConfig(void)
{
    if (!sAudio && sConfig.volume > 0) {
        OpenAudio();
        if (sAudio)
            SDL_PauseAudioStreamDevice(sAudio); // resumed when the menu closes
    }
    ApplyVideoConfig();
    if (sConfig.touchControls == PORT_TOUCH_ON)
        sTouchVisible = true;
    else if (sConfig.touchControls == PORT_TOUCH_OFF)
        sTouchVisible = false;
#ifndef __ANDROID__
    SDL_SetWindowFullscreen(sWindow, sConfig.fullscreen);
#endif
}

static void InitMenu(void)
{
    const PortMenuHost host = {
        .window = sWindow,
        .renderer = sRenderer,
        .config = &sConfig,
        .applyConfig = MenuApplyConfig,
        .saveState = SaveState,
        .loadState = LoadState,
        .stateInfo = StateInfo,
        .touchButtonCircle = TouchButtonCircle,
        .moveTouchButton = MoveTouchButton,
        .drawTouchPad = MenuDrawTouchPad,
        .toast = MenuToast,
        .quit = MenuQuit,
    };
    sMenuReady = PortMenu_Init(&host);
    if (!sMenuReady)
        Port_Log("options menu unavailable");
}

// Runs the menu until it closes; the game is paused meanwhile.
static void RunMenu(void)
{
    if (sAudio) {
        SDL_PauseAudioStreamDevice(sAudio);
        SDL_ClearAudioStream(sAudio);
    }
    PortMenu_Open();
    while (PortMenu_IsOpen() && !Port_QuitRequested()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                Port_RequestQuit();
                break;
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !sGamepad)
                sGamepad = SDL_OpenGamepad(e.gdevice.which);
            PortMenu_HandleEvent(&e);
        }
        DrawScene(false);
        PortMenu_Render();
        SDL_RenderPresent(sRenderer);
        if (!sConfig.vsync)
            SDL_Delay(16);
    }
    PortConfig_Save(&sConfig, sConfigPath);
    ApplyVideoConfig();
    if (sAudio)
        SDL_ResumeAudioStreamDevice(sAudio);
    for (int i = 0; i < MAX_FINGERS; i++)
        sFingers[i].down = false;
    sInputLatch = true;
    sFastForwardHeld = false;
    sNextFrameNs = SDL_GetTicksNS();
}

void Port_HostFrameBoundary(void)
{
    if (sPendingState != 0) {
        char error[128];
        const int slot = sPendingState > 0 ? sPendingState : -sPendingState;
        const bool load = sPendingState < 0;
        sPendingState = 0;
        if (load ? LoadState(slot, error, sizeof(error)) : SaveState(slot, error, sizeof(error)))
            Toast(load ? "Estado %d cargado" : "Estado %d guardado", slot);
        else
            Toast("Estado %d: %s", slot, error);
    }
    if (sMenuRequested) {
        sMenuRequested = false;
        if (sMenuReady)
            RunMenu();
    }
}

// ---- host interface -------------------------------------------------------------

bool Port_HostInit(const PortHostConfig* cfg)
{
    if (cfg->argc > 1)
        sRomArgPath = cfg->argv[1];
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        Port_Log("SDL_Init: %s", SDL_GetError());
        return false;
    }

    sPrefPath = SDL_GetPrefPath("mf-android", "mf");
    // Each region keeps its own save and ROM copy: on PC all builds share this
    // folder. The US build keeps the original names.
    const char* suffix = SDL_strcmp(Port_RomRegionName(), "US") == 0 ? "" : Port_RomRegionName();
    SDL_snprintf(sRegionSuffix, sizeof(sRegionSuffix), "%s%s", suffix[0] ? "_" : "", suffix);
    for (char* c = sRegionSuffix; *c; c++)
        *c = (char)SDL_tolower(*c);
    SDL_snprintf(sSavePath, sizeof(sSavePath), "%smf%s.sav", sPrefPath ? sPrefPath : "", sRegionSuffix);
    SDL_snprintf(sConfigPath, sizeof(sConfigPath), "%ssettings.ini", sPrefPath ? sPrefPath : "");
    PortConfig_Defaults(&sConfig);
    if (!PortConfig_Load(&sConfig, sConfigPath))
        PortConfig_Save(&sConfig, sConfigPath); // first run: write the defaults
    Port_Log("settings: %s", sConfigPath);

    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                  (sConfig.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    if (!SDL_CreateWindowAndRenderer("Metroid Fusion", GBA_SCREEN_WIDTH * sConfig.windowScale,
                                     GBA_SCREEN_HEIGHT * sConfig.windowScale, flags, &sWindow, &sRenderer)) {
        Port_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return false;
    }
    sTexture = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                 GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);
    if (!sTexture) {
        Port_Log("SDL_CreateTexture: %s", SDL_GetError());
        return false;
    }

    if (sConfig.volume > 0)
        OpenAudio();
    ApplyVideoConfig();

#ifdef __ANDROID__
    sTouchVisible = sConfig.touchControls != PORT_TOUCH_OFF;
#else
    sTouchVisible = sConfig.touchControls == PORT_TOUCH_ON;
#endif
    InitMenu();
    sNextFrameNs = SDL_GetTicksNS();
    return true;
}

void Port_HostShutdown(void)
{
    if (sMenuReady)
        PortMenu_Shutdown();
    for (int i = 0; i <= PORT_STATE_SLOTS; i++) {
        if (sThumbs[i])
            SDL_DestroyTexture(sThumbs[i]);
    }
    if (sMask)
        SDL_DestroyTexture(sMask);
    if (sEffectTarget)
        SDL_DestroyTexture(sEffectTarget);
    if (sAudio)
        SDL_DestroyAudioStream(sAudio);
    if (sGamepad)
        SDL_CloseGamepad(sGamepad);
    SDL_DestroyTexture(sTexture);
    SDL_DestroyRenderer(sRenderer);
    SDL_DestroyWindow(sWindow);
    SDL_free(sPrefPath);
    SDL_Quit();
}

static void OpenFirstGamepad(void)
{
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids && count > 0 && !sGamepad) {
        sGamepad = SDL_OpenGamepad(ids[0]);
        if (sGamepad)
            Port_Log("gamepad: %s", SDL_GetGamepadName(sGamepad));
    }
    SDL_free(ids);
}

static void HandleFinger(const SDL_TouchFingerEvent* e, bool down, bool pressed)
{
    int w, h;
    int slot = -1;
    OutputSize(&w, &h);
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (sFingers[i].down && sFingers[i].id == e->fingerID) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (!down)
            return;
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (!sFingers[i].down) {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return;
    }
    sFingers[slot].id = e->fingerID;
    sFingers[slot].down = down;
    sFingers[slot].x = e->x * (float)w;
    sFingers[slot].y = e->y * (float)h;
    if (pressed && sTouchVisible && TouchHit(PORT_TOUCH_MENU, sFingers[slot].x, sFingers[slot].y))
        sMenuRequested = true;
    if (sConfig.touchControls == PORT_TOUCH_AUTO)
        sTouchVisible = true;
}

static bool IsFastForwardButton(SDL_GamepadButton button)
{
    return button != SDL_GAMEPAD_BUTTON_INVALID && button == sConfig.bindings[PORT_INPUT_FAST_FORWARD].button;
}

static bool PumpEvents(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            return false;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat)
                break;
            if (e.key.key == SDLK_F11) {
                sConfig.fullscreen = !sConfig.fullscreen;
                SDL_SetWindowFullscreen(sWindow, sConfig.fullscreen);
                PortConfig_Save(&sConfig, sConfigPath);
            } else if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_F1 || e.key.key == SDLK_AC_BACK) {
                sMenuRequested = true;
            } else if (e.key.key == SDLK_F5) {
                sPendingState = sStateSlot;
            } else if (e.key.key == SDLK_F8) {
                sPendingState = -sStateSlot;
            } else if (sConfig.fastForwardToggle && e.key.scancode == sConfig.bindings[PORT_INPUT_FAST_FORWARD].key) {
                sFastForwardOn = !sFastForwardOn;
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            OpenFirstGamepad();
            if (sConfig.touchControls == PORT_TOUCH_AUTO)
                sTouchVisible = false;
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (sGamepad && SDL_GetGamepadID(sGamepad) == e.gdevice.which) {
                SDL_CloseGamepad(sGamepad);
                sGamepad = NULL;
                OpenFirstGamepad();
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            SDL_Gamepad* pad = SDL_GetGamepadFromID(e.gbutton.which);
            if (sConfig.touchControls == PORT_TOUCH_AUTO)
                sTouchVisible = false;
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)
                sMenuRequested = true;
            // Select+Start (not A+B+Select+Start: that is the game's soft reset).
            if (pad && (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK || e.gbutton.button == SDL_GAMEPAD_BUTTON_START) &&
                SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START) &&
                !SDL_GetGamepadButton(pad, sConfig.bindings[PORT_INPUT_A].button) &&
                !SDL_GetGamepadButton(pad, sConfig.bindings[PORT_INPUT_B].button))
                sMenuRequested = true;
            if (sConfig.fastForwardToggle && IsFastForwardButton((SDL_GamepadButton)e.gbutton.button))
                sFastForwardOn = !sFastForwardOn;
            break;
        }
        case SDL_EVENT_FINGER_DOWN:
            HandleFinger(&e.tfinger, true, true);
            break;
        case SDL_EVENT_FINGER_MOTION:
            HandleFinger(&e.tfinger, true, false);
            break;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            HandleFinger(&e.tfinger, false, false);
            break;
        default:
            break;
        }
    }
    return true;
}

bool Port_HostPresent(const uint32_t* abgr, int width, int height)
{
    if (!PumpEvents())
        return false;

    memcpy(sLastFrame, abgr, sizeof(sLastFrame));
    SDL_UpdateTexture(sTexture, NULL, abgr, width * (int)sizeof(uint32_t));
    (void)height;
    DrawScene(true);
    SDL_RenderPresent(sRenderer);
    return true;
}

uint16_t Port_HostPollKeys(void)
{
    const bool* kb = SDL_GetKeyboardState(NULL);
    uint16_t keys = 0;

    for (int k = 0; k < PORT_INPUT_GBA_COUNT; k++) {
        const PortBinding* b = &sConfig.bindings[k];
        bool down = false;
        if (b->key != SDL_SCANCODE_UNKNOWN && kb[b->key])
            down = true;
        if (sGamepad && b->button != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(sGamepad, b->button))
            down = true;
        if (sGamepad && b->axis != SDL_GAMEPAD_AXIS_INVALID) {
            const int v = SDL_GetGamepadAxis(sGamepad, b->axis);
            const int threshold = 32767 * sConfig.stickDeadzone / 100;
            if ((b->axisSign > 0 && v > threshold) || (b->axisSign < 0 && v < -threshold))
                down = true;
        }
        if (down)
            keys |= (uint16_t)(1u << k);
    }

    {
        const PortBinding* ff = &sConfig.bindings[PORT_INPUT_FAST_FORWARD];
        sFastForwardHeld = (ff->key != SDL_SCANCODE_UNKNOWN && kb[ff->key]) ||
                           (sGamepad && ff->button != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(sGamepad, ff->button));
    }

    keys |= TouchKeys();

    // After the menu closes, the buttons that closed it may still be held:
    // the game only sees input again once everything was released.
    if (sInputLatch) {
        if (keys != 0)
            return 0;
        sInputLatch = false;
    }

    // Opposite directions can't be held on a real d-pad; the game misbehaves.
    if ((keys & (PORT_KEY_LEFT | PORT_KEY_RIGHT)) == (PORT_KEY_LEFT | PORT_KEY_RIGHT))
        keys &= ~(PORT_KEY_LEFT | PORT_KEY_RIGHT);
    if ((keys & (PORT_KEY_UP | PORT_KEY_DOWN)) == (PORT_KEY_UP | PORT_KEY_DOWN))
        keys &= ~(PORT_KEY_UP | PORT_KEY_DOWN);
    return keys;
}

void Port_HostQueueAudio(const int16_t* samples, int frames)
{
    // At most ~100 ms queued: in fast-forward (or if the device falls behind)
    // the extra sound is dropped instead of building up a delay.
    const int maxQueued = PORT_AUDIO_RATE / 10 * 4;

    if (!sAudio || frames <= 0 || SDL_GetAudioStreamQueued(sAudio) > maxQueued)
        return;
    SDL_PutAudioStreamData(sAudio, samples, frames * 4);
}

void Port_HostWaitFrame(void)
{
    const bool fast = sConfig.fastForwardToggle ? sFastForwardOn : sFastForwardHeld;
    const uint64_t frameNs = fast ? GBA_FRAME_NS / (uint64_t)sConfig.fastForwardSpeed : GBA_FRAME_NS;
    const uint64_t now = SDL_GetTicksNS();

    sNextFrameNs += frameNs;
    if (sNextFrameNs > now) {
        SDL_DelayPrecise(sNextFrameNs - now);
    } else if (now - sNextFrameNs > 4 * GBA_FRAME_NS) {
        sNextFrameNs = now; // fell far behind (debugger, app suspended): resync
    }
}

// ---- ROM selection ----------------------------------------------------------------

static char sRomCachePath[1024];
static SDL_AtomicInt sDialogState; // 0 idle, 1 open, 2 done
static char sPickedPath[2048];

static void SDLCALL OnRomPicked(void* userdata, const char* const* files, int filter)
{
    (void)userdata;
    (void)filter;
    sPickedPath[0] = '\0';
    if (files && files[0])
        SDL_strlcpy(sPickedPath, files[0], sizeof(sPickedPath));
    SDL_SetAtomicInt(&sDialogState, 2);
}

static uint8_t* LoadWholeFile(const char* path, size_t* size)
{
    size_t len = 0;
    void* data = SDL_LoadFile(path, &len);
    if (!data)
        return NULL;
    // Hand back a malloc'd buffer (the port frees it with free()).
    uint8_t* copy = malloc(len ? len : 1);
    if (copy)
        memcpy(copy, data, len);
    SDL_free(data);
    *size = len;
    return copy;
}

static void DrawRomScreen(const char* error)
{
    float scale;
    int w, h;

    SDL_GetWindowSizeInPixels(sWindow, &w, &h);
    scale = (float)h / 200.0f;
    if (scale < 1.0f)
        scale = 1.0f;
    SDL_SetRenderLogicalPresentation(sRenderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderScale(sRenderer, scale, scale);
    SDL_SetRenderDrawColor(sRenderer, 8, 8, 24, 255);
    SDL_RenderClear(sRenderer);

    SDL_SetRenderDrawColor(sRenderer, 255, 255, 255, 255);
    SDL_RenderDebugText(sRenderer, 8, 8, "Metroid Fusion");
    SDL_SetRenderDrawColor(sRenderer, 180, 180, 200, 255);
    SDL_RenderDebugText(sRenderer, 8, 28, "Choose your own ROM dump:");
    SDL_RenderDebugTextFormat(sRenderer, 8, 40, "Metroid Fusion (%s)", Port_RomRegionName());
    SDL_RenderDebugText(sRenderer, 8, 60, "Press A / Enter or tap to browse.");
    SDL_RenderDebugText(sRenderer, 8, 72, "Press B / Esc to quit.");
    if (error) {
        SDL_SetRenderDrawColor(sRenderer, 255, 120, 120, 255);
        SDL_RenderDebugText(sRenderer, 8, 96, error);
    }
    SDL_SetRenderDrawColor(sRenderer, 120, 120, 140, 255);
    SDL_RenderDebugText(sRenderer, 8, 120, "SHA-1:");
    SDL_RenderDebugText(sRenderer, 8, 132, Port_RomExpectedSha1());

    SDL_SetRenderScale(sRenderer, 1.0f, 1.0f);
    SDL_RenderPresent(sRenderer);
    ApplyVideoConfig();
}

uint8_t* Port_HostLoadRom(size_t* size, const char* error)
{
    static const SDL_DialogFileFilter filters[] = {
        { "GBA ROM", "gba;bin;agb" },
        { "All files", "*" },
    };
    const char* candidates[4];
    char nextToExe[1024];
    int n = 0;

    SDL_snprintf(sRomCachePath, sizeof(sRomCachePath), "%srom%s.gba", sPrefPath ? sPrefPath : "", sRegionSuffix);
    {
        // Upstream's file name for the dump: mf_us.gba, next to the exe.
        char region[8];
        SDL_strlcpy(region, Port_RomRegionName(), sizeof(region));
        for (char* c = region; *c; c++)
            *c = (char)SDL_tolower(*c);
        SDL_snprintf(nextToExe, sizeof(nextToExe), "%smf_%s.gba",
                     SDL_GetBasePath() ? SDL_GetBasePath() : "", region);
    }

    if (!error) {
        if (sRomArgPath)
            candidates[n++] = sRomArgPath;
        candidates[n++] = sRomCachePath;
        candidates[n++] = nextToExe;
        for (int i = 0; i < n; i++) {
            uint8_t* data = LoadWholeFile(candidates[i], size);
            if (data) {
                Port_Log("ROM: %s", candidates[i]);
                return data;
            }
        }
    }

    // Nothing found (or it was rejected): ask the player.
    for (;;) {
        SDL_Event e;
        bool browse = false;

        DrawRomScreen(error);
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT)
                return NULL;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED)
                OpenFirstGamepad();
            if (SDL_GetAtomicInt(&sDialogState) != 0)
                continue;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE)
                    browse = true;
                else if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_AC_BACK)
                    return NULL;
            } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (e.gbutton.button == sConfig.bindings[PORT_INPUT_A].button ||
                    e.gbutton.button == SDL_GAMEPAD_BUTTON_START)
                    browse = true;
                else if (e.gbutton.button == sConfig.bindings[PORT_INPUT_B].button)
                    return NULL;
            } else if (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                browse = true;
            }
        }

        if (browse) {
            SDL_SetAtomicInt(&sDialogState, 1);
            SDL_ShowOpenFileDialog(OnRomPicked, NULL, sWindow, filters, SDL_arraysize(filters), NULL, false);
        }

        if (SDL_GetAtomicInt(&sDialogState) == 2) {
            SDL_SetAtomicInt(&sDialogState, 0);
            if (sPickedPath[0] != '\0') {
                uint8_t* data = LoadWholeFile(sPickedPath, size);
                if (data) {
                    Port_Log("ROM picked: %s", sPickedPath);
                    return data;
                }
                error = "Can't read that file";
            }
        }
        SDL_Delay(16);
    }
}

void Port_HostRomAccepted(const uint8_t* rom, size_t size)
{
    // Keep a private copy so the player only picks the file once (on Android
    // the picked content:// URI is not readable after a restart).
    size_t len = 0;
    void* existing = SDL_LoadFile(sRomCachePath, &len);
    const bool same = existing && len == size && memcmp(existing, rom, size) == 0;
    SDL_free(existing);
    if (!same && !SDL_SaveFile(sRomCachePath, rom, size))
        Port_Log("can't cache the ROM in %s: %s", sRomCachePath, SDL_GetError());
}

void Port_HostLoadSave(uint8_t* data, size_t size)
{
    size_t len = 0;
    void* buf = SDL_LoadFile(sSavePath, &len);
    if (!buf)
        return;
    memcpy(data, buf, len < size ? len : size);
    SDL_free(buf);
    Port_Log("save loaded: %s", sSavePath);
}

void Port_HostWriteSave(const uint8_t* data, size_t size)
{
    if (!SDL_SaveFile(sSavePath, data, size))
        Port_Log("can't write %s: %s", sSavePath, SDL_GetError());
}

int main(int argc, char** argv)
{
    return Port_Main(argc, argv);
}

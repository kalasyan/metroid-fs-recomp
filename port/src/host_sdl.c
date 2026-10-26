// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — SDL3 frontend (desktop and Android).
//
// Video: the 240x160 frame (wider with the widescreen option) is uploaded to
// a streaming texture and scaled to the window (aspect-correct, optional
// integer scaling / smooth filter / LCD or CRT effect).
// Audio: the port's 48 kHz output goes to an SDL audio stream.
// Input: keyboard, gamepads (the AYN Thor's built-in controls show up as one)
// and, on touch screens, an on-screen pad drawn over the game.
// Menu: Esc / F1, Select+Start, the Android back button or the touch menu
// button open the options menu (port/src/menu.cpp) with save states.
// Rumble: gamepad motors and the device's vibrator, from game events.
// Second screen: area map or Samus' status on a second display / window.
// Pacing: by default one game frame every 1/59.73 s by the clock. Optionally
// the game is locked to the display's refresh rate (60/120 Hz...) with the
// sound resampled to match, or frames are blended on 90/144 Hz screens
// (port_pacing.h). Neither changes the game's logic.
// Everything configurable lives in settings.ini in the app's pref folder.
#include "port.h"
#include "port_config.h"
#include "port_assets.h"
#include "port_menu.h"
#include "port_pacing.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>

#define GBA_FRAME_NS PORT_GBA_FRAME_NS // 280896 cycles / 16.777216 MHz
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
static uint32_t sLastFrame[PORT_MAX_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];
static int sFrameW = GBA_SCREEN_WIDTH; // width of sTexture / sLastFrame

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

// Display pacing (port_pacing.h).
static double sRefreshHz;         // current display, 0 if unknown
static uint64_t sRefreshNs;       // one refresh, 0 if unknown
static int sSyncDivisor;          // > 0: locked, one game frame every N refreshes
static double sSyncSpeed = 1.0;   // game speed while locked (1.0046 at 60 Hz)
static uint64_t sSyncFrameNs;     // game frame while locked
static int sAudioChunk;           // sound device buffer, in 48 kHz sample frames
static double sAudioQueued;       // smoothed queue level, in sample frames
static double sAudioRatio = 1.0;  // playback speed given to the stream
static SDL_Texture* sPrevTexture; // previous game frame, for frame blending
static bool sPrevValid;
static bool sBlendActive;         // frames are being blended right now
static uint64_t sFrameShownNs;    // when the newest game frame was first presented
static float sVsyncScore = 1.0f;  // ~fraction of recent presents that waited for VSync
static double sMeasuredHz;        // refresh rate seen while locked, if not the reported one
static uint64_t sLastPresentNs;
static uint32_t sIntervals[64];   // recent present-to-present times while locked
static int sIntervalCount;
static bool sSyncLog;             // MF_SYNC_LOG: print pacing stats
static uint64_t sSyncLogNs, sSyncLogFrames;
static int sAudioDry, sAudioDropped; // for MF_SYNC_LOG

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
    float scale = SDL_min((float)w / sFrameW, (float)h / GBA_SCREEN_HEIGHT);
    if (sConfig.integerScaling && scale >= 1.0f)
        scale = SDL_floorf(scale);
    const float gw = SDL_floorf(sFrameW * scale), gh = SDL_floorf(GBA_SCREEN_HEIGHT * scale);
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
    if (sAudio) {
        SDL_AudioSpec deviceSpec;
        int frames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(sAudio), &deviceSpec, &frames) && frames > 0)
            sAudioChunk = (int)((double)frames * PORT_AUDIO_RATE / SDL_max(deviceSpec.freq, 1));
        SDL_SetAudioStreamFrequencyRatio(sAudio, (float)sAudioRatio);
        SDL_ResumeAudioStreamDevice(sAudio);
    } else {
        Port_Log("no sound: %s", SDL_GetError()); // the game still runs
    }
}

// ---- pacing -------------------------------------------------------------------

// Locking to the display and blending only make sense if presenting waits
// for the refresh, so both turn VSync on.
static bool VsyncWanted(void)
{
    return sConfig.vsync || sConfig.syncToDisplay || sConfig.frameBlend;
}

static bool FastForwarding(void)
{
    return sConfig.fastForwardToggle ? sFastForwardOn : sFastForwardHeld;
}

static void SetAudioRatio(double ratio)
{
    if (ratio == sAudioRatio)
        return;
    sAudioRatio = ratio;
    if (sAudio)
        SDL_SetAudioStreamFrequencyRatio(sAudio, (float)ratio);
}

// Reads the display's refresh rate and decides how to pace the game.
static void UpdateDisplaySync(void)
{
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(sWindow));
    const char* forced = SDL_getenv("MF_REFRESH_HZ"); // testing: pretend another refresh rate
    double hz = 0;
    int divisor = 0;
    double speed = 1.0;

    if (sMeasuredHz > 0)
        hz = sMeasuredHz;
    else if (forced)
        hz = SDL_atof(forced);
    else if (mode && mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
        hz = (double)mode->refresh_rate_numerator / mode->refresh_rate_denominator;
    else if (mode)
        hz = mode->refresh_rate;

    const bool lockable = PortPacing_LockToDisplay(hz, &divisor, &speed);
    const int oldDivisor = sSyncDivisor;
    const double oldHz = sRefreshHz;
    sRefreshHz = hz > 0 ? hz : 0;
    sRefreshNs = hz > 0 ? (uint64_t)(1e9 / hz) : 0;
    sSyncDivisor = sConfig.syncToDisplay && lockable ? divisor : 0;
    sSyncSpeed = sSyncDivisor ? speed : 1.0;
    sSyncFrameNs = sSyncDivisor ? (uint64_t)(1e9 * divisor / hz) : GBA_FRAME_NS;
    if (!sSyncDivisor)
        SetAudioRatio(1.0);
    sVsyncScore = 1.0f;
    sLastPresentNs = 0;
    sIntervalCount = 0;
    if (oldDivisor != sSyncDivisor || oldHz != sRefreshHz) {
        if (sSyncDivisor)
            Port_Log("display %.2f Hz: game locked to it (1 frame every %d refreshes, speed %+.2f %%)", hz, divisor,
                     (speed - 1.0) * 100.0);
        else
            Port_Log("display %.2f Hz: game paced by the GBA clock (59.73 Hz)", hz);
    }
}

// For the menu: what the pacing options do on this screen (ASCII).
static void SyncStatus(char* text, size_t size)
{
    int divisor;
    double speed;

    if (sRefreshHz <= 0)
        SDL_snprintf(text, size, "Frecuencia de la pantalla desconocida: se usa el reloj de la GBA (59,73 Hz).");
    else if (sSyncDivisor)
        SDL_snprintf(text, size, "Pantalla a %.2f Hz: el juego va a %.2f fps (%+.2f %%).", sRefreshHz,
                     sRefreshHz / sSyncDivisor, (sSyncSpeed - 1.0) * 100.0);
    else if (PortPacing_LockToDisplay(sRefreshHz, &divisor, &speed))
        SDL_snprintf(text, size, "Pantalla a %.2f Hz: el juego va a 59,73 fps (reloj de la GBA).", sRefreshHz);
    else
        SDL_snprintf(text, size, "Pantalla a %.2f Hz: no es multiplo de 60 Hz, se usa el reloj de la GBA.",
                     sRefreshHz);
    for (char* c = text; *c; c++) { // decimal comma
        if (*c == '.' && c > text && SDL_isdigit((unsigned char)c[-1]) && SDL_isdigit((unsigned char)c[1]))
            *c = ',';
    }
}

// Sound queue to keep while locked: two device buffers and a frame, at least
// 40 ms (well under the 100 ms where new sound is dropped).
static double AudioTarget(void)
{
    const double frame = PORT_AUDIO_RATE / PORT_GBA_FRAME_RATE;
    return SDL_min(PORT_AUDIO_RATE * 0.07, SDL_max(PORT_AUDIO_RATE * 0.04, 2.0 * sAudioChunk + frame));
}

// Picture width for the widescreen setting ("auto" follows the window).
static int WantedFrameWidth(void)
{
    int w = 0, h = 0;
    if (sConfig.widescreen == PORT_WIDE_AUTO)
        OutputSize(&w, &h);
    return PortConfig_WideWidth(sConfig.widescreen, h > 0 ? (float)w / h : 1.5f);
}

// (Re)creates the game texture for a picture `width` pixels wide.
static bool SetFrameWidth(int width)
{
    if (sTexture && width == sFrameW)
        return true;
    SDL_Texture* t = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, width,
                                       GBA_SCREEN_HEIGHT);
    if (!t) {
        Port_Log("SDL_CreateTexture: %s", SDL_GetError());
        return false;
    }
    if (sTexture)
        SDL_DestroyTexture(sTexture);
    sTexture = t;
    SDL_SetTextureScaleMode(sTexture, sConfig.smoothScaling ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    // Until the next frame: the last one, centered (or cropped).
    static uint32_t old[PORT_MAX_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];
    const int oldW = sFrameW;
    memcpy(old, sLastFrame, sizeof(old));
    for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < width; x++) {
            const int ox = x - (width - oldW) / 2;
            sLastFrame[y * width + x] = (ox >= 0 && ox < oldW) ? old[y * oldW + ox] : 0xFF000000u;
        }
    }
    sFrameW = width;
    SDL_UpdateTexture(sTexture, NULL, sLastFrame, sFrameW * 4);
    // Frame blending's previous frame must have the same size.
    if (sPrevTexture) {
        SDL_DestroyTexture(sPrevTexture);
        sPrevTexture =
            SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, width, GBA_SCREEN_HEIGHT);
        if (sPrevTexture)
            SDL_SetTextureScaleMode(sPrevTexture, sConfig.smoothScaling ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        sPrevValid = false;
    }
    return true;
}

static void ApplyVideoConfig(void)
{
    const SDL_ScaleMode scale = sConfig.smoothScaling ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
    Port_WideSetWidth(WantedFrameWidth());
    SetFrameWidth(Port_WideWidth());
    SDL_SetRenderVSync(sRenderer, VsyncWanted() ? 1 : 0);
    SDL_SetTextureScaleMode(sTexture, scale);
    if (sConfig.frameBlend && !sPrevTexture) {
        sPrevTexture = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                         sFrameW, GBA_SCREEN_HEIGHT);
        sPrevValid = false;
    }
    if (sPrevTexture)
        SDL_SetTextureScaleMode(sPrevTexture, scale);
    UpdateDisplaySync();
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
    const int w = sFrameW * k, h = GBA_SCREEN_HEIGHT * k;
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
// `blend` < 1 mixes in the previous game frame (frame blending).
static void DrawScene(bool touchPad, float blend)
{
    const SDL_FRect dst = GameRect();

    SDL_SetRenderDrawColor(sRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sRenderer);
    SDL_FRect game;
    SDL_Texture* effect = BeginEffect(&dst, &game);
    if (blend < 1.0f && sPrevTexture && sPrevValid) {
        SDL_RenderTexture(sRenderer, sPrevTexture, NULL, &game);
        SDL_SetTextureAlphaModFloat(sTexture, blend);
        SDL_RenderTexture(sRenderer, sTexture, NULL, &game);
        SDL_SetTextureAlphaModFloat(sTexture, 1.0f);
    } else {
        SDL_RenderTexture(sRenderer, sTexture, NULL, &game);
    }
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

// File: the port's state image followed by a 240x160 thumbnail (ABGR8888;
// the middle of the picture when it is wider).
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
    for (int y = 0; y < GBA_SCREEN_HEIGHT; y++)
        memcpy(buf + size + (size_t)y * GBA_SCREEN_WIDTH * 4,
               sLastFrame + (size_t)y * sFrameW + (sFrameW - GBA_SCREEN_WIDTH) / 2, GBA_SCREEN_WIDTH * 4);
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
        const uint32_t* thumb = (const uint32_t*)((uint8_t*)buf + size);
        for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
            for (int x = 0; x < sFrameW; x++) {
                const int tx = x - (sFrameW - GBA_SCREEN_WIDTH) / 2;
                uint32_t p = 0xFF000000u;
                if (tx >= 0 && tx < GBA_SCREEN_WIDTH)
                    memcpy(&p, &thumb[y * GBA_SCREEN_WIDTH + tx], sizeof(p));
                sLastFrame[y * sFrameW + x] = p;
            }
        }
        SDL_UpdateTexture(sTexture, NULL, sLastFrame, sFrameW * 4);
    }
    sPrevValid = false; // don't blend with the frame from before the load
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

// ---- rumble ---------------------------------------------------------------------
// port/src/extras.c turns game events into motor levels every frame. Gamepads
// get them with SDL_RumbleGamepad; the device's own vibrator (the phone's, or
// the AYN Thor's) is an SDL haptic device that plays one-shot pulses.

static SDL_Haptic* sHaptic;
static bool sHapticTried;
static float sRumbleLow, sRumbleHigh; // last levels sent to the gamepad
static int sRumbleRefresh;

static void OpenDeviceHaptic(void)
{
    int count = 0;
    SDL_HapticID* ids;

    if (sHapticTried)
        return;
    sHapticTried = true;
    if (!SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
        Port_Log("rumble: no haptics: %s", SDL_GetError());
        return;
    }
    ids = SDL_GetHaptics(&count);
    // On Android the device vibrator is "VIBRATOR_SERVICE"; gamepads rumble
    // through SDL_RumbleGamepad instead.
    for (int pass = 0; pass < 2 && !sHaptic && ids; pass++) {
        for (int i = 0; i < count && !sHaptic; i++) {
            const char* name = SDL_GetHapticNameForID(ids[i]);
            if (pass == 0 && !(name && SDL_strstr(name, "VIBRATOR")))
                continue;
            sHaptic = SDL_OpenHaptic(ids[i]);
            if (sHaptic && !SDL_InitHapticRumble(sHaptic)) {
                SDL_CloseHaptic(sHaptic);
                sHaptic = NULL;
            }
            if (sHaptic)
                Port_Log("rumble: vibrator %s", name ? name : "?");
        }
    }
    SDL_free(ids);
}

static void StopRumble(void)
{
    if (sGamepad && (sRumbleLow > 0.0f || sRumbleHigh > 0.0f))
        SDL_RumbleGamepad(sGamepad, 0, 0, 0);
    sRumbleLow = sRumbleHigh = 0.0f;
    if (sHaptic)
        SDL_StopHapticRumble(sHaptic);
}

void Port_HostRumble(float low, float high, float pulse, int pulseMs)
{
    const float k = sConfig.rumbleStrength / 100.0f;

    if (sConfig.rumbleGamepad && sGamepad) {
        // Sent when it changes, and refreshed while it lasts.
        const bool changed = SDL_fabsf(low - sRumbleLow) > 0.02f || SDL_fabsf(high - sRumbleHigh) > 0.02f;
        const bool on = low > 0.0f || high > 0.0f;
        if (changed || (on && --sRumbleRefresh <= 0)) {
            SDL_RumbleGamepad(sGamepad, (Uint16)(SDL_min(low * k, 1.0f) * 0xFFFF),
                              (Uint16)(SDL_min(high * k, 1.0f) * 0xFFFF), on ? 150 : 0);
            sRumbleLow = low;
            sRumbleHigh = high;
            sRumbleRefresh = 5;
        }
    } else if (sRumbleLow > 0.0f || sRumbleHigh > 0.0f) {
        StopRumble();
    }

    if (sConfig.rumbleDevice && pulse > 0.0f && k > 0.0f) {
        OpenDeviceHaptic();
        if (sHaptic)
            SDL_PlayHapticRumble(sHaptic, SDL_min(pulse * k, 1.0f), (Uint32)pulseMs);
    }
}

// ---- second screen ------------------------------------------------------------
// The area map or Samus' status (port/src/second_screen.c) on a second display.
// PC: a second window (on the second monitor if there is one). Android: the
// Java side (MfActivity) shows it on the secondary display as a Presentation
// and pulls the pixels through JNI; on a device with one screen it does nothing.

#define SCREEN2_MAX_H 448
static uint32_t sScreen2Px[256 * SCREEN2_MAX_H];

#ifdef __ANDROID__
#include <jni.h>

static SDL_Mutex* sScreen2Lock;
static int sScreen2W, sScreen2H;    // canvas the Java side asked for
static uint64_t sScreen2Frame;      // frame drawn in sScreen2Px
static uint64_t sScreen2Copied;     // last frame handed to Java
static uint64_t sScreen2PolledNs;   // Java asked recently: keep drawing
static SDL_AtomicInt sScreen2Mode;  // sConfig.secondScreen, readable from the UI thread

static void Screen2Init(void)
{
    sScreen2Lock = SDL_CreateMutex();
    SDL_SetAtomicInt(&sScreen2Mode, sConfig.secondScreen);
}

static void Screen2Apply(void)
{
    SDL_SetAtomicInt(&sScreen2Mode, sConfig.secondScreen);
}

static void Screen2Present(void)
{
    if (!sScreen2Lock || sConfig.secondScreen == PORT_SCREEN2_OFF)
        return;
    SDL_LockMutex(sScreen2Lock);
    if (sScreen2W > 0 && SDL_GetTicksNS() - sScreen2PolledNs < 1000000000ull) {
        Port_SecondScreenRender(sScreen2Px, sScreen2W, sScreen2H, sConfig.secondScreen);
        sScreen2Frame = Port_FrameCount() + 1;
    }
    SDL_UnlockMutex(sScreen2Lock);
}

static void Screen2Shutdown(void)
{
    SDL_SetAtomicInt(&sScreen2Mode, PORT_SCREEN2_OFF);
}

// MfActivity (UI thread): PORT_SCREEN2_*, OFF until the game has started.
JNIEXPORT jint JNICALL Java_dev_mf_android_MfActivity_nativeSecondScreenMode(JNIEnv* env, jclass cls)
{
    (void)env;
    (void)cls;
    return sScreen2Lock ? SDL_GetAtomicInt(&sScreen2Mode) : PORT_SCREEN2_OFF;
}

// Called by MfActivity's second-screen view on the UI thread. Returns the
// canvas size for that display ((width << 16) | height), or 0 when the second
// screen is off.
JNIEXPORT jint JNICALL Java_dev_mf_android_MfActivity_nativeSecondScreenCanvas(JNIEnv* env, jclass cls, jint displayW,
                                                                              jint displayH)
{
    int w, h;
    (void)env;
    (void)cls;
    if (!sScreen2Lock || SDL_GetAtomicInt(&sScreen2Mode) == PORT_SCREEN2_OFF || displayW <= 0 || displayH <= 0)
        return 0;
    Port_SecondScreenSize(displayW, displayH, &w, &h);
    SDL_LockMutex(sScreen2Lock);
    if (w != sScreen2W || h != sScreen2H) {
        sScreen2W = w;
        sScreen2H = h;
        sScreen2Frame = sScreen2Copied = 0;
    }
    sScreen2PolledNs = SDL_GetTicksNS();
    SDL_UnlockMutex(sScreen2Lock);
    return (jint)((w << 16) | h);
}

// Copies the latest picture (ARGB, as android.graphics.Bitmap wants) into
// `pixels` (width * height of the canvas). Returns false if there is no new one.
JNIEXPORT jboolean JNICALL Java_dev_mf_android_MfActivity_nativeSecondScreenPixels(JNIEnv* env, jclass cls,
                                                                                  jintArray pixels)
{
    jboolean copied = JNI_FALSE;
    (void)cls;
    if (!sScreen2Lock)
        return JNI_FALSE;
    SDL_LockMutex(sScreen2Lock);
    const int count = sScreen2W * sScreen2H;
    if (sScreen2Frame != 0 && sScreen2Frame != sScreen2Copied && pixels &&
        (*env)->GetArrayLength(env, pixels) == count) {
        jint* dst = (*env)->GetIntArrayElements(env, pixels, NULL);
        if (dst) {
            for (int i = 0; i < count; i++) {
                const uint32_t p = sScreen2Px[i]; // 0xAABBGGRR
                dst[i] = (jint)((p & 0xFF00FF00u) | (p & 0xFFu) << 16 | (p >> 16 & 0xFFu));
            }
            (*env)->ReleaseIntArrayElements(env, pixels, dst, 0);
            sScreen2Copied = sScreen2Frame;
            copied = JNI_TRUE;
        }
    }
    SDL_UnlockMutex(sScreen2Lock);
    return copied;
}

#else // desktop: a second window

static SDL_Window* sScreen2Window;
static SDL_Renderer* sScreen2Renderer;
static SDL_Texture* sScreen2Texture;
static int sScreen2W, sScreen2H;

static void Screen2Close(void)
{
    if (sScreen2Texture)
        SDL_DestroyTexture(sScreen2Texture);
    if (sScreen2Renderer)
        SDL_DestroyRenderer(sScreen2Renderer);
    if (sScreen2Window)
        SDL_DestroyWindow(sScreen2Window);
    sScreen2Texture = NULL;
    sScreen2Renderer = NULL;
    sScreen2Window = NULL;
}

static void Screen2Open(void)
{
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    const SDL_DisplayID main = SDL_GetDisplayForWindow(sWindow);
    SDL_DisplayID target = 0;

    // Another monitor if there is one; next to the game otherwise.
    for (int i = 0; displays && i < count; i++) {
        if (displays[i] != main) {
            target = displays[i];
            break;
        }
    }
    SDL_free(displays);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Metroid Fusion - segunda pantalla");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 512);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 480);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    if (target) {
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(target));
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(target));
    } else {
        int x = 0, y = 0, w = 0;
        SDL_GetWindowPosition(sWindow, &x, &y);
        SDL_GetWindowSize(sWindow, &w, NULL);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x + w + 16);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    }
    sScreen2Window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (sScreen2Window)
        sScreen2Renderer = SDL_CreateRenderer(sScreen2Window, NULL);
    if (!sScreen2Renderer) {
        Port_Log("second window: %s", SDL_GetError());
        Screen2Close();
        return;
    }
    // The game's window sets the pace (vsync there, if any).
    SDL_SetRenderVSync(sScreen2Renderer, 0);
    SDL_RaiseWindow(sWindow);
}

static void Screen2Init(void)
{
}

static void Screen2Apply(void)
{
    if (sConfig.secondScreen != PORT_SCREEN2_OFF && !sScreen2Window)
        Screen2Open();
    else if (sConfig.secondScreen == PORT_SCREEN2_OFF && sScreen2Window)
        Screen2Close();
}

static void Screen2Present(void)
{
    int ww, wh, w, h;

    if (!sScreen2Renderer)
        return;
    if (!SDL_GetCurrentRenderOutputSize(sScreen2Renderer, &ww, &wh) || ww <= 0 || wh <= 0)
        return;
    Port_SecondScreenSize(ww, wh, &w, &h);
    if (!sScreen2Texture || w != sScreen2W || h != sScreen2H) {
        if (sScreen2Texture)
            SDL_DestroyTexture(sScreen2Texture);
        sScreen2Texture =
            SDL_CreateTexture(sScreen2Renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!sScreen2Texture)
            return;
        SDL_SetTextureScaleMode(sScreen2Texture, SDL_SCALEMODE_NEAREST);
        sScreen2W = w;
        sScreen2H = h;
    }
    Port_SecondScreenRender(sScreen2Px, w, h, sConfig.secondScreen);
    SDL_UpdateTexture(sScreen2Texture, NULL, sScreen2Px, w * 4);

    // Aspect-correct, whole pixels when it fits.
    float scale = SDL_min((float)ww / w, (float)wh / h);
    if (scale >= 1.0f)
        scale = SDL_floorf(scale);
    const SDL_FRect dst = { SDL_floorf((ww - w * scale) * 0.5f), SDL_floorf((wh - h * scale) * 0.5f), w * scale,
                            h * scale };
    SDL_SetRenderDrawColor(sScreen2Renderer, 0, 0, 0, 255);
    SDL_RenderClear(sScreen2Renderer);
    SDL_RenderTexture(sScreen2Renderer, sScreen2Texture, NULL, &dst);
    SDL_RenderPresent(sScreen2Renderer);
}

static void Screen2Shutdown(void)
{
    Screen2Close();
}

#endif // __ANDROID__

// Window events shared by the game loop and the menu loop. Returns false if
// the game should quit.
static bool HandleWindowEvent(const SDL_Event* e)
{
    if (e->type != SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        return true;
#ifndef __ANDROID__
    if (sScreen2Window && e->window.windowID == SDL_GetWindowID(sScreen2Window)) {
        // Closing the second window switches it off.
        sConfig.secondScreen = PORT_SCREEN2_OFF;
        Screen2Close();
        PortConfig_Save(&sConfig, sConfigPath);
        return true;
    }
#endif
    // With two windows SDL only sends SDL_EVENT_QUIT once both are closed.
    return e->window.windowID != SDL_GetWindowID(sWindow);
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
    Screen2Apply();
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
        .syncStatus = SyncStatus,
    };
    sMenuReady = PortMenu_Init(&host);
    if (!sMenuReady)
        Port_Log("options menu unavailable");
}

// Runs the menu until it closes; the game is paused meanwhile.
static void RunMenu(void)
{
    StopRumble();
    if (sAudio) {
        SDL_PauseAudioStreamDevice(sAudio);
        SDL_ClearAudioStream(sAudio);
    }
    PortMenu_Open();
    while (PortMenu_IsOpen() && !Port_QuitRequested()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || !HandleWindowEvent(&e)) {
                Port_RequestQuit();
                break;
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !sGamepad)
                sGamepad = SDL_OpenGamepad(e.gdevice.which);
            PortMenu_HandleEvent(&e);
        }
        DrawScene(false, 1.0f);
        PortMenu_Render();
        SDL_RenderPresent(sRenderer);
        Screen2Present(); // follows the menu's second-screen setting
        if (!VsyncWanted())
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
    sPrevValid = false;
    sAudioQueued = 0;
    sLastPresentNs = 0;
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
    const int startW = PortConfig_WideWidth(sConfig.widescreen == PORT_WIDE_AUTO ? PORT_WIDE_OFF : sConfig.widescreen, 1.5f);
    if (!SDL_CreateWindowAndRenderer("Metroid Fusion", startW * sConfig.windowScale,
                                     GBA_SCREEN_HEIGHT * sConfig.windowScale, flags, &sWindow, &sRenderer)) {
        Port_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return false;
    }
    if (!SetFrameWidth(GBA_SCREEN_WIDTH))
        return false;

    if (sConfig.volume > 0)
        OpenAudio();
    ApplyVideoConfig();

#ifdef __ANDROID__
    sTouchVisible = sConfig.touchControls != PORT_TOUCH_OFF;
#else
    sTouchVisible = sConfig.touchControls == PORT_TOUCH_ON;
#endif
    InitMenu();
    Screen2Init();
    sSyncLog = SDL_getenv("MF_SYNC_LOG") != NULL;
    sNextFrameNs = SDL_GetTicksNS();
    sSyncLogNs = sNextFrameNs;
    return true;
}

void Port_HostShutdown(void)
{
    StopRumble();
    if (sHaptic)
        SDL_CloseHaptic(sHaptic);
    Screen2Shutdown();
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
    if (sPrevTexture)
        SDL_DestroyTexture(sPrevTexture);
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
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (!HandleWindowEvent(&e))
                return false;
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            sMeasuredHz = 0;
            UpdateDisplaySync();
            break;
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

// Did presenting wait for VSync lately? Pacing by VSync only works if it does
// (not with a hidden window, or a driver that ignores it).
static bool VsyncWorks(void)
{
    return VsyncWanted() && sRefreshNs > 0 && sVsyncScore > 0.5f;
}

static int CompareU32(const void* a, const void* b)
{
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

// While locked, checks that VSync really ticks at the reported rate (a phone
// may report its 120 Hz mode but give the app 60 Hz). Refreshes missed when a
// frame runs late only make intervals longer, so the lower quartile of the
// time between presents is the real refresh period.
static void CheckRefreshRate(uint64_t now)
{
    if (sLastPresentNs && now - sLastPresentNs < 200000000ull)
        sIntervals[sIntervalCount++] = (uint32_t)(now - sLastPresentNs);
    sLastPresentNs = now;
    if (sIntervalCount < (int)SDL_arraysize(sIntervals))
        return;
    sIntervalCount = 0;
    qsort(sIntervals, SDL_arraysize(sIntervals), sizeof(sIntervals[0]), CompareU32);
    const double hz = 1e9 / sIntervals[SDL_arraysize(sIntervals) / 4];
    if (SDL_fabs(hz - sRefreshHz) > sRefreshHz * 0.05) {
        Port_Log("VSync runs at %.2f Hz, not %.2f Hz", hz, sRefreshHz);
        sMeasuredHz = hz;
        UpdateDisplaySync();
    }
}

// SDL_RenderPresent, watching whether it waited for the refresh. `start` is
// when drawing this frame began.
static bool Present(uint64_t start)
{
    SDL_RenderPresent(sRenderer);
    const uint64_t now = SDL_GetTicksNS();
    const bool waited = now - start >= sRefreshNs / 4;
    sVsyncScore += ((waited ? 1.0f : 0.0f) - sVsyncScore) * 0.1f;
    if (sSyncDivisor && VsyncWorks() && !FastForwarding())
        CheckRefreshRate(now);
    else
        sLastPresentNs = 0;
    return waited;
}

bool Port_HostPresent(const uint32_t* abgr, int width, int height)
{
    if (!PumpEvents())
        return false;

    if (!SetFrameWidth(width))
        return false;
    // Frame blending keeps the previous frame in a second texture. Only on
    // screens clearly faster than the GBA (at 60 Hz there is nothing to mix).
    sBlendActive = sConfig.frameBlend && sPrevTexture && !sSyncDivisor && !FastForwarding() && sRefreshNs > 0 &&
                   sRefreshNs < GBA_FRAME_NS * 9 / 10 && VsyncWorks();
    if (sBlendActive) {
        SDL_Texture* t = sPrevTexture;
        sPrevTexture = sTexture;
        sTexture = t;
        sPrevValid = true;
    } else {
        sPrevValid = false;
    }
    memcpy(sLastFrame, abgr, (size_t)width * GBA_SCREEN_HEIGHT * sizeof(uint32_t));
    SDL_UpdateTexture(sTexture, NULL, abgr, width * (int)sizeof(uint32_t));
    (void)height;
    // "auto" widescreen follows the window (the next frame uses it).
    if (sConfig.widescreen == PORT_WIDE_AUTO)
        Port_WideSetWidth(WantedFrameWidth());
    sFrameShownNs = SDL_GetTicksNS();
    DrawScene(true, sBlendActive ? PortPacing_BlendAlpha(sFrameShownNs + sRefreshNs, sFrameShownNs, GBA_FRAME_NS)
                                 : 1.0f);
    Present(sFrameShownNs);

    // The second window opens with the first game frame (not over the ROM screen).
    static bool sScreen2Started;
    if (!sScreen2Started) {
        sScreen2Started = true;
        Screen2Apply();
    }
    Screen2Present();
    return true;
}

// Shows the current game frame again for one more refresh. Returns false if
// presenting did not wait for VSync.
static bool PresentAgain(void)
{
    const uint64_t start = SDL_GetTicksNS();
    const float blend = sBlendActive ? PortPacing_BlendAlpha(start + sRefreshNs, sFrameShownNs, GBA_FRAME_NS) : 1.0f;

    DrawScene(true, blend);
    return Present(start);
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

    if (!sAudio || frames <= 0)
        return;
    const int queuedBefore = SDL_GetAudioStreamQueued(sAudio);
    if (sSyncLog) {
        sAudioDry += queuedBefore == 0;
        sAudioDropped += queuedBefore > maxQueued;
    }
    if (queuedBefore > maxQueued)
        return;
    SDL_PutAudioStreamData(sAudio, samples, frames * 4);

    // Locked to the display, the game makes sound a little faster than real
    // time: play it that much faster, nudged to keep the queue at its target.
    if (sSyncDivisor && !FastForwarding()) {
        const int queued = SDL_GetAudioStreamQueued(sAudio);
        if (queued >= 0)
            sAudioQueued += (queued / 4.0 - sAudioQueued) * 0.05;
        SetAudioRatio(PortPacing_AudioRatio(sSyncSpeed, sAudioQueued, AudioTarget()));
    }
}

static void LogSyncStats(void)
{
    const uint64_t now = SDL_GetTicksNS();
    sSyncLogFrames++;
    if (now - sSyncLogNs < 5000000000ull)
        return;
    Port_Log("pacing: %.3f game fps, sound queue %.1f ms (target %.1f), ratio %.5f, ran dry %d, dropped %d, %s",
             sSyncLogFrames * 1e9 / (double)(now - sSyncLogNs),
             sAudio ? SDL_GetAudioStreamQueued(sAudio) / 4.0 / PORT_AUDIO_RATE * 1000 : 0.0,
             AudioTarget() / PORT_AUDIO_RATE * 1000, sAudioRatio, sAudioDry, sAudioDropped,
             sSyncDivisor ? "locked to the display" : sBlendActive ? "blending" : "GBA clock");
    sSyncLogNs = now;
    sSyncLogFrames = 0;
    sAudioDry = sAudioDropped = 0;
}

// Sleeps until the next tick of a `frameNs` clock.
static void WaitTick(uint64_t frameNs)
{
    const uint64_t now = SDL_GetTicksNS();

    sNextFrameNs += frameNs;
    if (sNextFrameNs > now) {
        SDL_DelayPrecise(sNextFrameNs - now);
    } else if (now - sNextFrameNs > 4 * GBA_FRAME_NS) {
        sNextFrameNs = now; // fell far behind (debugger, app suspended): resync
    }
}

// Locked to the display: VSync paces the game (presenting waited for the
// refresh). If it does not wait (hidden window, driver that ignores VSync),
// a clock at the display's rate takes over.
static void WaitLocked(void)
{
    for (int i = 1; i < sSyncDivisor; i++)
        PresentAgain();
    if (VsyncWorks())
        sNextFrameNs = SDL_GetTicksNS();
    else
        WaitTick(sSyncFrameNs);
}

void Port_HostWaitFrame(void)
{
    const bool fast = FastForwarding();
    const uint64_t frameNs = fast ? GBA_FRAME_NS / (uint64_t)sConfig.fastForwardSpeed : GBA_FRAME_NS;

    if (sSyncLog)
        LogSyncStats();
    if (sSyncDivisor && !fast) {
        WaitLocked();
        return;
    }
    // Frame blending: show the in-between refreshes until the next game frame.
    if (sBlendActive) {
        while (SDL_GetTicksNS() + sRefreshNs <= sNextFrameNs + frameNs) {
            if (!PresentAgain())
                break;
        }
    }

    WaitTick(frameNs);
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

// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — widescreen (more of the room on the sides instead of bars).
//
// The game keeps running exactly as on the GBA: its camera, its 240 px
// screen and its logic don't change. Only the picture is wider: the PPU draws
// the GBA screen at column `origin` of a wider frame and keeps going on both
// sides (port/ppu: ws_origin, ws_obj). For that to show real room:
//
//   - the game side (port/game/widescreen.c) keeps the tilemap columns just
//     outside the GBA screen loaded, draws the sprites that are in the extra
//     columns and tags every OAM entry (room object, left/right HUD, banner);
//   - here, the frame is placed inside the part of the room the camera may
//     show (the room's scroll bounds): normally the GBA screen is centered,
//     near an edge the picture stops at the edge and the GBA screen slides
//     off-center, and where the area is narrower than the frame (vertical
//     shafts, boss rooms) it is centered with black on the sides. The HUD
//     sticks to the edges of what is shown.
//
// Frames outside the in-game loop (title, menus, map, cutscenes) are drawn at
// 240 px and centered.
#include "port.h"

#include "../ppu/include/cpu/mode1.h"
#include "../ppu/include/virtuappu.h"

static int sWidth = GBA_SCREEN_WIDTH;

// Reported by the game for the frame being drawn.
static struct {
    bool set;
    uint8_t tags[128];
    int cameraX, left, right;
} sFrame;

// Placement of the last wide frame (-1: the last frame wasn't wide).
static int sOrigin = -1;
static int sLastCameraX;

static uint32_t sWide[PORT_MAX_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];

void Port_WideSetWidth(int width)
{
    if (width < GBA_SCREEN_WIDTH)
        width = GBA_SCREEN_WIDTH;
    if (width > PORT_MAX_SCREEN_WIDTH)
        width = PORT_MAX_SCREEN_WIDTH;
    width &= ~1;
    if (width != sWidth)
        sOrigin = -1;
    sWidth = width;
}

int Port_WideWidth(void)
{
    return sWidth;
}

int Port_WideExtra(void)
{
    return (sWidth - GBA_SCREEN_WIDTH) / 2;
}

int Port_WideOrigin(void)
{
    return sOrigin >= 0 ? sOrigin : Port_WideExtra();
}

void Port_WideSetFrame(const unsigned char* oamTags, int cameraX, int left, int right)
{
    memcpy(sFrame.tags, oamTags, sizeof(sFrame.tags));
    sFrame.cameraX = cameraX;
    sFrame.left = left;
    sFrame.right = right;
    sFrame.set = true;
}

static int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static const uint32_t* RenderWide(void)
{
    const int w = sWidth;
    const int extra = (w - GBA_SCREEN_WIDTH) / 2;
    const int cam = sFrame.cameraX;
    // The area the camera may show always contains the GBA screen.
    int left = sFrame.left < cam ? sFrame.left : cam;
    int right = sFrame.right > cam + GBA_SCREEN_WIDTH ? sFrame.right : cam + GBA_SCREEN_WIDTH;
    int target;

    if (right - left >= w)
        target = Clamp(extra, cam + w - right, cam - left);
    else
        target = cam - left + (w - (right - left)) / 2; // narrow: center it
    target = Clamp(target, 0, 2 * extra);

    // Glide when the bounds change (entering another scroll area), jump when
    // the camera does (new room, save state).
    if (sOrigin < 0 || cam - sLastCameraX > 64 || sLastCameraX - cam > 64)
        sOrigin = target;
    else if (sOrigin < target)
        sOrigin = sOrigin + 2 < target ? sOrigin + 2 : target;
    else if (sOrigin > target)
        sOrigin = sOrigin - 2 > target ? sOrigin - 2 : target;
    sLastCameraX = cam;

    const int origin = sOrigin;
    const int visL = Clamp(left - cam + origin, 0, w);
    const int visR = Clamp(right - cam + origin, visL, w);

    virtuappu_mode1_ws_active = 1;
    virtuappu_mode1_ws_origin = origin;
    for (int i = 0; i < 128; i++) {
        VirtuaPPUMode1WsObj* o = &virtuappu_mode1_ws_obj[i];
        o->wrap = GBA_SCREEN_WIDTH;
        o->clip_l = 0;
        o->clip_r = (int16_t)w;
        switch (sFrame.tags[i]) {
        case PORT_WIDE_OAM_WORLD:
            o->shift = (int16_t)origin;
            o->wrap = (int16_t)(w - origin);
            break;
        case PORT_WIDE_OAM_HUD_LEFT:
            o->shift = (int16_t)visL;
            break;
        case PORT_WIDE_OAM_HUD_RIGHT:
            o->shift = (int16_t)(visR - GBA_SCREEN_WIDTH);
            break;
        case PORT_WIDE_OAM_CENTER:
            o->shift = (int16_t)extra;
            break;
        default: // PORT_WIDE_OAM_SCREEN
            o->shift = (int16_t)origin;
            o->clip_l = (int16_t)origin;
            o->clip_r = (int16_t)(origin + GBA_SCREEN_WIDTH);
            break;
        }
    }

    uint32_t* frame = (uint32_t*)Port_PpuRenderFrame(w);
    virtuappu_mode1_ws_active = 0;

    // Outside the room's visible area: black, like the bars it replaces.
    if (visL > 0 || visR < w) {
        for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
            uint32_t* row = frame + (size_t)y * w;
            for (int x = 0; x < visL; x++)
                row[x] = 0xFF000000u;
            for (int x = visR; x < w; x++)
                row[x] = 0xFF000000u;
        }
    }
    return frame;
}

const uint32_t* Port_WideRenderFrame(int* width)
{
    const bool wideFrame = sFrame.set;
    sFrame.set = false;

    *width = sWidth;
    if (sWidth == GBA_SCREEN_WIDTH)
        return Port_PpuRenderFrame(GBA_SCREEN_WIDTH);
    if (wideFrame)
        return RenderWide();

    // Not in game: the GBA picture in the middle.
    sOrigin = -1;
    const uint32_t* gba = Port_PpuRenderFrame(GBA_SCREEN_WIDTH);
    const int extra = (sWidth - GBA_SCREEN_WIDTH) / 2;
    for (int y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        uint32_t* row = sWide + (size_t)y * sWidth;
        for (int x = 0; x < extra; x++)
            row[x] = row[sWidth - 1 - x] = 0xFF000000u;
        memcpy(row + extra, gba + (size_t)y * GBA_SCREEN_WIDTH, GBA_SCREEN_WIDTH * sizeof(uint32_t));
    }
    return sWide;
}

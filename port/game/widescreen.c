// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — widescreen, game side (the port side is port/src/widescreen.c).
//
// Nothing here changes what the game does: the camera, the sprites' "on
// screen" status (that the AI reads) and the logic stay as on the GBA. This
// only makes sure the extra columns of a wide frame have something real in
// them:
//
//   - the room tilemaps (BG0-2) are a 32x16-block ring in VRAM that the game
//     streams for its 240 px screen (blocks x-2..x+16); the columns a wider
//     frame also needs are rewritten here every frame from the room data, the
//     same way RoomUpdateHorizontalTilemap / RoomCopyHorizontalTilemap do;
//   - sprites whose box is in the extra columns are drawn too (sprite.c asks
//     Port_WideSpriteStatus instead of reading the status), keeping some OAM
//     free for Samus and the projectiles that are drawn after them;
//   - every OAM entry gets a placement tag: HUD (left or right edge), banners
//     (centered), room objects;
//   - the port is told where the camera is and which part of the room it may
//     show (the scroll bounds), so it can stop the picture at the edges.
#include "types.h"
#include "globals.h"
#include "macros.h"
#include "oam.h"

#include "constants/room.h"
#include "constants/sprite.h"

#include "data/clipdata_data.h"

#include "structs/bg_clip.h"
#include "structs/clipdata.h"
#include "structs/room.h"
#include "structs/scroll.h"
#include "structs/sprite.h"

// OAM entries drawn this frame by sprites with a screen position (banners).
static u8 sCenterMark[OAM_BUFFER_DATA_SIZE];

void Port_WideMarkCenter(s32 from, s32 to)
{
    s32 i;

    if (Port_WideExtra() == 0)
        return;
    for (i = from; i < to && i < OAM_BUFFER_DATA_SIZE; i++)
        sCenterMark[i] = TRUE;
}

u16 Port_WideSpriteStatus(struct SpriteData* pSprite)
{
    u16 status = pSprite->status;
    s32 extra = Port_WideExtra();
    s32 origin;
    s32 dx;
    s32 dy;
    s32 dist;

    if (extra == 0 || status & SPRITE_STATUS_ONSCREEN || !(status & SPRITE_STATUS_EXISTS) ||
        pSprite->properties & SP_ABSOLUTE_POSITION)
        return status;

    // SpriteCheckOnScreen with the frame's columns instead of the GBA's
    // (in sub-pixels: 4 per pixel).
    origin = Port_WideOrigin();
    dx = (s32)pSprite->xPosition - (s32)gBg1XPosition;
    dist = PIXEL_TO_SUB_PIXEL(pSprite->drawDistanceHorizontal);
    if (dx <= -dist - PIXEL_TO_SUB_PIXEL(origin))
        return status;
    if (dx >= SCREEN_SIZE_X_SUB_PIXEL + dist + PIXEL_TO_SUB_PIXEL(2 * extra - origin))
        return status;

    dy = (s32)pSprite->yPosition - (s32)gBg1YPosition;
    if (dy <= -PIXEL_TO_SUB_PIXEL(pSprite->drawDistanceBottom))
        return status;
    if (dy >= SCREEN_SIZE_Y_SUB_PIXEL + PIXEL_TO_SUB_PIXEL(pSprite->drawDistanceTop))
        return status;

    return status | SPRITE_STATUS_ONSCREEN;
}

// Rewrites VRAM tilemap columns [first, last] of the RLE backgrounds (the rows
// the game keeps loaded: y-2..y+11), from the decompressed room data.
static void RefreshColumns(s32 bg, s32 first, s32 last)
{
    s32 width = gBackgroundsData.backgrounds[bg].width;
    s32 height = gBackgroundsData.backgrounds[bg].height;
    u16* pDecomp = gBackgroundsData.backgrounds[bg].pDecomp;
    s32 yStart;
    s32 x;
    s32 y;

    if (bg == 0)
        yStart = SUB_PIXEL_TO_BLOCK(gBg0YPosition) - 2;
    else if (bg == 1)
        yStart = SUB_PIXEL_TO_BLOCK(gBg1YPosition) - 2;
    else
        yStart = SUB_PIXEL_TO_BLOCK(gBg2YPosition) - 2;

    if (first < 0)
        first = 0;
    if (last > width - 1)
        last = width - 1;
    if (yStart < 0)
        yStart = 0;

    for (x = first; x <= last; x++)
    {
        u32* dst = (u32*)(VRAM_BASE + bg * 0x1000 + ((x & 0x10) ? 0x800 : 0)) + (x & 0xF);

        for (y = yStart; y < yStart + 14 && y < height; y++)
        {
            // Bit 15: a block of the common tilemap (SET_TILES_HORI).
            u16 block = pDecomp[y * width + x];
            const u16* pTilemap = block & 0x8000 ? &sCommonTilemap[(block & 0x7FFF) * 4] :
                &gTilemapAndClipPointers.pTilemap[block * 4];
            u32 offset = (y & 0xF) * 32;

            dst[offset] = pTilemap[0] | pTilemap[1] << 16;
            dst[offset + 16] = pTilemap[2] | pTilemap[3] << 16;
        }
    }
}

static void RefreshTilemaps(s32 extra)
{
    // Blocks the frame can show, relative to the camera's block x: the GBA
    // screen may sit up to 2 * extra px from the left edge of the frame.
    s32 left = (2 * extra + BLOCK_SIZE / PIXEL_SIZE - 1) / (BLOCK_SIZE / PIXEL_SIZE) + 1;
    s32 right = (SCREEN_SIZE_X + 2 * extra + 14) / (BLOCK_SIZE / PIXEL_SIZE) + 1;
    s32 bg;
    s32 x;
    u16 props;

    // While a door fades out the next room may already be loaded.
    if (gSubGameMode1 == SUB_GAME_MODE_LOADING_ROOM)
        return;

    for (bg = 0; bg < 3; bg++)
    {
        props = bg == 0 ? gCurrentRoomEntry.bg0Prop : bg == 1 ? gCurrentRoomEntry.bg1Prop : gCurrentRoomEntry.bg2Prop;
        if (!(props & BG_PROP_RLE_COMPRESSED))
            continue;

        x = SUB_PIXEL_TO_BLOCK(bg == 0 ? gBg0XPosition : bg == 1 ? gBg1XPosition : gBg2XPosition);
        // The game streams x-2..x+16 itself.
        RefreshColumns(bg, x - left, x - 3);
        RefreshColumns(bg, x + 17, x + right);
    }
}

void Port_WideInGameFrame(s32 hudEnd, s32 drewOam)
{
    static u8 sTags[OAM_BUFFER_DATA_SIZE];
    s32 extra = Port_WideExtra();
    s32 i;
    s32 left;
    s32 right;
    struct Scroll* pScroll;

    if (extra == 0)
        return;

    if (drewOam)
    {
        for (i = 0; i < OAM_BUFFER_DATA_SIZE; i++)
        {
            if (i < hudEnd)
                sTags[i] = gOamData[i].split.x < SCREEN_X_MIDDLE ? PORT_WIDE_OAM_HUD_LEFT : PORT_WIDE_OAM_HUD_RIGHT;
            else if (i < gNextOamSlot)
                sTags[i] = sCenterMark[i] ? PORT_WIDE_OAM_CENTER : PORT_WIDE_OAM_WORLD;
            else
                sTags[i] = PORT_WIDE_OAM_SCREEN;
        }
    }
    for (i = 0; i < OAM_BUFFER_DATA_SIZE; i++)
        sCenterMark[i] = FALSE;

    RefreshTilemaps(extra);

    // The part of the room the camera may show: the current scroll areas, or
    // the whole room minus its 2-block border.
    left = BLOCK_TO_SUB_PIXEL(gBackgroundsData.clipdataWidth);
    right = 0;
    if (gCurrentRoomEntry.scrollsFlag == ROOM_SCROLLS_FLAG_HAS_SCROLLS)
    {
        for (i = 0, pScroll = gCurrentScrolls; i < 2; i++, pScroll++)
        {
            if (pScroll->within == SCROLL_NOT_WITHIN_FLAG)
                continue;
            if (pScroll->xStart < left)
                left = pScroll->xStart;
            if (pScroll->xEnd > right)
                right = pScroll->xEnd;
        }
    }
    if (right <= left)
    {
        left = SCREEN_X_BLOCK_PADDING;
        right = BLOCK_TO_SUB_PIXEL(gBackgroundsData.clipdataWidth) - SCREEN_X_BLOCK_PADDING;
    }

    Port_WideSetFrame(sTags, SUB_PIXEL_TO_PIXEL(gBg1XPosition), SUB_PIXEL_TO_PIXEL(left),
        SUB_PIXEL_TO_PIXEL(right));
}

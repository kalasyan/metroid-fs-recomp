// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — game state for the port's extras: the second screen (area
// map, Samus' status) and rumble.
//
// Only reads the game's globals, once per frame, and compares them with the
// previous frame to find events (Samus hurt, a bomb exploding, a hard
// landing, the screen shaking...). The game code isn't touched, so this
// can't change how the game plays. The previous-frame copy isn't part of
// the machine state (a save state doesn't need it): after loading one,
// Port_GameStatusReset() makes the next call start over without events.
#include "types.h"
#include "globals.h"
#include "macros.h"

#include "data/menus/pause_screen_map_data.h"
#include "pause_screen.h"

#include "constants/connection.h"
#include "constants/minimap.h"
#include "constants/projectile.h"
#include "constants/samus.h"

#include "structs/connection.h"
#include "structs/in_game_timer.h"
#include "structs/minimap.h"
#include "structs/power_bomb.h"
#include "structs/projectile.h"
#include "structs/samus.h"
#include "structs/screen_shake.h"

// A hard landing is a fall from higher than a high jump on flat ground
// (about 5 blocks).
#define HARD_LANDING_HEIGHT (7 * BLOCK_SIZE)
// The pause screen's map uses the first three rows of its base palette
// (unexplored, explored, hidden), like the tile entries' palette field.
#define MAP_PALETTE_ROWS 3

static struct {
    boolu8 valid;
    boolu8 playing;
    u8 pose;
    u8 invincibility;
    u8 powerBombState;
    u16 energy;
    boolu8 inAir;
    u16 airTop; // highest point (smallest y) of the current jump or fall
    u16 maxEnergy, maxMissiles;
    u8 maxPowerBombs;
    u8 beams, weapons, suitMisc;
    u8 projType[MAX_AMOUNT_OF_PROJECTILES];
    u8 projStage[MAX_AMOUNT_OF_PROJECTILES];
} sPrev;

void Port_GameStatusReset(void)
{
    sPrev.valid = FALSE;
}

static boolu32 IsHurtPose(u8 pose)
{
    return pose == SPOSE_GETTING_HURT || pose == SPOSE_GETTING_KNOCKED_BACK ||
           pose == SPOSE_GETTING_HURT_IN_MORPH_BALL || pose == SPOSE_GETTING_KNOCKED_BACK_IN_MORPH_BALL ||
           pose == SPOSE_HURT_REQUEST || pose == SPOSE_KNOCKBACK_REQUEST;
}

static boolu32 IsAirPose(u8 pose)
{
    return pose == SPOSE_MID_AIR || pose == SPOSE_TURNING_AROUND_MID_AIR || pose == SPOSE_SPINNING ||
           pose == SPOSE_STARTING_SPIN_JUMP || pose == SPOSE_SPACE_JUMPING || pose == SPOSE_SCREW_ATTACKING ||
           pose == SPOSE_MORPH_BALL_MID_AIR || pose == SPOSE_MID_AIR_REQUEST || pose == SPOSE_WALL_JUMPING;
}

static u32 Items(u8 beams, u8 weapons, u8 suitMisc)
{
    u32 items = 0;

    if (beams & BF_CHARGE_BEAM) items |= PORT_ITEM_CHARGE_BEAM;
    if (beams & BF_WIDE_BEAM) items |= PORT_ITEM_WIDE_BEAM;
    if (beams & BF_PLASMA_BEAM) items |= PORT_ITEM_PLASMA_BEAM;
    if (beams & BF_WAVE_BEAM) items |= PORT_ITEM_WAVE_BEAM;
    if (beams & BF_ICE_BEAM) items |= PORT_ITEM_ICE_BEAM;
    if (weapons & MBF_MISSILES) items |= PORT_ITEM_MISSILES;
    if (weapons & MBF_SUPER_MISSILES) items |= PORT_ITEM_SUPER_MISSILES;
    if (weapons & MBF_ICE_MISSILES) items |= PORT_ITEM_ICE_MISSILES;
    if (weapons & MBF_DIFFUSION_MISSILES) items |= PORT_ITEM_DIFFUSION_MISSILES;
    if (weapons & MBF_BOMBS) items |= PORT_ITEM_BOMBS;
    if (weapons & MBF_POWER_BOMBS) items |= PORT_ITEM_POWER_BOMBS;
    if (suitMisc & SMF_MORPH_BALL) items |= PORT_ITEM_MORPH_BALL;
    if (suitMisc & SMF_HIGH_JUMP) items |= PORT_ITEM_HIGH_JUMP;
    if (suitMisc & SMF_SPEED_BOOSTER) items |= PORT_ITEM_SPEED_BOOSTER;
    if (suitMisc & SMF_SPACE_JUMP) items |= PORT_ITEM_SPACE_JUMP;
    if (suitMisc & SMF_SCREW_ATTACK) items |= PORT_ITEM_SCREW_ATTACK;
    if (suitMisc & SMF_VARIA_SUIT) items |= PORT_ITEM_VARIA_SUIT;
    if (suitMisc & SMF_GRAVITY_SUIT) items |= PORT_ITEM_GRAVITY_SUIT;
    return items;
}

static s32 ShakeLevel(const struct ScreenShake* pShake)
{
    if (pShake->timer == 0)
        return 0;
    return pShake->timer > CONVERT_SECONDS(1.f / 4) ? 2 : 1;
}

// The cells of the area map that have a room (explored or not).
static void MapBounds(struct PortGameStatus* pStatus)
{
    s32 x;
    s32 y;
    u16 tile;

    pStatus->mapMinX = MINIMAP_DIM;
    pStatus->mapMinY = MINIMAP_DIM;
    pStatus->mapMaxX = -1;
    pStatus->mapMaxY = -1;
    for (y = 0; y < MINIMAP_DIM; y++) {
        for (x = 0; x < MINIMAP_DIM; x++) {
            tile = gMinimapData[y * MINIMAP_DIM + x] & 0x3FF;
            if (tile == MINIMAP_TILE_BACKGROUND || tile == 0)
                continue;
            if (x < pStatus->mapMinX) pStatus->mapMinX = x;
            if (x > pStatus->mapMaxX) pStatus->mapMaxX = x;
            if (y < pStatus->mapMinY) pStatus->mapMinY = y;
            if (y > pStatus->mapMaxY) pStatus->mapMaxY = y;
        }
    }
    if (pStatus->mapMaxX < 0) {
        pStatus->mapMinX = pStatus->mapMinY = 0;
        pStatus->mapMaxX = pStatus->mapMaxY = MINIMAP_DIM - 1;
    }
}

static void Events(struct PortGameStatus* pStatus)
{
    s32 i;
    u8 pose = gSamusData.pose;
    struct ProjectileData* pProj;

    if (!sPrev.valid || !sPrev.playing || !pStatus->playing)
        return;

    // Hurt: energy lost and the invincibility frames that follow a hit.
    if (gEquipment.currentEnergy < sPrev.energy) {
        pStatus->damage = sPrev.energy - gEquipment.currentEnergy;
        if (gSamusData.invincibilityTimer > sPrev.invincibility || (IsHurtPose(pose) && !IsHurtPose(sPrev.pose)))
            pStatus->events |= PORT_EVENT_DAMAGE;
        else
            pStatus->events |= PORT_EVENT_DRAIN;
    }

    if (pose == SPOSE_DYING && sPrev.pose != SPOSE_DYING)
        pStatus->events |= PORT_EVENT_DEATH;

    if ((pose == SPOSE_LANDING || pose == SPOSE_LANDING_REQUEST || pose == SPOSE_MORPH_BALL) && sPrev.inAir &&
        gSamusData.yPosition > sPrev.airTop && gSamusData.yPosition - sPrev.airTop >= HARD_LANDING_HEIGHT) {
        pStatus->events |= PORT_EVENT_LANDING;
        pStatus->fallHeight = gSamusData.yPosition - sPrev.airTop;
    }

    // A shinespark ends with this pose when it hits a wall.
    if (pose == SPOSE_DELAY_AFTER_SHINESPARKING && pose != sPrev.pose)
        pStatus->events |= PORT_EVENT_SHINESPARK;

    for (i = 0, pProj = gProjectileData; i < MAX_AMOUNT_OF_PROJECTILES; i++, pProj++) {
        if (!(pProj->status & PROJ_STATUS_EXISTS) || pProj->type != PROJ_TYPE_BOMB)
            continue;
        if (pProj->movementStage == BOMB_STAGE_EXPLODING &&
            !(sPrev.projType[i] == PROJ_TYPE_BOMB && sPrev.projStage[i] == pProj->movementStage))
            pStatus->events |= PORT_EVENT_BOMB;
    }

    if (gCurrentPowerBomb.animationState == PB_STATE_EXPLODING && sPrev.powerBombState != PB_STATE_EXPLODING)
        pStatus->events |= PORT_EVENT_POWER_BOMB;

    if (gEquipment.maxEnergy > sPrev.maxEnergy || gEquipment.maxMissiles > sPrev.maxMissiles ||
        gEquipment.maxPowerBombs > sPrev.maxPowerBombs || (gEquipment.beamStatus & ~sPrev.beams) ||
        (gEquipment.weaponsStatus & ~sPrev.weapons) || (gEquipment.suitMiscStatus & ~sPrev.suitMisc))
        pStatus->events |= PORT_EVENT_ITEM;
}

static void Remember(const struct PortGameStatus* pStatus)
{
    s32 i;

    sPrev.valid = TRUE;
    sPrev.playing = pStatus->playing;
    if (!IsAirPose(gSamusData.pose)) {
        sPrev.inAir = FALSE;
    } else if (!sPrev.inAir || gSamusData.yPosition < sPrev.airTop) {
        sPrev.inAir = TRUE;
        sPrev.airTop = gSamusData.yPosition;
    }
    sPrev.pose = gSamusData.pose;
    sPrev.invincibility = gSamusData.invincibilityTimer;
    sPrev.powerBombState = gCurrentPowerBomb.animationState;
    sPrev.energy = gEquipment.currentEnergy;
    sPrev.maxEnergy = gEquipment.maxEnergy;
    sPrev.maxMissiles = gEquipment.maxMissiles;
    sPrev.maxPowerBombs = gEquipment.maxPowerBombs;
    sPrev.beams = gEquipment.beamStatus;
    sPrev.weapons = gEquipment.weaponsStatus;
    sPrev.suitMisc = gEquipment.suitMiscStatus;
    for (i = 0; i < MAX_AMOUNT_OF_PROJECTILES; i++) {
        sPrev.projType[i] = (gProjectileData[i].status & PROJ_STATUS_EXISTS) ? gProjectileData[i].type : 0xFF;
        sPrev.projStage[i] = gProjectileData[i].movementStage;
    }
}

void Port_GameStatus(struct PortGameStatus* pStatus)
{
    // Zeroed by hand: no libc here.
    u8* p = (u8*)pStatus;
    u32 i;
    boolu32 inGame;

    for (i = 0; i < sizeof(*pStatus); i++)
        p[i] = 0;
    pStatus->area = -1;

    inGame = gMainGameMode == GAME_MODE_IN_GAME;
    pStatus->playing = inGame && gSubGameMode1 == SUB_GAME_MODE_PLAYING;
    pStatus->paused = gMainGameMode == GAME_MODE_MAP_SCREEN;

    if (inGame || pStatus->paused) {
        pStatus->area = gCurrentArea < AREA_NORMAL_COUNT ? gCurrentArea : -1;
        pStatus->mapX = gMinimapX;
        pStatus->mapY = gMinimapY;
        pStatus->energy = gEquipment.currentEnergy;
        pStatus->maxEnergy = gEquipment.maxEnergy;
        pStatus->missiles = gEquipment.currentMissiles;
        pStatus->maxMissiles = gEquipment.maxMissiles;
        pStatus->powerBombs = gEquipment.currentPowerBombs;
        pStatus->maxPowerBombs = gEquipment.maxPowerBombs;
        pStatus->items = Items(gEquipment.beamStatus, gEquipment.weaponsStatus, gEquipment.suitMiscStatus);
        pStatus->itemsActive = pStatus->items;
        pStatus->hours = gInGameTimer.hours;
        pStatus->minutes = gInGameTimer.minutes;
        pStatus->seconds = gInGameTimer.seconds;
    }

    // The minimap keeps the explored / downloaded rooms of the current area;
    // the pause screen works on the same buffer.
    if (pStatus->area >= 0 && gPreviousArea == gCurrentArea) {
        pStatus->map = gMinimapData_Visited;
        pStatus->mapTiles = sMinimapTilesGfx;
        pStatus->mapTileCount = sizeof(sMinimapTilesGfx) / TILE_SIZE;
        pStatus->mapPalettes = (const u16*)sPauseScreenBgPal_Base;
        pStatus->mapPaletteCount = MAP_PALETTE_ROWS;
        MapBounds(pStatus);
    }

    if (pStatus->playing) {
        pStatus->shake = ShakeLevel(&gScreenShakeX) + ShakeLevel(&gScreenShakeY);
        pStatus->powerBombBlast = gCurrentPowerBomb.animationState == PB_STATE_EXPLODING ||
                                  gCurrentPowerBomb.animationState == PB_STATE_IMPLODING;
    }

    Events(pStatus);
    Remember(pStatus);
}

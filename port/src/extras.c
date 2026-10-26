// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — rumble from game events, and the game state the second
// screen draws.
//
// Every frame port/game/status.c reads the game's globals and reports what
// happened (Samus hurt, a bomb or power bomb exploding, a hard landing, the
// screen shaking for a boss...). Here each event starts a short rumble
// "effect"; the strongest one sets the motors. The host gets both the motor
// levels (gamepads, refreshed every frame) and, for simple vibrators like a
// phone's, a pulse when a new vibration should start.
//
// MF_RUMBLE_LOG=1 logs every event (used to test it in mf_headless).
#include "port.h"

#include <stdio.h>
#include <stdlib.h>

#define MAX_EFFECTS 8
#define MIN_F(a, b) ((a) < (b) ? (a) : (b))
#define MAX_F(a, b) ((a) > (b) ? (a) : (b))

typedef struct Effect {
    float low, high; // motor strength 0..1
    int frames;      // frames left
    int total;
} Effect;

static Effect sEffects[MAX_EFFECTS];
static struct PortGameStatus sStatus;
static uint16_t sMap[PORT_MAP_SIZE * PORT_MAP_SIZE];
static struct PortGameStatus sMapInfo; // map fields of the last frame that had one
static bool sHaveMap;
static int sLog = -1;
static int sPrevShake;
static int sPulseFramesLeft;
static float sPulseStrength;
static int sDrainCooldown;

static void StartEffect(float low, float high, int frames)
{
    int slot = 0;

    // Replace the weakest effect when they're all in use.
    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (sEffects[i].frames <= 0) {
            slot = i;
            break;
        }
        if (sEffects[i].low + sEffects[i].high < sEffects[slot].low + sEffects[slot].high)
            slot = i;
    }
    sEffects[slot] = (Effect){ low, high, frames, frames };
}

static void LogEvent(const char* name, int value)
{
    if (sLog < 0) {
        const char* env = getenv("MF_RUMBLE_LOG");
        sLog = env && *env && *env != '0';
    }
    if (sLog)
        Port_Log("rumble: frame %llu %s %d", (unsigned long long)Port_FrameCount(), name, value);
}

static void StartEvents(const struct PortGameStatus* s)
{
    const unsigned e = s->events;

    if (e & PORT_EVENT_DEATH) {
        LogEvent("death", 0);
        StartEffect(1.0f, 1.0f, 60);
    }
    if (e & PORT_EVENT_DAMAGE) {
        // Stronger for bigger hits (most enemies do 5-20, bosses up to 60).
        const float k = MIN_F(s->damage, 40) / 40.0f;
        LogEvent("damage", s->damage);
        StartEffect(0.55f + 0.45f * k, 0.45f + 0.3f * k, 15);
    } else if ((e & PORT_EVENT_DRAIN) && sDrainCooldown <= 0) {
        // Lava, acid, a metroid: a light buzz now and then, not all the time.
        LogEvent("drain", s->damage);
        StartEffect(0.0f, 0.3f, 5);
        sDrainCooldown = 30;
    }
    if (e & PORT_EVENT_POWER_BOMB) {
        LogEvent("power_bomb", 0);
        StartEffect(1.0f, 0.6f, 40);
    }
    if (e & PORT_EVENT_BOMB) {
        LogEvent("bomb", 0);
        StartEffect(0.45f, 0.3f, 9);
    }
    if (e & PORT_EVENT_SHINESPARK) {
        LogEvent("shinespark", 0);
        StartEffect(0.9f, 0.7f, 18);
    }
    if (e & PORT_EVENT_LANDING) {
        // Up to twice as strong for the longest falls.
        const float k = MIN_F(s->fallHeight, 14 * 64) / (14.0f * 64.0f);
        LogEvent("landing", s->fallHeight / 64);
        StartEffect(0.35f + 0.5f * k, 0.15f + 0.15f * k, 8);
    }
    if (e & PORT_EVENT_ITEM) {
        LogEvent("item", 0);
        StartEffect(0.3f, 0.35f, 12);
    }
    if (s->shake > 0 && sPrevShake == 0)
        LogEvent("shake", s->shake);
    sPrevShake = s->shake;
}

static void UpdateRumble(const struct PortGameStatus* s)
{
    float low = 0.0f, high = 0.0f;
    int longest = 0;

    if (sDrainCooldown > 0)
        sDrainCooldown--;
    if (s->playing)
        StartEvents(s);
    else
        sPrevShake = 0;

    for (int i = 0; i < MAX_EFFECTS; i++) {
        Effect* fx = &sEffects[i];
        if (fx->frames <= 0)
            continue;
        // Full strength, then fades out over the last 40%.
        const float fade = MIN_F(1.0f, (float)fx->frames / (0.4f * (float)fx->total));
        low = MAX_F(low, fx->low * fade);
        high = MAX_F(high, fx->high * fade);
        if (fx->frames > longest)
            longest = fx->frames;
        fx->frames--;
    }
    // Rumble that lasts as long as it's on screen: shaking (bosses, quakes)
    // and a power bomb's blast.
    if (s->playing && s->shake > 0) {
        low = MAX_F(low, 0.2f * (float)s->shake);
        high = MAX_F(high, 0.1f * (float)s->shake);
        if (longest < 6)
            longest = 6;
    }
    if (s->playing && s->powerBombBlast) {
        low = MAX_F(low, 0.3f);
        if (longest < 6)
            longest = 6;
    }

    // Pulses for simple vibrators: a new one when the rumble gets clearly
    // stronger or the previous one ran out while it still goes on.
    float pulse = 0.0f;
    int pulseMs = 0;
    const float level = MAX_F(low, high);
    if (sPulseFramesLeft > 0)
        sPulseFramesLeft--;
    if (level > 0.05f && (sPulseFramesLeft <= 0 || level > sPulseStrength + 0.15f)) {
        pulse = level;
        pulseMs = longest * 1000 / 60;
        if (pulseMs < 40)
            pulseMs = 40;
        sPulseStrength = level;
        sPulseFramesLeft = longest;
    } else if (sPulseFramesLeft <= 0) {
        sPulseStrength = 0.0f;
    }

    Port_HostRumble(low, high, pulse, pulseMs);
}

void Port_ExtrasFrame(void)
{
    Port_GameStatus(&sStatus);
    // Keep a copy of the map: the pause screen overwrites it in EWRAM, and the
    // second screen may be drawn later (on Android, from another thread's request).
    if (sStatus.map) {
        memcpy(sMap, sStatus.map, sizeof(sMap));
        sMapInfo = sStatus;
        sHaveMap = true;
    } else if (sHaveMap && sStatus.paused) {
        sStatus.mapTiles = sMapInfo.mapTiles;
        sStatus.mapTileCount = sMapInfo.mapTileCount;
        sStatus.mapPalettes = sMapInfo.mapPalettes;
        sStatus.mapPaletteCount = sMapInfo.mapPaletteCount;
        sStatus.mapMinX = sMapInfo.mapMinX;
        sStatus.mapMinY = sMapInfo.mapMinY;
        sStatus.mapMaxX = sMapInfo.mapMaxX;
        sStatus.mapMaxY = sMapInfo.mapMaxY;
        sStatus.map = sMap;
    }
    if (sStatus.map)
        sStatus.map = sMap;
    UpdateRumble(&sStatus);
}

void Port_ExtrasReset(void)
{
    Port_GameStatusReset();
    for (int i = 0; i < MAX_EFFECTS; i++)
        sEffects[i].frames = 0;
    sPrevShake = 0;
}

const struct PortGameStatus* Port_ExtrasStatus(void)
{
    return &sStatus;
}

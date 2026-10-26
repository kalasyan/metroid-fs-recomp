// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — the second screen (AYN Thor's lower display, or a second
// window on PC): the area map with Samus' position, or her status.
//
// Drawn in software into a small canvas (256 pixels wide) that the host
// scales to the display. The map uses the pause screen's own tiles and
// palettes, with the explored / downloaded rooms the game keeps for the
// minimap. Text uses an 8x8 bitmap font, ASCII only.
#include "port.h"
#include "font8x8.h"

#include <stdio.h>

#define TILE 8
#define HEADER_H 14
#define FOOTER_H 14

static int MaxI(int a, int b) { return a > b ? a : b; }
static int MinI(int a, int b) { return a < b ? a : b; }
static int ClampI(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t* sPx;
static int sW, sH;

static uint32_t Rgb(int r, int g, int b)
{
    return 0xFF000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
}

static uint32_t Bgr555(uint16_t c)
{
    const int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return Rgb(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2);
}

#define COLOR_BG Rgb(6, 8, 22)
#define COLOR_PANEL Rgb(16, 22, 48)
#define COLOR_TEXT Rgb(230, 236, 255)
#define COLOR_DIM Rgb(110, 120, 150)
#define COLOR_OFF Rgb(52, 58, 82)
#define COLOR_LABEL Rgb(120, 200, 255)
#define COLOR_ENERGY Rgb(255, 214, 90)
#define COLOR_TANK Rgb(255, 120, 200)

static void Fill(int x, int y, int w, int h, uint32_t c)
{
    for (int j = MaxI(y, 0); j < y + h && j < sH; j++) {
        for (int i = MaxI(x, 0); i < x + w && i < sW; i++)
            sPx[j * sW + i] = c;
    }
}

static void Text(int x, int y, int scale, uint32_t c, const char* s)
{
    for (; *s; s++, x += 8 * scale) {
        const unsigned ch = (unsigned char)*s;
        if (ch < PORT_FONT_FIRST || ch > PORT_FONT_LAST)
            continue;
        const uint8_t* glyph = &kPortFont8x8[(ch - PORT_FONT_FIRST) * 8];
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (glyph[row] & (1 << col))
                    Fill(x + col * scale, y + row * scale, scale, scale, c);
            }
        }
    }
}

static int TextWidth(const char* s, int scale)
{
    return (int)strlen(s) * 8 * scale;
}

static const char* AreaName(int area)
{
    static const char* const names[] = { "CUBIERTA PRINCIPAL", "SECTOR 1 (SRX)", "SECTOR 2 (TRO)", "SECTOR 3 (PYR)",
                                         "SECTOR 4 (AQA)",     "SECTOR 5 (ARC)", "SECTOR 6 (NOC)" };
    return area >= 0 && area < 7 ? names[area] : "";
}

static void Header(const struct PortGameStatus* s, const char* title)
{
    char time[16];

    Fill(0, 0, sW, HEADER_H, COLOR_PANEL);
    Text(4, 3, 1, COLOR_LABEL, title);
    snprintf(time, sizeof(time), "%d:%02d:%02d", s->hours, s->minutes, s->seconds);
    Text(sW - 4 - TextWidth(time, 1), 3, 1, COLOR_DIM, time);
}

// ---- map --------------------------------------------------------------------------

// One 8x8 map tile (4bpp, with flips) scaled to `scale` canvas pixels per pixel.
static void MapTile(const struct PortGameStatus* s, uint16_t entry, int x, int y, int scale)
{
    unsigned tile = entry & 0x3FF;
    const int hflip = (entry >> 10) & 1, vflip = (entry >> 11) & 1;
    unsigned pal = entry >> 12;

    if (tile >= s->mapTileCount)
        return;
    if (pal >= s->mapPaletteCount)
        pal = 0;
    const uint8_t* gfx = s->mapTiles + tile * 32;
    const uint16_t* palette = s->mapPalettes + pal * 16;
    for (int row = 0; row < TILE; row++) {
        const int sy = vflip ? TILE - 1 - row : row;
        for (int col = 0; col < TILE; col++) {
            const int sx = hflip ? TILE - 1 - col : col;
            const uint8_t byte = gfx[sy * 4 + sx / 2];
            const int index = (sx & 1) ? byte >> 4 : byte & 15;
            if (index != 0)
                Fill(x + col * scale, y + row * scale, scale, scale, Bgr555(palette[index]));
        }
    }
}

static void Frame(int x, int y, int w, int h, int t, uint32_t c)
{
    Fill(x, y, w, t, c);
    Fill(x, y + h - t, w, t, c);
    Fill(x, y, t, h, c);
    Fill(x + w - t, y, t, h, c);
}

static void DrawMap(const struct PortGameStatus* s)
{
    char line[48];

    Header(s, AreaName(s->area));
    if (!s->map || s->area < 0) {
        Text((sW - TextWidth("SIN MAPA", 1)) / 2, sH / 2 - 4, 1, COLOR_DIM, "SIN MAPA");
        return;
    }

    // The rooms of the area (plus a cell of margin) as big as they fit.
    const int top = HEADER_H + 2, bottom = sH - FOOTER_H - 2;
    const int areaW = sW - 4, areaH = bottom - top;
    const int x0 = MaxI(s->mapMinX - 1, 0), y0 = MaxI(s->mapMinY - 1, 0);
    const int x1 = MinI(s->mapMaxX + 1, PORT_MAP_SIZE - 1), y1 = MinI(s->mapMaxY + 1, PORT_MAP_SIZE - 1);
    const int cellsW = x1 - x0 + 1, cellsH = y1 - y0 + 1;
    int scale = MinI(areaW / (cellsW * TILE), areaH / (cellsH * TILE));
    int viewX = x0, viewY = y0, viewW = cellsW, viewH = cellsH;

    if (scale < 1) {
        // Too big: show the part around Samus.
        scale = 1;
        viewW = MinI(cellsW, areaW / TILE);
        viewH = MinI(cellsH, areaH / TILE);
        viewX = ClampI(s->mapX - viewW / 2, x0, x1 + 1 - viewW);
        viewY = ClampI(s->mapY - viewH / 2, y0, y1 + 1 - viewH);
    }
    const int cell = TILE * scale;
    const int ox = (sW - viewW * cell) / 2, oy = top + (areaH - viewH * cell) / 2;

    for (int cy = 0; cy < viewH; cy++) {
        for (int cx = 0; cx < viewW; cx++)
            MapTile(s, s->map[(viewY + cy) * PORT_MAP_SIZE + viewX + cx], ox + cx * cell, oy + cy * cell, scale);
    }

    // Samus: a blinking frame on her cell, like the minimap's.
    if ((Port_FrameCount() / 16) % 3 != 2 && s->mapX >= viewX && s->mapX < viewX + viewW && s->mapY >= viewY &&
        s->mapY < viewY + viewH) {
        const int x = ox + (s->mapX - viewX) * cell, y = oy + (s->mapY - viewY) * cell;
        const int t = MaxI(1, scale);
        Frame(x - t, y - t, cell + 2 * t, cell + 2 * t, t, Rgb(255, 255, 255));
        Fill(x + cell / 2 - t, y + cell / 2 - t, 2 * t, 2 * t, Rgb(255, 80, 120));
    }

    // Footer: energy and ammo.
    Fill(0, sH - FOOTER_H, sW, FOOTER_H, COLOR_PANEL);
    snprintf(line, sizeof(line), "EN %02d", s->energy % 100);
    Text(4, sH - FOOTER_H + 3, 1, COLOR_ENERGY, line);
    int x = 4 + TextWidth(line, 1) + 4;
    for (int i = 0; i < s->maxEnergy / 100 && i < 20; i++, x += 5)
        Fill(x, sH - FOOTER_H + 4, 4, 6, i < s->energy / 100 ? COLOR_TANK : COLOR_OFF);
    snprintf(line, sizeof(line), "M%d P%d", s->missiles, s->powerBombs);
    Text(sW - 4 - TextWidth(line, 1), sH - FOOTER_H + 3, 1, COLOR_TEXT, line);
}

// ---- status -----------------------------------------------------------------------

static void Ammo(int x, int y, int w, const char* label, int value, int max)
{
    char num[16];

    Text(x, y, 1, max > 0 ? COLOR_LABEL : COLOR_OFF, label);
    if (max > 0)
        snprintf(num, sizeof(num), "%d/%d", value, max);
    else
        snprintf(num, sizeof(num), "---");
    Text(x + w - TextWidth(num, 1), y, 1, max > 0 ? COLOR_TEXT : COLOR_OFF, num);
}

static void DrawStatus(const struct PortGameStatus* s)
{
    static const struct {
        unsigned item;
        const char* name;
    } items[] = {
        { PORT_ITEM_CHARGE_BEAM, "RAYO CARGADO" },   { PORT_ITEM_MORPH_BALL, "MORFOSFERA" },
        { PORT_ITEM_WIDE_BEAM, "RAYO AMPLIO" },      { PORT_ITEM_HIGH_JUMP, "SALTO ALTO" },
        { PORT_ITEM_PLASMA_BEAM, "RAYO PLASMA" },    { PORT_ITEM_SPEED_BOOSTER, "ACELERADOR" },
        { PORT_ITEM_WAVE_BEAM, "RAYO ONDA" },        { PORT_ITEM_SPACE_JUMP, "SALTO ESPACIAL" },
        { PORT_ITEM_ICE_BEAM, "RAYO HIELO" },        { PORT_ITEM_SCREW_ATTACK, "ATAQUE GIRAT." },
        { PORT_ITEM_MISSILES, "MISILES" },           { PORT_ITEM_VARIA_SUIT, "TRAJE VARIA" },
        { PORT_ITEM_SUPER_MISSILES, "SUPERMISILES" }, { PORT_ITEM_GRAVITY_SUIT, "TRAJE GRAVEDAD" },
        { PORT_ITEM_ICE_MISSILES, "MISIL HIELO" },   { PORT_ITEM_BOMBS, "BOMBAS" },
        { PORT_ITEM_DIFFUSION_MISSILES, "MISIL DIFUSOR" }, { PORT_ITEM_POWER_BOMBS, "BOMBAS PODER" },
    };
    char num[16];

    Header(s, "SAMUS ARAN");
    if (s->area < 0 && s->maxEnergy == 0)
        return;

    // Content is 182 pixels tall; extra height goes above and below.
    const int left = (sW - 248) / 2 + 4, width = 240;
    int y = HEADER_H + MaxI(4, (sH - HEADER_H - 182) / 2);

    Text(left, y + 4, 1, COLOR_LABEL, "ENERGIA");
    snprintf(num, sizeof(num), "%02d", s->energy % 100);
    Text(left + 64, y, 2, COLOR_ENERGY, num);
    {
        const int tanks = MinI(s->maxEnergy / 100, 20);
        int x = left + 104;
        for (int i = 0; i < tanks; i++) {
            const int row = i / 10, col = i % 10;
            Fill(x + col * 13, y + row * 9, 11, 7, i < s->energy / 100 ? COLOR_TANK : COLOR_OFF);
        }
    }
    y += 26;

    Ammo(left, y, width, "MISILES", s->missiles, s->maxMissiles);
    y += 12;
    Ammo(left, y, width, "BOMBAS DE PODER", s->powerBombs, s->maxPowerBombs);
    y += 18;

    Fill(left, y, width, 1, COLOR_OFF);
    y += 6;
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        const int x = left + (int)(i % 2) * (width / 2), row = y + (int)(i / 2) * 12;
        const bool have = (s->items & items[i].item) != 0;
        const bool active = (s->itemsActive & items[i].item) != 0;
        // Collected and on / collected but switched off / not found yet.
        const uint32_t c = have ? (active ? COLOR_TEXT : COLOR_DIM) : COLOR_OFF;
        Fill(x, row + 1, 5, 5, have ? (active ? COLOR_ENERGY : COLOR_DIM) : COLOR_OFF);
        Text(x + 9, row, 1, c, have ? items[i].name : "???");
    }
}

// ---- interface --------------------------------------------------------------------

void Port_SecondScreenSize(int displayW, int displayH, int* width, int* height)
{
    int h = displayW > 0 ? (int)((int64_t)256 * displayH / displayW) : 224;
    *width = 256;
    *height = h < 192 ? 192 : h > 448 ? 448 : h;
}

void Port_SecondScreenRender(uint32_t* pixels, int width, int height, int mode)
{
    const struct PortGameStatus* s = Port_ExtrasStatus();

    sPx = pixels;
    sW = width;
    sH = height;
    Fill(0, 0, width, height, COLOR_BG);
    if (!s->playing && !s->paused && s->area < 0) {
        // Title, menus, cutscenes...
        const char* t = "METROID";
        Text((width - TextWidth(t, 3)) / 2, height / 2 - 20, 3, Rgb(200, 60, 80), t);
        t = "FUSION";
        Text((width - TextWidth(t, 1)) / 2, height / 2 + 10, 1, COLOR_DIM, t);
        return;
    }
    if (mode == PORT_SCREEN2_STATUS)
        DrawStatus(s);
    else
        DrawMap(s);
}

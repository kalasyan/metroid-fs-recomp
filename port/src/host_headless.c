// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — headless host: runs the game without a window, as fast as
// possible, for automated tests and CI.
//
//   mf_headless [--rom FILE] [--frames N] [--dump-every K] [--out DIR] [--save FILE]
//                [--press FRAME:KEYS:LEN ...] [--wav FILE]
//                [--state-save FRAME:FILE] [--state-load FRAME:FILE] [--width W]
//                [--screen2 map|status]
//
// Frames are written as PPM images (no dependencies). KEYS is a combination
// of the letters A B s(elect) S(tart) R(ight) L(eft) U(p) D(own) r l.
// --wav records the sound output (16-bit stereo, 48 kHz).
// --state-save / --state-load write or restore a save state after FRAME.
// --screen2 also dumps the second screen (screen2_NNNNNN.ppm) with each frame.
// MF_RUMBLE_LOG=1 logs the rumble events (port/src/extras.c).
// --width renders a wider picture (widescreen option, e.g. 284 = 16:9).
#include "port.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

typedef struct Press {
    uint64_t frame;
    uint64_t length;
    uint16_t keys;
} Press;

static uint64_t sMaxFrames = 600;
static uint64_t sDumpEvery = 60;
static const char* sOutDir = "frames";
static const char* sSavePath = "mf_headless.sav";
static const char* sRomPath = "mf_us.gba";
static const char* sWavPath;
static FILE* sWav;
static uint32_t sWavFrames;
static Press sPresses[1024];
static int sPressCount;

typedef struct StateOp {
    uint64_t frame;
    const char* path;
    bool load;
} StateOp;
static StateOp sStateOps[8];
static int sStateOpCount;
static int sScreen2 = PORT_SCREEN2_OFF;

static uint16_t ParseKeys(const char* s)
{
    uint16_t keys = 0;
    for (; *s && *s != ':'; s++) {
        switch (*s) {
        case 'A': keys |= PORT_KEY_A; break;
        case 'B': keys |= PORT_KEY_B; break;
        case 's': keys |= PORT_KEY_SELECT; break;
        case 'S': keys |= PORT_KEY_START; break;
        case 'R': keys |= PORT_KEY_RIGHT; break;
        case 'L': keys |= PORT_KEY_LEFT; break;
        case 'U': keys |= PORT_KEY_UP; break;
        case 'D': keys |= PORT_KEY_DOWN; break;
        case 'r': keys |= PORT_KEY_R; break;
        case 'l': keys |= PORT_KEY_L; break;
        default: break;
        }
    }
    return keys;
}

bool Port_HostInit(const PortHostConfig* cfg)
{
    for (int i = 1; i < cfg->argc; i++) {
        const char* a = cfg->argv[i];
        const char* v = (i + 1 < cfg->argc) ? cfg->argv[i + 1] : NULL;
        if (strcmp(a, "--frames") == 0 && v) {
            sMaxFrames = strtoull(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--dump-every") == 0 && v) {
            sDumpEvery = strtoull(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--out") == 0 && v) {
            sOutDir = v;
            i++;
        } else if (strcmp(a, "--rom") == 0 && v) {
            sRomPath = v;
            i++;
        } else if (strcmp(a, "--save") == 0 && v) {
            sSavePath = v;
            i++;
        } else if (strcmp(a, "--width") == 0 && v) {
            Port_WideSetWidth(atoi(v));
            i++;
        } else if (strcmp(a, "--wav") == 0 && v) {
            sWavPath = v;
            i++;
        } else if (strcmp(a, "--press") == 0 && v && sPressCount < (int)(sizeof(sPresses) / sizeof(sPresses[0]))) {
            // FRAME:KEYS:LEN
            Press* p = &sPresses[sPressCount++];
            char* end;
            p->frame = strtoull(v, &end, 10);
            p->keys = (*end == ':') ? ParseKeys(end + 1) : 0;
            const char* len = strrchr(v, ':');
            p->length = (len && len != end) ? strtoull(len + 1, NULL, 10) : 1;
            i++;
        } else if (strcmp(a, "--screen2") == 0 && v) {
            sScreen2 = strcmp(v, "status") == 0 ? PORT_SCREEN2_STATUS : PORT_SCREEN2_MAP;
            i++;
        } else if ((strcmp(a, "--state-save") == 0 || strcmp(a, "--state-load") == 0) && v && sStateOpCount < 8) {
            StateOp* op = &sStateOps[sStateOpCount++];
            char* end;
            op->frame = strtoull(v, &end, 10);
            op->path = (*end == ':') ? end + 1 : "";
            op->load = strcmp(a, "--state-load") == 0;
            i++;
        } else {
            Port_Log("unknown argument: %s", a);
            return false;
        }
    }

    if (sDumpEvery != 0 && mkdir(sOutDir, 0755) != 0 && errno != EEXIST) {
        Port_Log("can't create %s", sOutDir);
        return false;
    }
    if (sWavPath) {
        sWav = fopen(sWavPath, "wb");
        if (!sWav) {
            Port_Log("can't create %s", sWavPath);
            return false;
        }
        fseek(sWav, 44, SEEK_SET); // header written at shutdown
    }
    Port_Log("headless: %llu frames, dump every %llu to %s/", (unsigned long long)sMaxFrames,
             (unsigned long long)sDumpEvery, sOutDir);
    return true;
}

static void Put32(unsigned char* p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void FinishWav(void)
{
    unsigned char h[44] = "RIFF....WAVEfmt ";
    const uint32_t bytes = sWavFrames * 4;

    Put32(h + 4, 36 + bytes);
    Put32(h + 16, 16);
    h[20] = 1; // PCM
    h[22] = 2; // stereo
    Put32(h + 24, PORT_AUDIO_RATE);
    Put32(h + 28, PORT_AUDIO_RATE * 4);
    h[32] = 4;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    Put32(h + 40, bytes);
    fseek(sWav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), sWav);
    fclose(sWav);
    sWav = NULL;
}

void Port_HostShutdown(void)
{
    if (sWav)
        FinishWav();
    Port_Log("headless: stopped after %llu frames", (unsigned long long)Port_FrameCount());
}

static void WritePpm(const uint32_t* abgr, int width, int height, uint64_t frame, const char* name)
{
    char path[512];
    FILE* f;

    snprintf(path, sizeof(path), "%s/%s_%06llu.ppm", sOutDir, name, (unsigned long long)frame);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        const uint32_t p = abgr[i];
        const unsigned char rgb[3] = { (unsigned char)(p & 0xFF), (unsigned char)((p >> 8) & 0xFF),
                                       (unsigned char)((p >> 16) & 0xFF) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

bool Port_HostPresent(const uint32_t* abgr, int width, int height)
{
    const uint64_t frame = Port_FrameCount();

    if (sDumpEvery != 0 && frame % sDumpEvery == 0) {
        WritePpm(abgr, width, height, frame, "frame");
        if (sScreen2 != PORT_SCREEN2_OFF) {
            static uint32_t screen2[256 * 240];
            Port_SecondScreenRender(screen2, 256, 240, sScreen2);
            WritePpm(screen2, 256, 240, frame, "screen2");
        }
    }
    return frame + 1 < sMaxFrames;
}

uint16_t Port_HostPollKeys(void)
{
    const uint64_t frame = Port_FrameCount() + 1;
    uint16_t keys = 0;

    for (int i = 0; i < sPressCount; i++) {
        if (frame >= sPresses[i].frame && frame < sPresses[i].frame + sPresses[i].length)
            keys |= sPresses[i].keys;
    }
    return keys;
}

void Port_HostQueueAudio(const int16_t* samples, int frames)
{
    if (!sWav)
        return;
    // WAV is little-endian, like every host this builds for.
    fwrite(samples, sizeof(int16_t) * 2, (size_t)frames, sWav);
    sWavFrames += (uint32_t)frames;
}

void Port_HostWaitFrame(void)
{
}

void Port_HostRumble(float low, float high, float pulse, int pulseMs)
{
    // Nothing to shake; MF_RUMBLE_LOG=1 shows the events.
    (void)low;
    (void)high;
    (void)pulse;
    (void)pulseMs;
}

void Port_HostFrameBoundary(void)
{
    for (int i = 0; i < sStateOpCount; i++) {
        const StateOp* op = &sStateOps[i];
        if (op->frame != Port_FrameCount())
            continue;
        const size_t size = Port_StateSize();
        uint8_t* buf = malloc(size);
        FILE* f = fopen(op->path, op->load ? "rb" : "wb");
        if (!buf || !f) {
            Port_Log("state: can't open %s", op->path);
        } else if (op->load) {
            const size_t got = fread(buf, 1, size, f);
            const PortStateResult r = Port_StateLoad(buf, got);
            Port_Log("state: loaded %s at frame %llu: %s", op->path, (unsigned long long)op->frame,
                     Port_StateResultText(r));
        } else {
            Port_StateSave(buf);
            fwrite(buf, 1, size, f);
            Port_Log("state: saved %s at frame %llu", op->path, (unsigned long long)op->frame);
        }
        if (f)
            fclose(f);
        free(buf);
    }
}

uint8_t* Port_HostLoadRom(size_t* size, const char* error)
{
    FILE* f;
    long len;
    uint8_t* data;

    if (error) // no one to ask for another file
        return NULL;
    f = fopen(sRomPath, "rb");
    if (!f) {
        Port_Log("can't open ROM %s (use --rom FILE)", sRomPath);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(len > 0 ? (size_t)len : 1);
    if (!data || fread(data, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    *size = (size_t)len;
    return data;
}

void Port_HostRomAccepted(const uint8_t* rom, size_t size)
{
    (void)rom;
    (void)size;
}

void Port_HostLoadSave(uint8_t* data, size_t size)
{
    FILE* f = fopen(sSavePath, "rb");
    if (!f)
        return;
    if (fread(data, 1, size, f) != size)
        Port_Log("save file %s is short", sSavePath);
    fclose(f);
}

void Port_HostWriteSave(const uint8_t* data, size_t size)
{
    FILE* f = fopen(sSavePath, "wb");
    if (!f)
        return;
    fwrite(data, 1, size, f);
    fclose(f);
}

int main(int argc, char** argv)
{
    return Port_Main(argc, argv);
}

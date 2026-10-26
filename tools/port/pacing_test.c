// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — test for port/src/pacing.c.
//
// Checks the display-lock decision and simulates ten minutes of sound with the
// game locked to several refresh rates: the game queues its 48 kHz samples
// once per frame (exactly as port/src/apu.c makes them), the sound device
// pulls fixed-size chunks with a clock slightly off from 48 kHz, and the host
// applies PortPacing_AudioRatio as host_sdl.c does. The queue must never run
// dry (a click) nor reach the 100 ms cap where the host drops sound (another
// click). The same run with a fixed ratio shows why the correction is needed.
#include "port_pacing.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RATE 48000.0
#define CAP (RATE / 10) // host_sdl.c drops new sound above ~100 ms queued

static int sFailures;

static void Check(bool ok, const char* what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        sFailures++;
    }
}

static void TestLock(void)
{
    static const struct {
        double hz;
        bool lock;
        int divisor;
    } cases[] = {
        { 60.0, true, 1 },     { 59.94, true, 1 },    { 59.7275, true, 1 }, { 120.0, true, 2 },
        { 119.88, true, 2 },   { 240.0, true, 4 },    { 180.0, true, 3 },   { 50.0, false, 0 },
        { 75.0, false, 0 },    { 90.0, false, 0 },    { 144.0, false, 0 },  { 165.0, false, 0 },
        { 0.0, false, 0 },     { -1.0, false, 0 },    { 61.0, false, 0 },   { 58.0, false, 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int divisor = 0;
        double speed = 0;
        const bool lock = PortPacing_LockToDisplay(cases[i].hz, &divisor, &speed);
        char what[96];
        snprintf(what, sizeof(what), "lock decision at %.3f Hz", cases[i].hz);
        Check(lock == cases[i].lock && (!lock || divisor == cases[i].divisor), what);
        if (lock)
            Check(fabs(speed * divisor * PORT_GBA_FRAME_RATE - cases[i].hz) < 1e-9, "speed matches the display");
    }
}

static void TestBlend(void)
{
    Check(PortPacing_BlendAlpha(100, 100, 1000) == 0.0f, "blend starts at the previous frame");
    Check(PortPacing_BlendAlpha(600, 100, 1000) == 0.5f, "blend halfway");
    Check(PortPacing_BlendAlpha(5000, 100, 1000) == 1.0f, "blend ends at the newest frame");
    Check(PortPacing_BlendAlpha(50, 100, 1000) == 0.0f, "blend before the frame");
    Check(PortPacing_BlendAlpha(50, 100, 0) == 0.0f, "blend without a period");
}

typedef struct SimResult {
    int underruns, drops;
    double minQueue, maxQueue, meanRatio;
} SimResult;

// `deviceDrift`: the sound card's real clock relative to 48 kHz.
static SimResult Simulate(double refreshHz, double deviceDrift, int chunk, bool control, double seconds)
{
    SimResult r = { 0, 0, 1e9, 0, 0 };
    int divisor = 1;
    double speed = 1.0;
    const bool locked = PortPacing_LockToDisplay(refreshHz, &divisor, &speed);
    const double framePeriod = locked ? divisor / refreshHz : 1.0 / PORT_GBA_FRAME_RATE;
    const double chunkPeriod = chunk / (RATE * (1.0 + deviceDrift));
    const double target = fmax(RATE * 0.04, 2.0 * chunk + RATE / PORT_GBA_FRAME_RATE);
    double queue = 0, smoothed = 0, carry = 0, ratio = control ? speed : 1.0;
    double nextFrame = 0, nextPull = 0.05; // the device starts a bit later
    double ratioSum = 0;
    long ratioCount = 0;
    unsigned seed = 1;
    long frameIndex = 0;

    while (nextFrame < seconds) {
        if (nextFrame <= nextPull) {
            // One game frame: 803 or 804 samples, as in apu.c.
            int out;
            carry += RATE / PORT_GBA_FRAME_RATE;
            out = (int)carry;
            carry -= out;
            if (queue > CAP)
                r.drops++;
            else
                queue += out;
            smoothed += (queue - smoothed) * 0.05;
            if (control)
                ratio = PortPacing_AudioRatio(speed, smoothed, target);
            if (nextFrame > 2.0) {
                ratioSum += ratio;
                ratioCount++;
            }
            // Up to +-1 ms of jitter on when each frame is handed over.
            seed = seed * 1103515245u + 12345u;
            frameIndex++;
            nextFrame = frameIndex * framePeriod + ((double)(seed >> 16 & 0x7FFF) / 0x7FFF - 0.5) * 0.002;
        } else {
            const double need = chunk * ratio;
            if (nextPull > 2.0) {
                if (queue < need)
                    r.underruns++;
                if (queue < r.minQueue)
                    r.minQueue = queue;
                if (queue > r.maxQueue)
                    r.maxQueue = queue;
            }
            queue = queue > need ? queue - need : 0;
            nextPull += chunkPeriod;
        }
    }
    r.meanRatio = ratioCount ? ratioSum / ratioCount : 0;
    return r;
}

static void TestAudio(void)
{
    static const double rates[] = { 60.0, 59.94, 120.0, 59.7275 };
    static const double drifts[] = { 0.0, 0.001, -0.001 };
    static const int chunks[] = { 256, 1024 };

    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        for (size_t d = 0; d < sizeof(drifts) / sizeof(drifts[0]); d++) {
            for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
                const SimResult on = Simulate(rates[i], drifts[d], chunks[c], true, 600.0);
                char what[128];
                printf("%7.3f Hz, device %+.1f %%, chunk %4d: underruns %d, drops %d, queue %5.0f..%5.0f, "
                       "ratio %.5f\n",
                       rates[i], drifts[d] * 100, chunks[c], on.underruns, on.drops, on.minQueue, on.maxQueue,
                       on.meanRatio);
                snprintf(what, sizeof(what), "sound stays continuous at %.3f Hz (drift %+.1f %%, chunk %d)",
                         rates[i], drifts[d] * 100, chunks[c]);
                Check(on.underruns == 0 && on.drops == 0, what);
            }
        }
    }

    // Without the correction a 60 Hz lock piles up sound and drops it.
    const SimResult off = Simulate(60.0, 0.0, 1024, false, 600.0);
    printf("60 Hz without correction: underruns %d, drops %d\n", off.underruns, off.drops);
    Check(off.drops > 0, "without correction the queue overflows (sanity check of the simulation)");
}

int main(void)
{
    TestLock();
    TestBlend();
    TestAudio();
    if (sFailures) {
        printf("%d failure(s)\n", sFailures);
        return 1;
    }
    printf("pacing: all tests passed\n");
    return 0;
}

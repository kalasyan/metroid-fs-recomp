// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — frame pacing helpers (port/src/pacing.c).
//
// The GBA draws 59.7275 frames per second; most screens refresh at 60 Hz (or
// 120, 144...). Two optional ways to make that look smooth, both host-side
// only (the game logic, its frames and its sound samples do not change):
//
// - Lock to the display: when the refresh rate is a whole multiple of ~59.73 Hz
//   (within 1 %: 59.94, 60, 120, 180, 240 Hz...), run one game frame every
//   `divisor` refreshes, paced by VSync, and play the sound that much faster
//   (+0.46 % at 60 Hz) with a small dynamic correction that keeps the audio
//   queue at a fixed level, so it never runs dry or piles up.
// - Frame blending: on other refresh rates (90, 144, 165 Hz...) every refresh
//   shows a mix of the last two game frames weighted by time, which hides the
//   uneven 2-3-2-3 repeat pattern.
//
// Plain C, no SDL: the unit test (tools/port/pacing_test.c) runs it headless.
#ifndef PORT_PACING_H
#define PORT_PACING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_GBA_FRAME_RATE (16777216.0 / 280896.0) // 59.7275 Hz
#define PORT_GBA_FRAME_NS 16742706ull               // 280896 cycles / 16.777216 MHz

// Largest speed change accepted to lock the game to the display (1 %).
#define PORT_PACING_MAX_SPEEDUP 0.01
// Largest extra correction of the sound speed to keep its queue level (0.5 %).
#define PORT_PACING_MAX_AUDIO_CORRECTION 0.005

// Can the game be locked to a display refreshing at `refreshHz`? If so, it
// runs one frame every `*divisor` refreshes at `*speed` times the GBA's speed.
bool PortPacing_LockToDisplay(double refreshHz, int* divisor, double* speed);

// Dynamic rate control for the sound while locked: playback ratio to give the
// audio stream, for a game running at `speed`. `queued` is the (smoothed)
// amount of sound waiting to be played and `target` the level to keep, in the
// same unit. Above the target the sound plays a bit faster, below it a bit
// slower, by at most PORT_PACING_MAX_AUDIO_CORRECTION.
double PortPacing_AudioRatio(double speed, double queued, double target);

// Frame blending: how much of the newest game frame (0..1, the rest is the
// previous one) to show at `displayNs`, a frame that started showing at
// `frameNs`. Game frames last `periodNs`.
float PortPacing_BlendAlpha(uint64_t displayNs, uint64_t frameNs, uint64_t periodNs);

#ifdef __cplusplus
}
#endif

#endif // PORT_PACING_H

// SPDX-License-Identifier: GPL-3.0-or-later
// mf-android — frame pacing helpers (see port_pacing.h).
#include "port_pacing.h"

#include <math.h>

bool PortPacing_LockToDisplay(double refreshHz, int* divisor, double* speed)
{
    if (!(refreshHz >= PORT_GBA_FRAME_RATE * (1.0 - PORT_PACING_MAX_SPEEDUP)) || refreshHz > 1000.0)
        return false;
    const int n = (int)floor(refreshHz / PORT_GBA_FRAME_RATE + 0.5);
    const double s = refreshHz / n / PORT_GBA_FRAME_RATE;
    if (n < 1 || fabs(s - 1.0) > PORT_PACING_MAX_SPEEDUP)
        return false;
    *divisor = n;
    *speed = s;
    return true;
}

double PortPacing_AudioRatio(double speed, double queued, double target)
{
    double error;

    if (!(target > 0.0))
        return speed;
    error = (queued - target) / target;
    if (error > 1.0)
        error = 1.0;
    else if (error < -1.0)
        error = -1.0;
    return speed * (1.0 + PORT_PACING_MAX_AUDIO_CORRECTION * error);
}

float PortPacing_BlendAlpha(uint64_t displayNs, uint64_t frameNs, uint64_t periodNs)
{
    if (periodNs == 0 || displayNs <= frameNs)
        return 0.0f;
    const uint64_t elapsed = displayNs - frameNs;
    if (elapsed >= periodNs)
        return 1.0f;
    return (float)((double)elapsed / (double)periodNs);
}

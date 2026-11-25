// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SHENG_SCREEN_BLANK_H
#define SHENG_SCREEN_BLANK_H

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#define SHENG_POWER_HOLD_MS 1200u
#define SHENG_POWER_FORCE_HALT_MS 4500u

enum screen_power_action {
    ScreenPowerPassThrough = 0,
    ScreenPowerSwallowWait = 1,
    ScreenPowerSwallowIdle = 2,
    ScreenPowerToggleBlank = 3,
    ScreenPowerForwardHold = 4,
    ScreenPowerForceHalt = 5,
};

struct screen_blank_power {
    bool held;
    bool forwarded;
    bool force_halt_fired;
    uint64_t down_ms;
};

#define SCREEN_BLANK_POWER_INIT {false, false, false, 0}

static inline enum screen_power_action screen_blank_power_step(
    struct screen_blank_power *s,
    bool power_pressed,
    uint64_t now_ms,
    bool lid_closed,
    bool platform_sleep,
    uint32_t *wait_ms_out)
{
    if (wait_ms_out) *wait_ms_out = 0;
    if (platform_sleep) {
        s->held = false;
        s->forwarded = false;
        s->force_halt_fired = false;
        return ScreenPowerPassThrough;
    }
    if (power_pressed) {
        if (!s->held) {
            s->held = true;
            s->forwarded = false;
            s->force_halt_fired = false;
            s->down_ms = now_ms;
            if (wait_ms_out) *wait_ms_out = SHENG_POWER_HOLD_MS;
            return ScreenPowerSwallowWait;
        }
        const uint64_t elapsed = now_ms >= s->down_ms ? (now_ms - s->down_ms) : 0;
        if (s->forwarded) {
            if (!s->force_halt_fired) {
                if (elapsed >= SHENG_POWER_FORCE_HALT_MS) {
                    s->force_halt_fired = true;
                    return ScreenPowerForceHalt;
                }
                if (wait_ms_out) *wait_ms_out = (uint32_t)(SHENG_POWER_FORCE_HALT_MS - elapsed);
            }
            return ScreenPowerPassThrough;
        }
        if (elapsed >= SHENG_POWER_HOLD_MS) {
            s->forwarded = true;
            if (wait_ms_out) {
                *wait_ms_out = elapsed < SHENG_POWER_FORCE_HALT_MS
                    ? (uint32_t)(SHENG_POWER_FORCE_HALT_MS - elapsed)
                    : 1u;
            }
            return ScreenPowerForwardHold;
        }
        if (wait_ms_out) *wait_ms_out = (uint32_t)(SHENG_POWER_HOLD_MS - elapsed);
        return ScreenPowerSwallowWait;
    }
    if (s->held) {
        const bool was_forwarded = s->forwarded;
        s->held = false;
        s->forwarded = false;
        s->force_halt_fired = false;
        if (was_forwarded) {
            return ScreenPowerPassThrough;
        }
        return lid_closed ? ScreenPowerSwallowIdle : ScreenPowerToggleBlank;
    }
    return ScreenPowerSwallowIdle;
}

#endif // SHENG_SCREEN_BLANK_H

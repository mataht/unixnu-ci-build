#include <assert.h>
#include <stdio.h>
#include "../ScreenBlank.h"

int main(void)
{
    struct screen_blank_power st = SCREEN_BLANK_POWER_INIT;
    uint32_t wait_ms = 0;

    assert(screen_blank_power_step(&st, true, 1000, false, false, &wait_ms) == ScreenPowerSwallowWait);
    assert(wait_ms == SHENG_POWER_HOLD_MS);
    assert(screen_blank_power_step(&st, true, 1100, false, false, &wait_ms) == ScreenPowerSwallowWait);
    assert(wait_ms == SHENG_POWER_HOLD_MS - 100);
    assert(screen_blank_power_step(&st, false, 1180, false, false, &wait_ms) == ScreenPowerToggleBlank);
    assert(wait_ms == 0);
    assert(screen_blank_power_step(&st, false, 1300, false, false, &wait_ms) == ScreenPowerSwallowIdle);

    assert(screen_blank_power_step(&st, true, 2000, true, false, &wait_ms) == ScreenPowerSwallowWait);
    assert(screen_blank_power_step(&st, false, 2200, true, false, &wait_ms) == ScreenPowerSwallowIdle);

    assert(screen_blank_power_step(&st, true, 5000, false, false, &wait_ms) == ScreenPowerSwallowWait);
    assert(screen_blank_power_step(&st, true, 6199, false, false, &wait_ms) == ScreenPowerSwallowWait);
    assert(wait_ms == 1);
    assert(screen_blank_power_step(&st, true, 6200, false, false, &wait_ms) == ScreenPowerForwardHold);
    assert(wait_ms == SHENG_POWER_FORCE_HALT_MS - SHENG_POWER_HOLD_MS);
    assert(screen_blank_power_step(&st, true, 6500, false, false, &wait_ms) == ScreenPowerPassThrough);
    assert(wait_ms == SHENG_POWER_FORCE_HALT_MS - 1500);
    assert(screen_blank_power_step(&st, false, 6800, false, false, &wait_ms) == ScreenPowerPassThrough);
    assert(wait_ms == 0);
    assert(screen_blank_power_step(&st, false, 6900, false, false, &wait_ms) == ScreenPowerSwallowIdle);

    assert(screen_blank_power_step(&st, true, 10000, false, false, &wait_ms) == ScreenPowerSwallowWait);
    assert(screen_blank_power_step(&st, true, 11200, false, false, &wait_ms) == ScreenPowerForwardHold);
    assert(wait_ms == 3300);
    assert(screen_blank_power_step(&st, true, 14499, false, false, &wait_ms) == ScreenPowerPassThrough);
    assert(wait_ms == 1);
    assert(screen_blank_power_step(&st, true, 14500, false, false, &wait_ms) == ScreenPowerForceHalt);
    assert(wait_ms == 0);
    assert(screen_blank_power_step(&st, true, 15000, false, false, &wait_ms) == ScreenPowerPassThrough);
    assert(wait_ms == 0);
    assert(screen_blank_power_step(&st, false, 15100, false, false, &wait_ms) == ScreenPowerPassThrough);

    assert(screen_blank_power_step(&st, true, 8000, false, true, &wait_ms) == ScreenPowerPassThrough);
    assert(screen_blank_power_step(&st, false, 8050, false, true, &wait_ms) == ScreenPowerPassThrough);

    printf("screen_blank_host: ok\n");
    return 0;
}

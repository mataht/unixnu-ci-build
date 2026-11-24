// SPDX-License-Identifier: GPL-3.0-or-later
// PSCI SYSTEM_OFF (function 0x84000008) through the SMC conduit, as BootReset.c and ArmDTPlatform.cpp call SYSTEM_RESET. Only
// ThermalCap.cpp calls it, in mode 3: at level 5, 20 s after a level 4 halt that has not finished, or at the end of that halt.
// It does not return; if the firmware refuses the call, the CPU waits here with interrupts as they were.
#include <stdint.h>

void sheng_thermal_cap_power_off(void) __attribute__((noreturn));
void sheng_thermal_cap_power_off(void)
{
    register uint64_t x0 __asm__("x0") = 0x84000008;
    register uint64_t x1 __asm__("x1") = 0, x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                     "x12", "x13", "x14", "x15", "x16", "x17");
    for (;;) __asm__ volatile("wfe");
}

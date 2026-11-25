/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the two frequency governors read from the thermal caps (ThermalCap.cpp, thermal-cap=2 or 3), and the two helpers they apply it with.
 * C and C++; no register, no clock.
 *   sheng_thermal_cpu_percent[d]  CpuFreqProbe.cpp (cpufreq=4): domain d's highest level is the highest at or below that share of its top
 *                                 non-turbo frequency (100 = no cap)
 *   sheng_thermal_gpu_level       GpuPowerProbe.cpp (gpu-dcvs=1): the highest GPU level it may vote (8 = no cap)
 * Both are defined in CpuFreqProbe.cpp, which every kernel with either governor links (the iPadOS and the macOS 27 board lists), with no
 * cap; only ThermalCap.cpp writes them, and a kernel without it never changes them. */
#ifndef SHENG_THERMAL_CAP_HOOKS_H
#define SHENG_THERMAL_CAP_HOOKS_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern volatile uint32_t sheng_thermal_cpu_percent[3];
extern volatile uint32_t sheng_thermal_gpu_level;
#ifdef __cplusplus
}
#endif

/* Domain d's cap in percent, 100 without a cap. */
static inline unsigned sheng_thermal_cpu_cap(unsigned d)
{
    if (d > 2) return 100;
    const uint32_t percent = sheng_thermal_cpu_percent[d];
    return percent > 100 ? 100 : (unsigned)percent;
}
/* The GPU cap, 8 without a cap. */
static inline unsigned sheng_thermal_gpu_cap(void)
{
    const uint32_t level = sheng_thermal_gpu_level;
    return level < 1 ? 1 : level > 8 ? 8 : (unsigned)level;
}
/* In the ascending khz[count]: the highest index at or below the top times percent / 100, never less than 0. */
static inline unsigned sheng_cpufreq_cap_index(const uint32_t *khz, unsigned count, unsigned percent)
{
    if (!count) return 0;
    if (percent > 100) percent = 100;
    const uint64_t limit = (uint64_t)khz[count - 1] * percent / 100;
    unsigned index = 0;
    for (unsigned i = 0; i < count; ++i)
        if (khz[i] <= limit) index = i;
    return index;
}
/* The GPU level the governor may vote: the configured top (gpu-dcvs-max) and the cap, the lower of the two, within 1..8. */
static inline unsigned sheng_gpu_ceiling(unsigned top, unsigned cap)
{
    const unsigned a = top < 1 ? 1 : top > 8 ? 8 : top, b = cap < 1 ? 1 : cap > 8 ? 8 : cap;
    return a < b ? a : b;
}
#endif

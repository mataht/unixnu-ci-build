// SPDX-License-Identifier: GPL-3.0-or-later
// Thermal caps (ThermalCap.cpp, thermal-cap=<n>): temperatures in, frequency caps and the shutdown action out. Pure logic, no register
// and no clock, so the host tests run it as it is (tests in hw-build/upstream-thermal/tests of the sheng macOS research tree).
//
// The temperatures are the ones ThermalProbe.cpp (thermal=1) already reads: native_thermal_mc[controller][sensor] in millidegrees, and its
// sensor table thermal_group_of (ThermalProbe.h). Nothing here reads TSENS a second time. The register facts behind those values (status
// word at 0xa0 + 4 n, temperature in tenths of a degree in bits 11:0, two's complement, valid bit 21, the three TM and SROT bases) are the
// ones this project checked against the pinned Linux tree for its own macOS 26.5 driver; ThermalProbe.h uses the same values.
//
// The caps only lower what the two governors may choose: CpuFreqProbe.cpp (cpufreq=4) reads sheng_thermal_cpu_percent[3] and stops at
// the highest level at or below that share of each domain's top, GpuPowerProbe.cpp (gpu-dcvs=1) reads sheng_thermal_gpu_level and votes
// no level above it. Each governor stays the only writer of its own registers.
//
// The level table is provisional: Android's GPU junction target on this tablet is 95 C, its CPU emergency trips 108 to 112 C, the hardware
// reset monitor 115 C (the tablet's own device tree). It has not been validated on the device.
#pragma once
#include <stdint.h>
#include "ThermalProbe.h"

namespace sheng_thermal_cap {
constexpr int32_t kNone = THERMAL_NONE;
constexpr uint32_t kTableVersion = 4;
constexpr uint8_t kGpuTop = 0, kGpuTopMinusOne = 255;   // symbolic GPU caps: the configured top level, and one below it
constexpr unsigned kGpuLevels = 8;                       // GpuPowerProbe.cpp kGpuTop: 1 = 220 MHz ... 8 = 680 MHz

struct LevelRow {
    int32_t enterC;        // tjMax at or above this enters the level
    int32_t socEnterC;     // tSoc at or above this enters the level as well; 0 = not used (no calibration of the aoss proxy yet)
    uint8_t cpuPercent[3]; // E, P, X3: cap as a percentage of the domain's highest non-turbo frequency
    uint8_t gpuLevel;      // GPU level cap, or one of the symbolic values above
    uint8_t action;        // 0 none, 1 orderly halt, 2 immediate power-off
};

// TABLE-BEGIN
constexpr LevelRow kLevels[6] = {
    {0,   0,  {100, 100, 100}, kGpuTop, 0},
    {72,  64, { 92,  82,  72}, kGpuTop, 0},
    {78,  68, { 82,  65,  55}, 6,       0},
    {84,  72, { 65,  45,  35}, 4,       0},
    {102, 0,  { 55,  35,  25}, 1,       1},
    {108, 0,  { 55,  35,  25}, 1,       2},
};
// TABLE-END
constexpr unsigned kTopLevel = 5, kHaltLevel = 4, kThrottleWhenBlind = 2;
constexpr uint32_t kHaltRounds = 2;            // level 4 needs this many rounds in a row at or above its entry ...
constexpr uint32_t kPowerOffRounds = 2;        // ... and level 5 as well: the probe takes one valid read per sensor (ThermalProbe.h)
constexpr int32_t kExitMarginMilliC = 5000;    // a level is left only this far below its entry ...
constexpr uint32_t kExitHoldMs = 3000;        // ... for this long
constexpr int32_t kGpuHotMilliC = 80000;       // the GPU alone at or above this caps it as level 2 would
constexpr uint32_t kBlindRounds = 5;           // rounds without a usable reading before the caps hold level 2
constexpr uint32_t kStaleRounds = 3;           // rounds without a new probe sample before its values count as old
constexpr int32_t kMinMilliC = -40000, kMaxMilliC = 150000;   // outside: not a temperature this chip reports, the reading is dropped

// The sensors the level is made from, by the groups of ThermalProbe.h, and two single sensors of its "other" group.
constexpr unsigned kDdrController = 1, kDdrSensor = 9;   // sm8550.dtsi "mem" zone: tsens1 sensor 9
constexpr unsigned kAossSensor = 0;                      // sensor 0 of each controller: aoss-0..2

struct Readings {
    int32_t cpu[3], cpuss, gpu, ddr, aossMin;   // hottest of each group (aoss: the coolest); kNone when the group has no reading
    uint32_t unreadable;                        // sensors of those groups that are enabled but gave no usable reading
    uint32_t disabled;                          // sensors of those groups the controller has not enabled (SROT control word)
};

// Exactly 0 is not taken either: the probe accepts two equal status words without the valid bit (ThermalProbe.h thermal_pick), and a TM
// block that reads all zeros then gives 0.0 C for every sensor, which would hide that group's real temperature.
inline bool usable(int32_t mc) {return mc != kNone && mc != 0 && mc >= kMinMilliC && mc <= kMaxMilliC;}

// From the probe's last sample. ctrl: the SROT control word of each controller (native_thermal_info[6..8]).
inline Readings readingsFromProbe(const int32_t mc[THERMAL_CONTROLLERS][THERMAL_SENSORS], const uint32_t ctrl[THERMAL_CONTROLLERS])
{
    Readings r = {{kNone, kNone, kNone}, kNone, kNone, kNone, kNone, 0, 0};
    auto take = [&r](int32_t &slot, int32_t value, bool enabled) {
        if (!enabled) {++r.disabled; return;}
        if (!usable(value)) {++r.unreadable; return;}
        if (slot == kNone || value > slot) slot = value;
    };
    for (unsigned c = 0; c < THERMAL_CONTROLLERS; ++c)
        for (unsigned s = 0; s < THERMAL_SENSORS; ++s) {
            const bool enabled = thermal_sensor_enabled(ctrl[c], s) != 0;
            const int32_t value = mc[c][s];
            switch (thermal_group_of[c][s]) {
            case THERMAL_GROUP_CPU_D0: take(r.cpu[0], value, enabled); break;
            case THERMAL_GROUP_CPU_D1: take(r.cpu[1], value, enabled); break;
            case THERMAL_GROUP_CPU_D2: take(r.cpu[2], value, enabled); break;
            case THERMAL_GROUP_CPUSS: take(r.cpuss, value, enabled); break;
            case THERMAL_GROUP_GPU: take(r.gpu, value, enabled); break;
            default: break;
            }
        }
    {
        const bool enabled = thermal_sensor_enabled(ctrl[kDdrController], kDdrSensor) != 0;
        take(r.ddr, mc[kDdrController][kDdrSensor], enabled);
    }
    for (unsigned c = 0; c < THERMAL_CONTROLLERS; ++c) {
        if (!thermal_sensor_enabled(ctrl[c], kAossSensor)) {++r.disabled; continue;}
        const int32_t value = mc[c][kAossSensor];
        if (!usable(value)) {++r.unreadable; continue;}
        if (r.aossMin == kNone || value < r.aossMin) r.aossMin = value;
    }
    return r;
}

// The level is made from the CPU domains, the GPU and the memory sensor; cpuss is logged only (the 26.5 driver's set, kept so that the
// numbers of its first hardware runs carry over).
inline int32_t hottest(const Readings &r)
{
    int32_t best = kNone;
    const int32_t values[5] = {r.cpu[0], r.cpu[1], r.cpu[2], r.gpu, r.ddr};
    for (int32_t v : values)
        if (v != kNone && (best == kNone || v > best)) best = v;
    return best;
}

// Blind: a CPU domain or the GPU without any reading. The caller adds a stale or missing probe sample.
inline bool groupsMissing(const Readings &r) {return r.cpu[0] == kNone || r.cpu[1] == kNone || r.cpu[2] == kNone || r.gpu == kNone;}

// Whether the probe's sample count says the values are new. Call once a round; samples is native_thermal_info[1].
struct SampleWatch {
    bool started;
    uint32_t last, same;
};
inline bool sampleFresh(SampleWatch &w, uint32_t samples)
{
    if (w.started && samples == w.last) {
        if (w.same < kStaleRounds) ++w.same;
    } else {
        w.same = 0;
    }
    w.started = true;
    w.last = samples;
    return w.same < kStaleRounds;
}

struct ThermalInput {
    int32_t tjCpu[3];   // hottest sensor of each CPU domain, thousandths of a degree or kNone
    int32_t tjGpu;
    int32_t tjMax;      // hottest(): the CPU domains, the GPU and ddr
    int32_t tSoc;       // SocAverage below; only a stand-in for the skin temperature, not used until calibrated
    bool blind;         // a CPU domain or the GPU without a reading, or the probe's values are old
    int32_t offsetC;    // test: added to every reading before it is judged
    uint8_t gpuTop;     // the configured top GPU level, 1..8
};

struct ThermalCaps {
    uint8_t level;          // 0..5, including the blind floor
    uint8_t cpuPercent[3];
    uint8_t gpuLevel;       // 1..8
    uint8_t action;         // 0 none, 1 orderly halt, 2 immediate power-off
};

struct ThermalState {
    uint8_t level;
    bool below;
    uint32_t belowSinceMs, hotRounds, topRounds, blindRounds;
};

inline int32_t offsetReading(int32_t value, int32_t offsetMilliC) {return value == kNone ? value : value + offsetMilliC;}

inline bool levelReached(unsigned level, int32_t tj, int32_t soc)
{
    const LevelRow &row = kLevels[level];
    if (tj != kNone && tj >= row.enterC * 1000) return true;
    return row.socEnterC != 0 && soc != kNone && soc >= row.socEnterC * 1000;
}

inline uint8_t clampGpuLevel(unsigned level) {return uint8_t(level < 1 ? 1 : level > kGpuLevels ? kGpuLevels : level);}

inline uint8_t gpuCapOfRow(const LevelRow &row, uint8_t top)
{
    if (row.gpuLevel == kGpuTop) return top;
    if (row.gpuLevel == kGpuTopMinusOne) return uint8_t(top > 1 ? top - 1 : 1);
    return row.gpuLevel < top ? row.gpuLevel : top;
}

// One round. Going up happens at once and may skip levels; levels 4 and 5 need two rounds in a row at their entry. Going down is one level
// at a time, only after the temperature has stayed at least 5 C below that level's entry for 10 s. Missing readings never cause a
// shutdown: after five blind rounds the caps hold level 2 or stricter until the readings are back.
inline ThermalCaps thermalStep(ThermalState &state, const ThermalInput &in, uint32_t nowMs)
{
    const int32_t offset = in.offsetC * 1000;
    const int32_t tj = offsetReading(in.tjMax, offset), gpu = offsetReading(in.tjGpu, offset), soc = offsetReading(in.tSoc, offset);
    const bool haveReading = tj != kNone;
    state.blindRounds = in.blind ? state.blindRounds + 1 : 0;
    if (haveReading) {
        state.hotRounds = levelReached(kHaltLevel, tj, soc) ? state.hotRounds + 1 : 0;
        state.topRounds = levelReached(kTopLevel, tj, soc) ? state.topRounds + 1 : 0;
        unsigned target = 0;
        for (unsigned level = kTopLevel; level > 0; --level) {
            if (!levelReached(level, tj, soc)) continue;
            if (level == kTopLevel && state.topRounds < kPowerOffRounds) continue;
            if (level == kHaltLevel && state.hotRounds < kHaltRounds) continue;
            target = level;
            break;
        }
        if (target > state.level) {
            state.level = uint8_t(target);
            state.below = false;
        } else if (state.level > 0) {
            const LevelRow &row = kLevels[state.level];
            const bool socClear = row.socEnterC == 0 || soc == kNone || soc <= row.socEnterC * 1000 - kExitMarginMilliC;
            if (tj <= row.enterC * 1000 - kExitMarginMilliC && socClear) {
                if (!state.below) {
                    state.below = true;
                    state.belowSinceMs = nowMs;
                } else if (nowMs - state.belowSinceMs >= kExitHoldMs) {
                    state.level = uint8_t(state.level - 1);
                    state.below = false;
                }
            } else {
                state.below = false;
            }
        }
    } else {
        state.hotRounds = state.topRounds = 0;
    }
    unsigned effective = state.level;
    if (state.blindRounds >= kBlindRounds && effective < kThrottleWhenBlind) effective = kThrottleWhenBlind;
    const LevelRow &row = kLevels[effective];
    const uint8_t top = clampGpuLevel(in.gpuTop);
    ThermalCaps caps = {};
    caps.level = uint8_t(effective);
    for (unsigned d = 0; d < 3; ++d) caps.cpuPercent[d] = row.cpuPercent[d];
    caps.gpuLevel = gpuCapOfRow(row, top);
    if (gpu != kNone && gpu >= kGpuHotMilliC) {
        const uint8_t hot = gpuCapOfRow(kLevels[2], top);
        if (hot < caps.gpuLevel) caps.gpuLevel = hot;
    }
    caps.action = kLevels[state.level].action;   // never from the blind floor
    return caps;
}

// The 30 s moving average of the coolest aoss sensor, in one-second buckets.
class SocAverage {
public:
    int32_t add(int32_t milliC, uint32_t nowMs)
    {
        if (milliC == kNone) return result();
        const uint32_t second = nowMs / 1000;
        if (!started_ || second != second_) {
            started_ = true;
            second_ = second;
            at_ = (at_ + 1) % kSeconds;
            sum_[at_] = 0;
            count_[at_] = 0;
        }
        sum_[at_] += milliC;
        ++count_[at_];
        return result();
    }

private:
    int32_t result() const
    {
        int64_t total = 0;
        int64_t samples = 0;
        for (unsigned i = 0; i < kSeconds; ++i) {
            total += sum_[i];
            samples += count_[i];
        }
        return samples ? int32_t(total / samples) : kNone;
    }
    static constexpr unsigned kSeconds = 30;
    int64_t sum_[kSeconds] = {};
    uint32_t count_[kSeconds] = {};
    unsigned at_ = 0;
    uint32_t second_ = 0;
    bool started_ = false;
};
}  // namespace sheng_thermal_cap

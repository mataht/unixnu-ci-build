// SPDX-License-Identifier: GPL-3.0-or-later
// The thermal cap log (ThermalCap.cpp): one fixed-format line per logged round, and the 64-line memory ring that holds the lines until the
// Data volume can be written (/private/var/log/sheng-thermal.log). Pure logic, tested on the host.
#pragma once
#include <stdint.h>
#include <string.h>
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif
#include "ThermalProbe.h"

namespace sheng_thermal_cap_log {
constexpr int32_t kNoReading = THERMAL_NONE;
constexpr uint32_t kNone = UINT32_MAX;
constexpr unsigned kLineBytes = 200, kRingLines = 64;

struct Row {
    uint32_t seconds, level;
    int32_t tjMax, tjGpu, tjCpu[3], ddr, cpuss, tSoc;   // real readings, without the test offset
    uint32_t capCpu[3], capGpu;
    uint32_t mhz[3], gpuLevel, gpuBusy, throttled, limitIrq, unreadable;   // kNone: that source is not running
    uint32_t blindRounds;   // rounds in a row without usable readings; the temperatures shown are then the last ones the probe had
    int32_t offsetC;
};

// "<degrees with one decimal>" or "-" without a reading. Returns the space used.
inline unsigned formatTemp(char *out, unsigned size, int32_t milliC)
{
    int n;
    if (milliC == kNoReading) {
        n = snprintf(out, size, "-");
    } else {
        const int sign = milliC < 0;
        const uint32_t magnitude = uint32_t(milliC < 0 ? -int64_t(milliC) : int64_t(milliC));
        n = snprintf(out, size, "%s%u.%u", sign ? "-" : "", magnitude / 1000, (magnitude % 1000) / 100);
    }
    return n < 0 ? 0 : unsigned(n) >= size ? size - 1 : unsigned(n);
}

inline unsigned formatValue(char *out, unsigned size, uint32_t value)
{
    const int n = value == kNone ? snprintf(out, size, "-") : snprintf(out, size, "%u", value);
    return n < 0 ? 0 : unsigned(n) >= size ? size - 1 : unsigned(n);
}

// Fields: t seconds since boot, L level, tj hottest (CPU domains, GPU, ddr), g GPU, c CPU domains, d ddr, x cpuss, s 30 s mean of the
// coolest aoss sensor, cap E/P/X % and GPU level, f mean MHz per domain over the governor's last 10 s, gl GPU level now, gb GPU busy % over
// its last 10 s, thr throttled windows since boot (CPU governor), irq domains whose firmware limit interrupt is pending (bits), inv
// enabled sensors without a usable reading; b only while blind (rounds in a row; the temperatures are then the probe's last ones); off
// only with a test offset. At most 199 characters.
inline unsigned formatLine(char *out, unsigned size, const Row &row)
{
    if (size < kLineBytes) return 0;
    unsigned used = 0;
    auto text = [&](const char *value) {
        const int n = snprintf(out + used, size - used, "%s", value);
        if (n > 0) used = unsigned(n) >= size - used ? size - 1 : used + unsigned(n);
    };
    auto temp = [&](int32_t value) {used += formatTemp(out + used, size - used, value);};
    auto value = [&](uint32_t number) {used += formatValue(out + used, size - used, number);};
    {
        const int n = snprintf(out, size, "t=%u L%u tj=", row.seconds, row.level);
        used = n < 0 ? 0 : unsigned(n);
    }
    temp(row.tjMax); text(" g="); temp(row.tjGpu); text(" c="); temp(row.tjCpu[0]); text("/"); temp(row.tjCpu[1]); text("/"); temp(row.tjCpu[2]);
    text(" d="); temp(row.ddr); text(" x="); temp(row.cpuss); text(" s="); temp(row.tSoc);
    text(" cap="); value(row.capCpu[0]); text("/"); value(row.capCpu[1]); text("/"); value(row.capCpu[2]); text("%/"); value(row.capGpu);
    text(" f="); value(row.mhz[0]); text("/"); value(row.mhz[1]); text("/"); value(row.mhz[2]); text("MHz gl="); value(row.gpuLevel);
    text(" gb="); value(row.gpuBusy); text(" thr="); value(row.throttled); text(" irq="); value(row.limitIrq); text(" inv="); value(row.unreadable);
    if (row.blindRounds) {
        text(" b=");
        value(row.blindRounds);
    }
    if (row.offsetC) {
        text(" off=");
        value(uint32_t(row.offsetC));
    }
    return used;
}

// Oldest lines are dropped when it is full; the count of dropped lines is kept until the next successful write. Callers hold their own lock.
class Ring {
public:
    void push(const char *line)
    {
        if (count_ == kRingLines) {
            head_ = (head_ + 1) % kRingLines;
            --count_;
            ++dropped_;
        }
        char *slot = lines_[(head_ + count_) % kRingLines];
        unsigned i = 0;
        for (; i + 1 < kLineBytes && line[i]; ++i) slot[i] = line[i];
        slot[i] = 0;
        ++count_;
    }
    unsigned count() const {return count_;}
    uint32_t dropped() const {return dropped_;}
    // Copies whole lines, each ended by a newline, oldest first. Returns the number of lines; bytes in *used. Nothing is removed.
    unsigned copyOut(char *out, unsigned capacity, unsigned *used) const
    {
        unsigned bytes = 0, lines = 0;
        for (; lines < count_; ++lines) {
            const char *line = lines_[(head_ + lines) % kRingLines];
            unsigned length = 0;
            while (length < kLineBytes && line[length]) ++length;
            if (bytes + length + 1 > capacity) break;
            memcpy(out + bytes, line, length);
            bytes += length;
            out[bytes++] = '\n';
        }
        *used = bytes;
        return lines;
    }
    void consume(unsigned lines)
    {
        if (lines > count_) lines = count_;
        head_ = (head_ + lines) % kRingLines;
        count_ -= lines;
        dropped_ = 0;
    }

private:
    char lines_[kRingLines][kLineBytes] = {};
    unsigned head_ = 0, count_ = 0;
    uint32_t dropped_ = 0;
};
}  // namespace sheng_thermal_cap_log

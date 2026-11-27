// SPDX-License-Identifier: GPL-3.0-or-later
// Thermal caps for the sheng board, off by default: the SoC temperatures that ThermalProbe.cpp (thermal=1) reads become a thermal level,
// the level becomes frequency caps for the two governors, and the two hottest levels stop the system. `thermal-cap=<n>`:
//   1  read only: compute the level and log it. No cap, no shutdown.
//   2  also publish the caps: CpuFreqProbe.cpp (cpufreq=4) and GpuPowerProbe.cpp (gpu-dcvs=1, or the cap-only thread with gpu-dcvs=0)
//      read them every 16 ms window and choose no level above them.
//   3  also act on levels 4 and 5: level 4 asks the kernel for an orderly halt (reboot_kernel with RB_HALT: processes end, file systems
//      are synced and unmounted) and the halt ends in PSCI SYSTEM_OFF; level 5, or a halt that has not finished 20 s later, is PSCI
//      SYSTEM_OFF at once.
// `thermal-cap-offset=<0..80>` adds that many degrees to every reading before it is judged (a test: reach any level at room
// temperature); the log shows the real values and the offset. `thermal-cap-log=0` keeps the lines off the disk (IOLog and the boot
// log's [thermal cap] row only). `gpu-dcvs-max=<1..8>` is the GPU top the level table counts from (default 8, GpuPowerProbe.cpp).
//
// It needs the probe: without thermal=1 there are no readings and nothing starts (native_thermal_cap[0] = -1). It never writes a TSENS
// or a frequency register and never sends a GPU message: it stores the four cap words (ThermalCapHooks.h), and each governor stays the
// only writer of its own registers. It never calls the power management's over-temperature notification either (that asks for sleep).
// The levels, why they are where they are, and their rounds: ThermalCapPolicy.h.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include "ThermalCapPolicy.h"
#include "ThermalCapLog.h"

extern "C" {
// The caps the governors read (ThermalCapHooks.h, defined in CpuFreqProbe.cpp): until mode 2 publishes, no cap.
extern volatile uint32_t sheng_thermal_cpu_percent[3];
extern volatile uint32_t sheng_thermal_gpu_level;
// [0] 0 off, 1..3 the mode running, -1 the probe is not running, -2 no memory, -3 no thread, -4 in a VM; [1] level, [2] rounds,
// [3] blind rounds in a row, [4] 1 halt requested, 2 power-off requested, [5] lines written to the file, [6] lines dropped from the ring,
// [7] the last file error (0 when the last write went through).
volatile int32_t native_thermal_cap[8];
// ThermalProbe.cpp: [0] stage (2: sampled), [1] samples, [2] thermal | thermal-ms << 8, [6..8] SROT control words; every sensor in
// millidegrees; per frequency domain [7] the firmware limit interrupt pending.
extern volatile uint32_t native_thermal_info[9] __attribute__((weak_import));
extern volatile int32_t native_thermal_mc[THERMAL_CONTROLLERS][THERMAL_SENSORS] __attribute__((weak_import));
extern volatile uint32_t native_thermal_lmh[3][8] __attribute__((weak_import));
void sheng_thermal_sample(void) __attribute__((weak_import));
// The governors' statistics, for the log only.
extern volatile uint32_t native_cpufreq_gov[9] __attribute__((weak_import));
extern volatile uint32_t native_cpufreq_throttle[9] __attribute__((weak_import));
extern volatile uint32_t native_gpu_dcvs[15] __attribute__((weak_import));
extern uint32_t native_gpu_perf_vote[2] __attribute__((weak_import));
int reboot_kernel(int howto, char *message);   // bsd/kern/kern_shutdown.c; BootLog.c calls it the same way
int get_system_inshutdown(void);
int sheng_thermal_cap_file_append(const char *data, unsigned bytes);   // ThermalCapFile.c (VFS, BSD flags)
void sheng_thermal_cap_power_off(void) __attribute__((noreturn));      // ThermalCapPsci.c
void sheng_thermal_cap_probe(void);
unsigned sheng_thermal_cap_row(char *row, unsigned size);
}

namespace {
using namespace sheng_thermal_cap;
namespace tlog = sheng_thermal_cap_log;

constexpr int kRbHalt = 0x08;   // RB_HALT, bsd/sys/reboot.h
constexpr uint32_t kPollQuietMs = 1000, kPollThrottledMs = 250, kLogEveryMs = 10000, kFileRetryMs = 5000, kHaltGraceMs = 20000;
constexpr unsigned kWriteBufferBytes = tlog::kRingLines * tlog::kLineBytes;

uint32_t gMode, gOffsetC, gFileLog = 1, gGpuTop = kGpuLevels;
bool gOwnSamples;                  // the probe has no sampling thread (thermal-ms=0): this thread asks for its samples
IOLock *gRingLock;
tlog::Ring gRing;
char gLastLine[tlog::kLineBytes];  // the last logged line, for the boot log's row
volatile uint32_t gHaltRequested;
int (*gPreviousHaltRestart)(unsigned int);

uint32_t nowMs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return uint32_t(ns / 1000000);
}

uint32_t bootValue(const char *name, uint32_t fallback)
{
    uint32_t value = fallback;
    PE_parse_boot_argn(name, &value, sizeof(value));
    return value;
}

void logLine(const char *line)
{
    IOLog("ThermalCap: %s\n", line);
    if (!gRingLock) return;
    IOLockLock(gRingLock);
    strlcpy(gLastLine, line, sizeof gLastLine);
    if (gFileLog) gRing.push(line);
    native_thermal_cap[6] = int32_t(gRing.dropped());
    IOLockUnlock(gRingLock);
}

// Writes what the ring holds and removes it on success. During a shutdown nothing is written: the file systems are going away.
int flushRing(char *buffer)
{
    if (get_system_inshutdown() != 0) return 16;   // EBUSY
    unsigned used = 0, lines;
    IOLockLock(gRingLock);
    lines = gRing.copyOut(buffer, kWriteBufferBytes, &used);
    IOLockUnlock(gRingLock);
    if (!lines) return 0;
    const int error = sheng_thermal_cap_file_append(buffer, used);
    native_thermal_cap[7] = error;
    if (error) return error;
    IOLockLock(gRingLock);
    gRing.consume(lines);
    native_thermal_cap[5] = native_thermal_cap[5] + int32_t(lines);
    native_thermal_cap[6] = 0;
    IOLockUnlock(gRingLock);
    return 0;
}

// Separate from the polling thread, so that a slow or stuck file system can never delay the temperature checks.
void writerThread(void *, wait_result_t)
{
    char *buffer = static_cast<char *>(IOMallocData(kWriteBufferBytes));
    if (!buffer) {
        IOLog("ThermalCap: no memory for the log writer\n");
        return;
    }
    uint32_t nextTry = 0;
    bool reported = false;
    for (;;) {
        IOSleep(1000);
        const uint32_t now = nowMs();
        if (int32_t(now - nextTry) < 0) continue;
        const int error = flushRing(buffer);
        if (error) {
            nextTry = now + kFileRetryMs;
            if (!reported) IOLog("ThermalCap: log file not writable yet (%d)\n", error);
            reported = true;
        } else {
            reported = false;
        }
    }
}

// The platform expert's halt hook. Only a halt this driver asked for ends in SYSTEM_OFF; anything else goes on as before.
int haltRestart(unsigned int type)
{
    if (type == kPEHaltCPU && __atomic_load_n(&gHaltRequested, __ATOMIC_ACQUIRE)) sheng_thermal_cap_power_off();
    return gPreviousHaltRestart ? gPreviousHaltRestart(type) : -1;
}

void haltThread(void *, wait_result_t)
{
    char *buffer = static_cast<char *>(IOMallocData(kWriteBufferBytes));
    if (buffer) {
        flushRing(buffer);   // the reason reaches the disk first; a stuck file system is ended by the polling thread's deadline
        IOFreeData(buffer, kWriteBufferBytes);
    }
    reboot_kernel(kRbHalt, const_cast<char *>("ThermalCap: level 4"));
}

void publishCaps(const ThermalCaps &caps)
{
    for (unsigned d = 0; d < 3; ++d) __atomic_store_n(&sheng_thermal_cpu_percent[d], uint32_t(caps.cpuPercent[d]), __ATOMIC_RELAXED);
    __atomic_store_n(&sheng_thermal_gpu_level, uint32_t(caps.gpuLevel), __ATOMIC_RELAXED);
}

void fillGovernors(tlog::Row &row)
{
    using tlog::kNone;
    for (unsigned d = 0; d < 3; ++d) row.mhz[d] = kNone;
    row.gpuLevel = row.gpuBusy = row.throttled = row.limitIrq = kNone;
    if (&native_cpufreq_gov && native_cpufreq_gov[1]) {
        for (unsigned d = 0; d < 3; ++d) row.mhz[d] = native_cpufreq_gov[d * 3 + 1];
        if (&native_cpufreq_throttle) {
            uint32_t total = 0;
            for (unsigned d = 0; d < 3; ++d) total += native_cpufreq_throttle[d * 3 + 1];
            row.throttled = total;
        }
    }
    if (&native_gpu_dcvs && native_gpu_dcvs[0]) {
        row.gpuLevel = native_gpu_dcvs[1];
        if (native_gpu_dcvs[0] == 1) row.gpuBusy = native_gpu_dcvs[4];
    } else if (&native_gpu_perf_vote && native_gpu_perf_vote[0]) {
        row.gpuLevel = native_gpu_perf_vote[0];
    }
    if (&native_thermal_lmh) {
        uint32_t bits = 0;
        bool known = false;
        for (unsigned d = 0; d < 3; ++d) {
            if (native_thermal_lmh[d][7] == 2) continue;   // the distributor is not mapped
            known = true;
            bits |= (native_thermal_lmh[d][7] & 1) << d;
        }
        if (known) row.limitIrq = bits;
    }
}

void pollThread(void *, wait_result_t)
{
    ThermalState state = {};
    SampleWatch watch = {};
    SocAverage soc;
    uint32_t lastLogged = 0, haltAt = 0;
    uint8_t lastLevel = 0xff;
    bool logged = false, haltStarted = false;
    for (;;) {
        const uint32_t now = nowMs();
        if (gOwnSamples) sheng_thermal_sample();
        int32_t mc[THERMAL_CONTROLLERS][THERMAL_SENSORS];
        uint32_t ctrl[THERMAL_CONTROLLERS];
        for (unsigned c = 0; c < THERMAL_CONTROLLERS; ++c) {
            ctrl[c] = native_thermal_info[6 + c];
            for (unsigned s = 0; s < THERMAL_SENSORS; ++s) mc[c][s] = native_thermal_mc[c][s];
        }
        const bool sampled = native_thermal_info[0] == 2;
        const bool fresh = sampleFresh(watch, native_thermal_info[1]);
        const Readings readings = readingsFromProbe(mc, ctrl);
        ThermalInput in = {};
        for (unsigned d = 0; d < 3; ++d) in.tjCpu[d] = readings.cpu[d];
        in.tjGpu = readings.gpu;
        in.tjMax = hottest(readings);
        in.tSoc = soc.add(readings.aossMin, now);
        in.blind = !sampled || !fresh || groupsMissing(readings);
        if (!sampled || !fresh) {
            // Old values are not judged: without a new sample nothing can raise or lower the level, and the blind floor follows.
            in.tjMax = in.tjGpu = in.tSoc = kNone;
            for (unsigned d = 0; d < 3; ++d) in.tjCpu[d] = kNone;
        }
        in.offsetC = int32_t(gOffsetC);
        in.gpuTop = uint8_t(gGpuTop);
        const ThermalCaps caps = thermalStep(state, in, now);
        if (gMode >= 2) publishCaps(caps);
        native_thermal_cap[1] = caps.level;
        native_thermal_cap[2] = native_thermal_cap[2] + 1;
        native_thermal_cap[3] = int32_t(state.blindRounds);
        if (!logged || caps.level != lastLevel || now - lastLogged >= kLogEveryMs) {
            tlog::Row row = {};
            row.seconds = now / 1000;
            row.level = caps.level;
            row.tjMax = hottest(readings);
            row.tjGpu = readings.gpu;
            for (unsigned d = 0; d < 3; ++d) {
                row.tjCpu[d] = readings.cpu[d];
                row.capCpu[d] = caps.cpuPercent[d];
            }
            row.ddr = readings.ddr;
            row.cpuss = readings.cpuss;
            row.tSoc = in.tSoc;
            row.capGpu = caps.gpuLevel;
            row.unreadable = readings.unreadable;
            row.blindRounds = state.blindRounds;
            row.offsetC = in.offsetC;
            fillGovernors(row);
            char line[tlog::kLineBytes];
            tlog::formatLine(line, sizeof line, row);
            logLine(line);
            logged = true;
            lastLogged = now;
            lastLevel = caps.level;
        }
        if (gMode == 3) {
            if (caps.action == 1 && !haltStarted) {
                char line[tlog::kLineBytes];
                snprintf(line, sizeof line, "level 4: orderly halt requested, hottest %d mC", in.tjMax);
                logLine(line);
                haltStarted = true;
                haltAt = now;
                native_thermal_cap[4] = 1;
                __atomic_store_n(&gHaltRequested, 1u, __ATOMIC_RELEASE);
                thread_t thread = nullptr;
                if (kernel_thread_start(haltThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
                else IOLog("ThermalCap: cannot start the halt thread\n");
            }
            if (caps.action == 2 || (haltStarted && now - haltAt >= kHaltGraceMs)) {
                native_thermal_cap[4] = 2;
                IOLog("ThermalCap: powering off now (level %u, hottest %d mC, halt pending %d)\n", unsigned(caps.level), in.tjMax,
                      int(haltStarted));
                sheng_thermal_cap_power_off();
            }
        }
        IOSleep(caps.level ? kPollThrottledMs : kPollQuietMs);
    }
}
}  // namespace

extern "C" void sheng_thermal_cap_probe(void)
{
    uint32_t mode = 0;
    if (!PE_parse_boot_argn("thermal-cap", &mode, sizeof(mode)) || mode < 1 || mode > 3 || native_thermal_cap[0]) return;
    // Note: sheng board.toml sets vmm-present=1 so AMFI skips ApplePMGRNub on non-Apple silicon, making PE_vmm_present=1 on real
    // SM8550 hardware. Verify real TSENS hardware via ThermalProbe's sampled stage and SROT controller enable bits instead.
    // The probe maps the sensors and samples them; without it there is nothing to judge (thermal=1).
    if (!sheng_thermal_sample || !&native_thermal_info || !&native_thermal_mc || (native_thermal_info[0] != 1 && native_thermal_info[0] != 2)) {
        native_thermal_cap[0] = -1;
        IOLog("ThermalCap: the temperature probe is not running (thermal=1): not started\n");
        return;
    }
    if (((native_thermal_info[6] | native_thermal_info[7] | native_thermal_info[8]) & THERMAL_CTRL_ENABLE) == 0) {
        native_thermal_cap[0] = -4;
        IOLog("ThermalCap: no enabled TSENS controller (srot %x/%x/%x): not started\n",
              native_thermal_info[6], native_thermal_info[7], native_thermal_info[8]);
        return;
    }
    gOwnSamples = (native_thermal_info[2] >> 8) == 0;
    gOffsetC = bootValue("thermal-cap-offset", 0);
    if (gOffsetC > 80) gOffsetC = 80;
    gFileLog = bootValue("thermal-cap-log", 1) != 0;
    gGpuTop = clampGpuLevel(bootValue("gpu-dcvs-max", kGpuLevels));
    gRingLock = IOLockAlloc();
    if (!gRingLock) {native_thermal_cap[0] = -2; return;}
    gMode = mode;
    if (mode == 3) {
        // Installed once, before any level can ask for a halt; an earlier hook keeps working for everything else.
        gPreviousHaltRestart = PE_halt_restart;
        PE_halt_restart = haltRestart;
    }
    char line[tlog::kLineBytes];
    snprintf(line, sizeof line, "start table v%u mode %u offset %u gpu-top %u samples %s log %u", kTableVersion, gMode, gOffsetC, gGpuTop,
             gOwnSamples ? "own" : "probe", gFileLog);
    logLine(line);
    snprintf(line, sizeof line, "args thermal=%u thermal-ms=%u cpufreq=%u cpufreq-cap=%u gpu-dcvs=%u gpu-dcvs-min=%u gpu-perf=%u srot %x/%x/%x",
             native_thermal_info[2] & 0xff, native_thermal_info[2] >> 8, bootValue("cpufreq", 0), bootValue("cpufreq-cap", 0),
             bootValue("gpu-dcvs", 0), bootValue("gpu-dcvs-min", 3), bootValue("gpu-perf", 1), native_thermal_info[6], native_thermal_info[7],
             native_thermal_info[8]);
    logLine(line);
    if (mode >= 2 && bootValue("cpufreq", 0) != 4) logLine("cpufreq is not 4: no CPU governor reads the CPU caps");
    thread_t thread = nullptr;
    if (gFileLog) {
        if (kernel_thread_start(writerThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
        else IOLog("ThermalCap: cannot start the log writer\n");
    }
    thread = nullptr;
    if (kernel_thread_start(pollThread, nullptr, &thread) != KERN_SUCCESS) {
        native_thermal_cap[0] = -3;
        IOLog("ThermalCap: cannot start the polling thread\n");
        return;
    }
    thread_deallocate(thread);
    native_thermal_cap[0] = int32_t(mode);
}

// The boot log's row: "[thermal cap] st=<state> halt=<0..2> file=<lines written>/<dropped>/<last error> | <the last logged line>".
// Returns its length, 0 while the feature is off. Read without waiting for the poll thread: a number may be one round behind.
extern "C" unsigned sheng_thermal_cap_row(char *row, unsigned size)
{
    if (!native_thermal_cap[0] || !row || size < 2) return 0;
    char last[tlog::kLineBytes] = "";
    if (gRingLock) {
        IOLockLock(gRingLock);
        strlcpy(last, gLastLine, sizeof last);
        IOLockUnlock(gRingLock);
    }
    const int n = snprintf(row, size, "[thermal cap] st=%d halt=%d file=%d/%d/%d | %s", native_thermal_cap[0], native_thermal_cap[4],
                           native_thermal_cap[5], native_thermal_cap[6], native_thermal_cap[7], last);
    return n < 0 ? 0 : unsigned(n) >= size ? size - 1 : unsigned(n);
}

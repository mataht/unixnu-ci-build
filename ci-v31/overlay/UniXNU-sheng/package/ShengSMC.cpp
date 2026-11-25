// SPDX-License-Identifier: BSD-2-Clause-Patent
// Virtual AppleSMC + IOReport "Energy Model" telemetry bridge for SM8550 (sheng) on macOS 27.
// Publishes AppleSMC + AppleSMCClient (silencing thermalmonitord "Couldn't connect to smc!", serving powerd, sysmond,
// Activity Monitor, and powermetrics) and exposes monotonic mJ IOReport channels via IOSimpleReporter + IOReportLegend.
// Enabled by default via smc=1 in PE_builtin_boot_args[] (smc=0 disables).
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOKernelReporters.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <mach/kmod.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include "ShengSmcKeys.h"
#include "ThermalProbe.h"
#include "ThermalLevel.h"

extern kmod_info_t g_kernel_kmod_info;

extern "C" {
extern volatile uint32_t native_thermal_info[9] __attribute__((weak_import));
extern volatile int32_t native_thermal_now[THERMAL_GROUPS] __attribute__((weak_import));
extern volatile int32_t native_thermal_mc[THERMAL_CONTROLLERS][THERMAL_SENSORS] __attribute__((weak_import));
extern void sheng_thermal_sample(void) __attribute__((weak_import));
extern volatile uint32_t native_cpufreq_gov[9] __attribute__((weak_import));
extern volatile uint32_t native_gpu_dcvs[15] __attribute__((weak_import));
extern uint32_t native_gpu_perf_vote[2] __attribute__((weak_import));
extern volatile int32_t native_battery_stats[16] __attribute__((weak_import));
extern volatile int32_t native_cdsp_stats[16] __attribute__((weak_import));

void sheng_smc_static_init(void) __asm__("__GLOBAL__sub_I_ShengSMC.cpp");
volatile int32_t native_smc_state;
void sheng_smc_probe(void);
unsigned sheng_smc_row(char *row, unsigned size);
}

namespace {

enum EnergyCh {
    ChCpu = 0,
    ChEcpu,
    ChPcpu,
    ChGpu,
    ChAne,
    ChDram,
    ChPkg,
    ChCount
};

const uint64_t kChIds[ChCount] = {
    IOREPORT_MAKEID('C', 'P', 'U', ' ', 'E', 'n', 'r', 'g'),
    IOREPORT_MAKEID('E', 'C', 'P', 'U', 0,   0,   0,   0),
    IOREPORT_MAKEID('P', 'C', 'P', 'U', 0,   0,   0,   0),
    IOREPORT_MAKEID('G', 'P', 'U', ' ', 'E', 'n', 'r', 'g'),
    IOREPORT_MAKEID('A', 'N', 'E', 0,   0,   0,   0,   0),
    IOREPORT_MAKEID('D', 'R', 'A', 'M', 0,   0,   0,   0),
    IOREPORT_MAKEID('P', 'a', 'c', 'k', 'a', 'g', 'e', 0),
};

const char *const kChNames[ChCount] = {
    "CPU Energy",
    "ECPU",
    "PCPU",
    "GPU Energy",
    "ANE",
    "DRAM",
    "Package",
};

IOLock *gSmcLock;
ShengSmcSnapshot gSnap;
thermal_level_state gTlState;
uint64_t gLastSampleAbs, gMinSampleAbs, gLastEnergyAbs;
uint64_t gEnergyUj[ChCount];
uint32_t gOpens, gCloses, gReads, gInfos, gIndices, gWrites, gMisses, gLastMiss;
uint32_t gReportCfg, gReportUpd;

float mcToC(int32_t mc, float fallback)
{
    return (mc != THERMAL_NONE && mc > -40000 && mc < 150000) ? ((float)mc * 0.001f) : fallback;
}

// Called with gSmcLock held. Integrates real elapsed ns * domain Watts into monotonic uJ -> mJ counters.
void refreshSnapshotLocked(IOSimpleReporter *reporter)
{
    const uint64_t now = mach_absolute_time();
    if (!gLastSampleAbs || (now - gLastSampleAbs) >= gMinSampleAbs) {
        gLastSampleAbs = now;
        int32_t groups[THERMAL_GROUPS];
        for (unsigned g = 0; g < THERMAL_GROUPS; ++g) groups[g] = THERMAL_NONE;
        bool sampled = false;
        uint32_t samples = 0;
        if (sheng_thermal_sample && &native_thermal_info[0] && &native_thermal_now[0]) {
            if (native_thermal_info[0] != 2) sheng_thermal_sample();
            sampled = native_thermal_info[0] == 2;
            samples = native_thermal_info[1];
            if (sampled) {
                for (unsigned g = 0; g < THERMAL_GROUPS; ++g) groups[g] = native_thermal_now[g];
            }
        }
        gSnap.cpu_e_c  = mcToC(groups[THERMAL_GROUP_CPU_D0], 45.0f);
        gSnap.cpu_p_c  = mcToC(groups[THERMAL_GROUP_CPU_D1], 48.0f);
        gSnap.cpu_x3_c = mcToC(groups[THERMAL_GROUP_CPU_D2], 48.0f);
        float maxCpu = gSnap.cpu_e_c;
        if (gSnap.cpu_p_c > maxCpu) maxCpu = gSnap.cpu_p_c;
        if (gSnap.cpu_x3_c > maxCpu) maxCpu = gSnap.cpu_x3_c;
        gSnap.cpu_max_c = maxCpu;
        gSnap.gpu_c     = mcToC(groups[THERMAL_GROUP_GPU], 44.0f);
        const int32_t aossMc = (sampled && &native_thermal_mc[0][0]) ? native_thermal_mc[0][0] : groups[THERMAL_GROUP_OTHER];
        const int32_t cdspMc = (sampled && &native_thermal_mc[0][0]) ? native_thermal_mc[2][4] : groups[THERMAL_GROUP_GPU];
        gSnap.soc_c     = mcToC(aossMc, mcToC(groups[THERMAL_GROUP_OTHER], 44.0f));
        gSnap.wifi_c    = mcToC(groups[THERMAL_GROUP_MODEM], gSnap.soc_c);
        gSnap.npu_c     = mcToC(cdspMc, gSnap.gpu_c);
        gSnap.thermal_level = (uint8_t)thermal_level_step(&gTlState, groups, sampled, samples);

        int32_t battMv = 0, battMa = 0;
        bool extConn = false, battPres = true;
        float battC = 32.0f;
        if (&native_battery_stats[0] && native_battery_stats[0] >= 4) {
            battMv = native_battery_stats[2];
            battMa = native_battery_stats[3];
            if (native_battery_stats[4] > 0 && native_battery_stats[4] < 1000) {
                battC = (float)native_battery_stats[4] * 0.1f;
            }
            const int32_t flags = native_battery_stats[5];
            battPres = (flags & 1) != 0;
            extConn  = ((flags >> 1) & 1) != 0;
        }
        gSnap.batt_c = battC;
        gSnap.external_connected = extConn ? 1u : 0u;
        gSnap.battery_present = battPres ? 1u : 0u;

        uint32_t mhz[3] = {614, 883, 883}, busy[3] = {5, 5, 2};
        if (&native_cpufreq_gov[0] && native_cpufreq_gov[1]) {
            for (unsigned d = 0; d < 3; ++d) {
                mhz[d]  = native_cpufreq_gov[d * 3 + 1];
                busy[d] = native_cpufreq_gov[d * 3 + 2] / 10u; // permille -> percent
            }
        }
        uint32_t gpuLvl = 1, gpuBusy = 0;
        if (&native_gpu_dcvs[0] && native_gpu_dcvs[0]) {
            gpuLvl = native_gpu_dcvs[1];
            if (native_gpu_dcvs[0] == 1) gpuBusy = native_gpu_dcvs[4];
        } else if (&native_gpu_perf_vote[0] && native_gpu_perf_vote[0]) {
            gpuLvl = native_gpu_perf_vote[0];
        }
        const bool cdspUp = (&native_cdsp_stats[0] && native_cdsp_stats[0] >= 11);
        sheng_smc_estimate_power(mhz, busy, gpuLvl, gpuBusy, cdspUp, battMv, battMa, extConn, &gSnap);
    }

    if (gLastEnergyAbs && now > gLastEnergyAbs) {
        uint64_t dtNs = 0;
        absolutetime_to_nanoseconds(now - gLastEnergyAbs, &dtNs);
        if (dtNs > 0 && dtNs < 60ull * 1000000000ull) {
            const float dtMs = (float)dtNs * 1e-6f; // W * ms = mJ; * 1000 = uJ
            gEnergyUj[ChCpu]  += (uint64_t)(gSnap.cpu_w  * dtMs * 1000.0f);
            gEnergyUj[ChEcpu] += (uint64_t)(gSnap.ecpu_w * dtMs * 1000.0f);
            gEnergyUj[ChPcpu] += (uint64_t)(gSnap.pcpu_w * dtMs * 1000.0f);
            gEnergyUj[ChGpu]  += (uint64_t)(gSnap.gpu_w  * dtMs * 1000.0f);
            gEnergyUj[ChAne]  += (uint64_t)(gSnap.npu_w  * dtMs * 1000.0f);
            gEnergyUj[ChDram] += (uint64_t)(gSnap.dram_w * dtMs * 1000.0f);
            gEnergyUj[ChPkg]  += (uint64_t)(gSnap.sys_w  * dtMs * 1000.0f);
        }
    }
    gLastEnergyAbs = now;

    if (reporter) {
        for (unsigned c = 0; c < ChCount; ++c) {
            reporter->setValue(kChIds[c], (int64_t)(gEnergyUj[c] / 1000ull)); // mJ
        }
    }
}

} // namespace

class AppleSMC : public IOService {
    OSDeclareDefaultStructors(AppleSMC)
    IOSimpleReporter *fEnergyReporter = nullptr;

public:
    bool publish(IOService *provider)
    {
        if (!init() || !attach(provider) || !start(provider)) return false;
        setName("AppleSMC");
        setProperty("IOUserClientClass", "AppleSMCClient");
        setProperty("smc-version", "2.45f1");

        fEnergyReporter = IOSimpleReporter::with(this, kIOReportCategoryPower, kIOReportUnit_mJ);
        if (fEnergyReporter) {
            for (unsigned c = 0; c < ChCount; ++c) {
                fEnergyReporter->addChannel(kChIds[c], kChNames[c]);
                fEnergyReporter->setValue(kChIds[c], 0);
            }
            if (IOReportLegend::addReporterLegend(this, fEnergyReporter, "Energy Model", nullptr) == kIOReturnSuccess) {
                setProperty(kIOReportLegendPublicKey, kOSBooleanTrue);
            }
        }
        registerService();
        return true;
    }

    IOSimpleReporter *energyReporter() const { return fEnergyReporter; }

    IOReturn configureReport(
        IOReportChannelList *channels,
        IOReportConfigureAction action,
        void *result,
        void *destination) override
    {
        __atomic_fetch_add(&gReportCfg, 1u, __ATOMIC_RELAXED);
        if (fEnergyReporter) {
            return fEnergyReporter->configureReport(channels, action, result, destination);
        }
        return IOService::configureReport(channels, action, result, destination);
    }

    IOReturn updateReport(
        IOReportChannelList *channels,
        IOReportUpdateAction action,
        void *result,
        void *destination) override
    {
        __atomic_fetch_add(&gReportUpd, 1u, __ATOMIC_RELAXED);
        if (fEnergyReporter) {
            IOLockLock(gSmcLock);
            refreshSnapshotLocked(fEnergyReporter);
            IOLockUnlock(gSmcLock);
            return fEnergyReporter->updateReport(channels, action, result, destination);
        }
        return IOService::updateReport(channels, action, result, destination);
    }
};
OSDefineMetaClassAndStructors(AppleSMC, IOService)

class AppleSMCClient : public IOUserClient {
    OSDeclareDefaultStructors(AppleSMCClient)
    AppleSMC *fOwner = nullptr;

    static IOReturn sOpen(OSObject *, void *, IOExternalMethodArguments *)
    {
        return kIOReturnSuccess;
    }

    static IOReturn sClose(OSObject *, void *, IOExternalMethodArguments *)
    {
        return kIOReturnSuccess;
    }

    static IOReturn sKextCall(OSObject *target, void *, IOExternalMethodArguments *args)
    {
        if (!args || !args->structureInput || !args->structureOutput) return kIOReturnBadArgument;
        if (args->structureInputSize < SHENG_SMC_LEGACY_BYTES || args->structureOutputSize < SHENG_SMC_LEGACY_BYTES) {
            return kIOReturnBadArgument;
        }
        auto *self = static_cast<AppleSMCClient *>(target);
        ShengSmcParam inBuf = {}, outBuf = {};
        const uint32_t inCopy = args->structureInputSize < sizeof(inBuf) ? args->structureInputSize : (uint32_t)sizeof(inBuf);
        memcpy(&inBuf, args->structureInput, inCopy);

        IOLockLock(gSmcLock);
        refreshSnapshotLocked(self->fOwner ? self->fOwner->energyReporter() : nullptr);
        sheng_smc_handle_call(&inBuf, &outBuf, &gSnap, &gReads, &gInfos, &gIndices, &gWrites, &gMisses, &gLastMiss);
        IOLockUnlock(gSmcLock);

        const uint32_t outCopy = args->structureOutputSize < sizeof(outBuf) ? args->structureOutputSize : (uint32_t)sizeof(outBuf);
        memcpy(args->structureOutput, &outBuf, outCopy);
        args->structureOutputSize = outCopy;
        return kIOReturnSuccess;
    }

public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override
    {
        return IOUserClient::initWithTask(owningTask, securityID, type);
    }

    bool start(IOService *provider) override
    {
        fOwner = OSDynamicCast(AppleSMC, provider);
        if (!fOwner || !IOUserClient::start(provider)) return false;
        setProperty(kIOUserClientDefaultLockingKey, kOSBooleanTrue);
        IOLockLock(gSmcLock);
        gOpens++;
        IOLockUnlock(gSmcLock);
        return true;
    }

    IOReturn clientClose() override
    {
        IOLockLock(gSmcLock);
        gCloses++;
        IOLockUnlock(gSmcLock);
        terminate();
        return kIOReturnSuccess;
    }

    IOReturn externalMethod(
        uint32_t selector,
        IOExternalMethodArguments *args,
        IOExternalMethodDispatch *,
        OSObject *,
        void *) override
    {
        static const IOExternalMethodDispatch kMethods[3] = {
            {&AppleSMCClient::sOpen,     0, 0,                             0, 0},
            {&AppleSMCClient::sClose,    0, 0,                             0, 0},
            {&AppleSMCClient::sKextCall, 0, kIOUCVariableStructureSize,    0, kIOUCVariableStructureSize},
        };
        if (selector >= 3) return kIOReturnUnsupported;
        return IOUserClient::externalMethod(selector, args, const_cast<IOExternalMethodDispatch *>(&kMethods[selector]), this, nullptr);
    }
};
OSDefineMetaClassAndStructors(AppleSMCClient, IOUserClient)

extern "C" void sheng_smc_probe(void)
{
    unsigned mode = 1;
    PE_parse_boot_argn("smc", &mode, sizeof mode);
    if (!mode || native_smc_state) return;

    thermal_level_init(&gTlState);
    nanoseconds_to_absolutetime(250ull * 1000000ull, &gMinSampleAbs);
    gSmcLock = IOLockAlloc();
    if (!gSmcLock) { native_smc_state = -2; return; }

    static const char *const kClasses[2] = {"AppleSMC", "AppleSMCClient"};
    for (const char *name : kClasses) {
        const OSSymbol *sym = OSSymbol::withCString(name);
        const bool taken = sym && OSMetaClass::getMetaClassWithName(sym);
        OSSafeReleaseNULL(sym);
        if (taken) { native_smc_state = -12; return; }
    }

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) { native_smc_state = -10; return; }
    sheng_smc_static_init();
    if (OSMetaClass::postModLoad(handle) != kIOReturnSuccess) { native_smc_state = -11; return; }

    IOService *platform = IOService::getPlatform();
    if (!platform) platform = IOService::getResourceService();
    if (!platform) { native_smc_state = -13; return; }

    AppleSMC *smc = OSTypeAlloc(AppleSMC);
    if (!smc || !smc->publish(platform)) {
        OSSafeReleaseNULL(smc);
        native_smc_state = -14;
        return;
    }
    IOLockLock(gSmcLock);
    refreshSnapshotLocked(smc->energyReporter());
    IOLockUnlock(gSmcLock);
    native_smc_state = 1;
    IOLog("ShengSMC: published AppleSMC (%u keys) + IOReport Energy Model (7 mJ channels)\n", SHENG_SMC_KEY_COUNT);
}

extern "C" unsigned sheng_smc_row(char *row, unsigned size)
{
    if (!native_smc_state || !row || size < 2) return 0;
    if (native_smc_state < 0) {
        const int n = snprintf(row, size, "[smc] st=%d", native_smc_state);
        return n > 0 ? ((unsigned)n < size ? (unsigned)n : size - 1) : 0;
    }
    IOLockLock(gSmcLock);
    const uint32_t opens = gOpens, closes = gCloses, reads = gReads, infos = gInfos, idxs = gIndices, writes = gWrites;
    const uint32_t misses = gMisses, lastMiss = gLastMiss;
    const uint32_t cfg = gReportCfg, upd = gReportUpd;
    const uint64_t cpuMj = gEnergyUj[ChCpu] / 1000ull;
    const uint64_t gpuMj = gEnergyUj[ChGpu] / 1000ull;
    const uint64_t pkgMj = gEnergyUj[ChPkg] / 1000ull;
    const int sysMw = (int)(gSnap.sys_w * 1000.0f);
    IOLockUnlock(gSmcLock);

    char missStr[5] = {'-', 0, 0, 0, 0};
    if (lastMiss) {
        for (int i = 0; i < 4; ++i) {
            const uint8_t ch = (uint8_t)((lastMiss >> (24 - i * 8)) & 0xff);
            missStr[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        }
        missStr[4] = 0;
    }
    const int n = snprintf(
        row, size,
        "[smc] st=%d open=%u/%u rd=%u inf=%u idx=%u wr=%u miss=%u(%s) sys=%dmW | ioreport cfg=%u upd=%u mJ=%llu/%llu/%llu",
        native_smc_state, opens, closes, reads, infos, idxs, writes, misses, missStr, sysMw,
        cfg, upd, (unsigned long long)cpuMj, (unsigned long long)gpuMj, (unsigned long long)pkgMj);
    if (n <= 0) return 0;
    return (unsigned)n < size ? (unsigned)n : size - 1;
}

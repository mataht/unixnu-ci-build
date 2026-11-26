// SPDX-License-Identifier: BSD-2-Clause-Patent
// Closed-Loop Performance Controller (CLPC) replacement for SM8550 (sheng) on macOS 27 XNU Edge scheduler.
// Enabled by default via clpc=1 in PE_builtin_boot_args[] (clpc=0 disables).
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <kern/thread.h>
#include <kern/thread_group.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "ShengClpcPolicy.h"
#include "ThermalProbe.h"

static_assert(SHENG_PC_IDLE == PERFCONTROL_CLASS_IDLE, "IDLE enum");
static_assert(SHENG_PC_BACKGROUND == PERFCONTROL_CLASS_BACKGROUND, "BG enum");
static_assert(SHENG_PC_UTILITY == PERFCONTROL_CLASS_UTILITY, "UT enum");
static_assert(SHENG_PC_NONUI == PERFCONTROL_CLASS_NONUI, "NONUI enum");
static_assert(SHENG_PC_UI == PERFCONTROL_CLASS_UI, "UI enum");
static_assert(SHENG_PC_ABOVEUI == PERFCONTROL_CLASS_ABOVEUI, "ABOVEUI enum");
static_assert(SHENG_PC_USER_INITIATED == PERFCONTROL_CLASS_USER_INITIATED, "IN enum");
static_assert(SHENG_PC_MAX == PERFCONTROL_CLASS_MAX, "MAX enum");
static_assert(SHENG_CLPC_PSET_NONE == SCHED_PERFCONTROL_PREFERRED_PSET_OVERRIDE_NONE, "PSET_NONE");
static_assert(SHENG_TG_ID_SYSTEM == THREAD_GROUP_SYSTEM, "TG SYSTEM");
static_assert(SHENG_TG_ID_BACKGROUND == THREAD_GROUP_BACKGROUND, "TG BACKGROUND");
static_assert(SHENG_TG_ID_IO_STORAGE == THREAD_GROUP_IO_STORAGE, "TG IO_STORAGE");
static_assert(SHENG_TG_FLAG_EFFICIENT == THREAD_GROUP_FLAGS_EFFICIENT, "FLAG EFFICIENT");
static_assert(SHENG_TG_FLAG_APPLICATION == THREAD_GROUP_FLAGS_APPLICATION, "FLAG APPLICATION");
static_assert(SHENG_TG_FLAG_CRITICAL == THREAD_GROUP_FLAGS_CRITICAL, "FLAG CRITICAL");
static_assert(SHENG_TG_FLAG_BEST_EFFORT == THREAD_GROUP_FLAGS_BEST_EFFORT, "FLAG BEST_EFFORT");
static_assert(SHENG_TG_FLAG_UI_APP == THREAD_GROUP_FLAGS_UI_APP, "FLAG UI_APP");
static_assert(SHENG_TG_FLAG_GAME_MODE == THREAD_GROUP_FLAGS_GAME_MODE, "FLAG GAME_MODE");

extern "C" {
extern unsigned int processor_avail_count;
extern volatile int32_t native_thermal_cap[8] __attribute__((weak_import));
extern volatile uint32_t native_thermal_info[9] __attribute__((weak_import));
extern volatile int32_t native_thermal_now[THERMAL_GROUPS] __attribute__((weak_import));
extern void sched_perfcontrol_edge_cpu_rotation_bitmasks_set(uint32_t pset_id, uint64_t preferred_bitmask, uint64_t migration_bitmask);

// [0] state (0 off, 1 active), [1] thermal tier, [2] x3 rotated (0/1), [3] tg_init calls,
// [4] tg_flags_update calls, [5] resyncs, [6] role E count, [7] role UI/P count, [8] role default count, [9] x3 rotations
volatile uint32_t native_clpc_stats[10];
void sheng_clpc_probe(void);
unsigned sheng_clpc_row(char *row, unsigned size);
}

namespace {

volatile unsigned gThermalTier = 0;
volatile uint32_t gMigrateOnInit = 0;
bool gX3Rotated = false;

void applyPolicy(thread_group_data_t data, sched_perfcontrol_preferred_pset_options_t options)
{
    if (!data || !data->thread_group_data) return;
    ShengClpcDecision d = {};
    const unsigned tier = __atomic_load_n(&gThermalTier, __ATOMIC_RELAXED);
    sheng_clpc_decide(data->thread_group_id, data->thread_group_flags, tier, &d);

    pset_id_t overrides[PERFCONTROL_CLASS_MAX];
    for (unsigned i = 0; i < PERFCONTROL_CLASS_MAX; ++i) {
        overrides[i] = d.overrides[i];
    }
    sched_perfcontrol_thread_group_preferred_psets_set(
        data->thread_group_data,
        d.tg_preferred_pset,
        overrides,
        options);

    if (d.role == SHENG_CLPC_ROLE_BACKGROUND_E) {
        __atomic_fetch_add(&native_clpc_stats[6], 1u, __ATOMIC_RELAXED);
    } else if (d.role == SHENG_CLPC_ROLE_UI_CRITICAL) {
        __atomic_fetch_add(&native_clpc_stats[7], 1u, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&native_clpc_stats[8], 1u, __ATOMIC_RELAXED);
    }
}

void clpcThreadGroupInit(thread_group_data_t data)
{
    __atomic_fetch_add(&native_clpc_stats[3], 1u, __ATOMIC_RELAXED);
    const sched_perfcontrol_preferred_pset_options_t opts =
        __atomic_load_n(&gMigrateOnInit, __ATOMIC_RELAXED)
            ? (SCHED_PERFCONTROL_PREFERRED_PSET_MIGRATE_RUNNING | SCHED_PERFCONTROL_PREFERRED_PSET_MIGRATE_RUNNABLE)
            : 0;
    applyPolicy(data, opts);
}

void clpcThreadGroupDeinit(thread_group_data_t)
{
}

void clpcThreadGroupFlagsUpdate(thread_group_data_t data)
{
    __atomic_fetch_add(&native_clpc_stats[4], 1u, __ATOMIC_RELAXED);
    applyPolicy(
        data,
        SCHED_PERFCONTROL_PREFERRED_PSET_MIGRATE_RUNNING | SCHED_PERFCONTROL_PREFERRED_PSET_MIGRATE_RUNNABLE);
}

void clpcRunningTimerExpire(uint64_t, uint32_t, uint32_t, uint64_t *timeout_ticks)
{
    if (timeout_ticks) *timeout_ticks = 0;
}

sched_perfcontrol_callbacks gCallbacks = {
    .version                   = SCHED_PERFCONTROL_CALLBACKS_VERSION_CURRENT,
    .offcore                   = nullptr,
    .context_switch            = nullptr,
    .oncore                    = nullptr,
    .max_runnable_latency      = nullptr,
    .work_interval_notify      = nullptr,
    .thread_group_init         = clpcThreadGroupInit,
    .thread_group_deinit       = clpcThreadGroupDeinit,
    .deadline_passed           = nullptr,
    .csw                       = nullptr,
    .state_update              = nullptr,
    .thread_group_flags_update = clpcThreadGroupFlagsUpdate,
    .work_interval_ctl         = nullptr,
    .thread_group_blocked      = nullptr,
    .thread_group_unblocked    = nullptr,
    .running_timer_expire      = clpcRunningTimerExpire,
};

void clpcMonitorThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "sheng-clpc");
    // Wait until all 8 SM8550 cores (pset 0 E + pset 1 P) have registered before enabling Edge CLPC callbacks.
    for (unsigned tries = 0; tries < 200 && processor_avail_count < 8; ++tries) {
        IOSleep(50);
    }
    if (processor_avail_count < 8) {
        IOLog("ShengCLPC: SMP incomplete (%u cores), staying passive\n", processor_avail_count);
        return;
    }

    __atomic_store_n(&gMigrateOnInit, 1u, __ATOMIC_RELEASE);
    sched_perfcontrol_register_callbacks(&gCallbacks, 0);
    __atomic_store_n(&gMigrateOnInit, 0u, __ATOMIC_RELEASE);
    sched_perfcontrol_edge_cpu_rotation_bitmasks_set(SHENG_CLPC_PSET_P, SHENG_CLPC_P_ALL_MASK, 0ull);
    __atomic_store_n(&native_clpc_stats[0], 1u, __ATOMIC_RELEASE);
    IOLog("ShengCLPC: registered XNU Edge CLPC callbacks (inits=%u bg_e=%u ui_p=%u def=%u)\n",
          native_clpc_stats[3], native_clpc_stats[6], native_clpc_stats[7], native_clpc_stats[8]);

    for (;;) {
        IOSleep(500);
        const int32_t capLevel = (&native_thermal_cap[0] && native_thermal_cap[0] > 0) ? native_thermal_cap[1] : 0;
        int32_t pMc = THERMAL_NONE, x3Mc = THERMAL_NONE;
        if (&native_thermal_info[0] && &native_thermal_now[0] && native_thermal_info[0] == 2) {
            pMc = native_thermal_now[THERMAL_GROUP_CPU_D1];
            x3Mc = native_thermal_now[THERMAL_GROUP_CPU_D2];
        }

        const unsigned curTier = __atomic_load_n(&gThermalTier, __ATOMIC_RELAXED);
        const unsigned nextTier = sheng_clpc_next_tier(curTier, capLevel);
        if (nextTier != curTier) {
            __atomic_store_n(&gThermalTier, nextTier, __ATOMIC_RELEASE);
            __atomic_store_n(&native_clpc_stats[1], nextTier, __ATOMIC_RELAXED);
            __atomic_store_n(&gMigrateOnInit, 1u, __ATOMIC_RELEASE);
            thread_group_resync(TRUE);
            __atomic_store_n(&gMigrateOnInit, 0u, __ATOMIC_RELEASE);
            __atomic_fetch_add(&native_clpc_stats[5], 1u, __ATOMIC_RELAXED);
        }

        uint64_t prefMask = SHENG_CLPC_P_ALL_MASK, migMask = 0ull;
        const bool nextRot = sheng_clpc_x3_rotate(gX3Rotated, capLevel, pMc, x3Mc, &prefMask, &migMask);
        if (nextRot != gX3Rotated) {
            gX3Rotated = nextRot;
            sched_perfcontrol_edge_cpu_rotation_bitmasks_set(SHENG_CLPC_PSET_P, prefMask, migMask);
            __atomic_store_n(&native_clpc_stats[2], nextRot ? 1u : 0u, __ATOMIC_RELAXED);
            if (nextRot) __atomic_fetch_add(&native_clpc_stats[9], 1u, __ATOMIC_RELAXED);
        }
    }
}

} // namespace

extern "C" void sheng_clpc_probe(void)
{
    unsigned mode = 1;
    PE_parse_boot_argn("clpc", &mode, sizeof mode);
    if (!mode || native_clpc_stats[0]) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(clpcMonitorThread, nullptr, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
}

extern "C" unsigned sheng_clpc_row(char *row, unsigned size)
{
    if (!native_clpc_stats[0] || !row || size < 2) return 0;
    const int n = snprintf(
        row, size,
        "[clpc] st=%u tier=%u x3_rot=%u/%u inits=%u flags=%u resync=%u | bg_e=%u ui_p=%u def=%u",
        native_clpc_stats[0], native_clpc_stats[1], native_clpc_stats[2], native_clpc_stats[9],
        native_clpc_stats[3], native_clpc_stats[4], native_clpc_stats[5],
        native_clpc_stats[6], native_clpc_stats[7], native_clpc_stats[8]);
    if (n <= 0) return 0;
    return (unsigned)n < size ? (unsigned)n : size - 1;
}

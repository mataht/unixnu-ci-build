// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure closed-loop performance controller (CLPC) policy for SM8550 (sheng) on XNU Edge scheduler.
// Shared between ShengCLPC.cpp (kernel) and package/tests/clpc_policy_host.c (host unit test).
#ifndef SHENG_CLPC_POLICY_H
#define SHENG_CLPC_POLICY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// SM8550 Edge processor sets registered by ArmCpu:
//   pset 0 = E-cluster (cpu0..2, 3x Cortex-A510, 0x07)
//   pset 1 = P-cluster (cpu3..6 4x Cortex-A715/A710 + cpu7 1x Cortex-X3, 0xf8)
#define SHENG_CLPC_PSET_E        ((uint8_t)0)
#define SHENG_CLPC_PSET_P        ((uint8_t)1)
#define SHENG_CLPC_PSET_NONE     ((uint8_t)0xff)

// Mirror XNU osfmk/kern/kern_types.h perfcontrol_class_t (checked by static_assert in ShengCLPC.cpp).
#define SHENG_PC_IDLE            1u
#define SHENG_PC_KERNEL          2u
#define SHENG_PC_REALTIME        3u
#define SHENG_PC_BACKGROUND      4u
#define SHENG_PC_UTILITY         5u
#define SHENG_PC_NONUI           6u
#define SHENG_PC_UI              7u
#define SHENG_PC_ABOVEUI         8u
#define SHENG_PC_USER_INITIATED  9u
#define SHENG_PC_MAX             10u

// Mirror XNU osfmk/kern/thread_group.h IDs and flags (checked by static_assert in ShengCLPC.cpp).
#define SHENG_TG_ID_SYSTEM       0ull
#define SHENG_TG_ID_BACKGROUND   1ull
#define SHENG_TG_ID_VM           3ull
#define SHENG_TG_ID_IO_STORAGE   4ull
#define SHENG_TG_ID_PERF_CTRL    5ull
#define SHENG_TG_ID_CELLULAR     6ull

#define SHENG_TG_FLAG_EFFICIENT    0x1u
#define SHENG_TG_FLAG_APPLICATION  0x2u
#define SHENG_TG_FLAG_CRITICAL     0x4u
#define SHENG_TG_FLAG_BEST_EFFORT  0x8u
#define SHENG_TG_FLAG_UI_APP       0x100u
#define SHENG_TG_FLAG_GAME_MODE    0x800u

// CPU bitmasks inside pset 1 (P-cluster: cpu3..7).
// Note XNU's sched_perfcontrol_edge_cpu_rotation_bitmasks_set asserts (preferred & migration) == 0.
#define SHENG_CLPC_P_ALL_MASK      0xf8ull  // cpu3..7 all preferred
#define SHENG_CLPC_P_NO_X3_MASK    0x78ull  // cpu3..6 preferred when X3 is hot
#define SHENG_CLPC_X3_MIGRATE_MASK 0x80ull  // cpu7 (Cortex-X3) rotated off

enum ShengClpcRole {
    SHENG_CLPC_ROLE_BACKGROUND_E = 0,  // SIP/launchd background daemons, storage, efficient/best-effort TGs -> E-cores
    SHENG_CLPC_ROLE_UI_CRITICAL  = 1,  // WindowServer, active UI app, critical/system/game -> P-cores (UT/BG on E)
    SHENG_CLPC_ROLE_DEFAULT      = 2,  // Unflagged helper coalitions -> P-cores normally, DF squeezed to E at ThermalCap L2+
};

struct ShengClpcDecision {
    uint8_t tg_preferred_pset;
    uint8_t overrides[SHENG_PC_MAX];
    uint8_t role;
};

static inline void sheng_clpc_decide(
    uint64_t tg_id,
    uint32_t tg_flags,
    unsigned thermal_tier,
    struct ShengClpcDecision *out)
{
    for (unsigned i = 0; i < SHENG_PC_MAX; ++i) {
        out->overrides[i] = SHENG_CLPC_PSET_NONE;
    }

    const uint32_t fg_mask = SHENG_TG_FLAG_CRITICAL | SHENG_TG_FLAG_UI_APP |
                             SHENG_TG_FLAG_APPLICATION | SHENG_TG_FLAG_GAME_MODE;
    const uint32_t bg_mask = SHENG_TG_FLAG_EFFICIENT | SHENG_TG_FLAG_BEST_EFFORT;

    // 1. Explicit background / storage / efficient / best-effort daemon coalitions (when not foreground/critical):
    //    steer all non-realtime buckets (including IN & DF, which otherwise hogged P-cores under SIP!) onto E-cores.
    if (tg_id == SHENG_TG_ID_BACKGROUND ||
        tg_id == SHENG_TG_ID_IO_STORAGE ||
        tg_id == SHENG_TG_ID_CELLULAR ||
        ((tg_flags & bg_mask) != 0 && (tg_flags & fg_mask) == 0 && tg_id != SHENG_TG_ID_SYSTEM)) {
        out->tg_preferred_pset = SHENG_CLPC_PSET_E;
        out->role = SHENG_CLPC_ROLE_BACKGROUND_E;
        out->overrides[SHENG_PC_REALTIME] = SHENG_CLPC_PSET_P;
        out->overrides[SHENG_PC_ABOVEUI]  = SHENG_CLPC_PSET_P;
        return;
    }

    // 2. System/kernel, WindowServer, foreground UI apps, critical services, and Game Mode:
    //    keep interactive/compute buckets on P-cores while offloading Utility & Background buckets to E-cores.
    if (tg_id == SHENG_TG_ID_SYSTEM || tg_id == SHENG_TG_ID_VM ||
        tg_id == SHENG_TG_ID_PERF_CTRL || (tg_flags & fg_mask) != 0) {
        out->tg_preferred_pset = SHENG_CLPC_PSET_P;
        out->role = SHENG_CLPC_ROLE_UI_CRITICAL;
        out->overrides[SHENG_PC_IDLE]       = SHENG_CLPC_PSET_E;
        out->overrides[SHENG_PC_BACKGROUND] = SHENG_CLPC_PSET_E;
        out->overrides[SHENG_PC_UTILITY]    = SHENG_CLPC_PSET_E;
        return;
    }

    // 3. Default unflagged daemon/XPC helper coalitions:
    //    P-cores for FG/IN/DF with UT/BG on E-cores; under closed-loop thermal squeeze (ThermalCap >= L2),
    //    demote NONUI (Default QoS) to E-cores as well so foreground UI retains cool P-core headroom.
    out->tg_preferred_pset = SHENG_CLPC_PSET_P;
    out->role = SHENG_CLPC_ROLE_DEFAULT;
    out->overrides[SHENG_PC_IDLE]       = SHENG_CLPC_PSET_E;
    out->overrides[SHENG_PC_BACKGROUND] = SHENG_CLPC_PSET_E;
    out->overrides[SHENG_PC_UTILITY]    = SHENG_CLPC_PSET_E;
    if (thermal_tier >= 1) {
        out->overrides[SHENG_PC_NONUI] = SHENG_CLPC_PSET_E;
    }
}

// Closed-loop thermal tier with hysteresis: enters squeeze at ThermalCap >= 2 (74 C Tj / 62 C aoss),
// exits once ThermalCap returns to 0.
static inline unsigned sheng_clpc_next_tier(unsigned current_tier, int32_t thermal_cap_level)
{
    if (thermal_cap_level >= 2) return 1u;
    if (thermal_cap_level <= 0) return 0u;
    return current_tier;
}

// Closed-loop Cortex-X3 (cpu7) hot-core die rotation with hysteresis:
// rotates threads off X3 onto the 4x A715/A710 cores when ThermalCap >= 2 or X3 >= 72 C and >= 4 C hotter than P-cluster;
// restores X3 when ThermalCap <= 1 and X3 <= 68 C.
static inline bool sheng_clpc_x3_rotate(
    bool currently_rotated,
    int32_t thermal_cap_level,
    int32_t p_cluster_mc,
    int32_t x3_mc,
    uint64_t *preferred_mask,
    uint64_t *migration_mask)
{
    bool rotate = currently_rotated;
    const bool valid_temps = (p_cluster_mc > 0 && x3_mc > 0);
    if (thermal_cap_level >= 2 || (valid_temps && x3_mc >= 72000 && (x3_mc - p_cluster_mc) >= 4000)) {
        rotate = true;
    } else if (thermal_cap_level <= 1 && (!valid_temps || x3_mc <= 68000)) {
        rotate = false;
    }
    *preferred_mask = rotate ? SHENG_CLPC_P_NO_X3_MASK : SHENG_CLPC_P_ALL_MASK;
    *migration_mask = rotate ? SHENG_CLPC_X3_MIGRATE_MASK : 0ull;
    return rotate;
}

#ifdef __cplusplus
}
#endif

#endif // SHENG_CLPC_POLICY_H

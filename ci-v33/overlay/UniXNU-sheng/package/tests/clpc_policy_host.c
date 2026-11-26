#include <assert.h>
#include <stdio.h>
#include "../ShengClpcPolicy.h"

int main(void)
{
    struct ShengClpcDecision d;

    // 1. THREAD_GROUP_BACKGROUND (id=1) steers DF and IN to E-cores (pset 0), keeping REALTIME on P (pset 1).
    sheng_clpc_decide(SHENG_TG_ID_BACKGROUND, 0, 0, &d);
    assert(d.role == SHENG_CLPC_ROLE_BACKGROUND_E);
    assert(d.tg_preferred_pset == SHENG_CLPC_PSET_E);
    assert(d.overrides[SHENG_PC_NONUI] == SHENG_CLPC_PSET_NONE);
    assert(d.overrides[SHENG_PC_USER_INITIATED] == SHENG_CLPC_PSET_NONE);
    assert(d.overrides[SHENG_PC_REALTIME] == SHENG_CLPC_PSET_P);

    // 2. Efficient / best-effort coalition steers to E-cores unless UI_APP/APPLICATION/CRITICAL is also set.
    sheng_clpc_decide(42, SHENG_TG_FLAG_EFFICIENT, 0, &d);
    assert(d.role == SHENG_CLPC_ROLE_BACKGROUND_E);
    assert(d.tg_preferred_pset == SHENG_CLPC_PSET_E);

    sheng_clpc_decide(42, SHENG_TG_FLAG_EFFICIENT | SHENG_TG_FLAG_UI_APP, 0, &d);
    assert(d.role == SHENG_CLPC_ROLE_UI_CRITICAL);
    assert(d.tg_preferred_pset == SHENG_CLPC_PSET_P);
    assert(d.overrides[SHENG_PC_UTILITY] == SHENG_CLPC_PSET_E);
    assert(d.overrides[SHENG_PC_BACKGROUND] == SHENG_CLPC_PSET_E);
    assert(d.overrides[SHENG_PC_NONUI] == SHENG_CLPC_PSET_NONE);

    // 3. Default helper coalition: NONUI stays on P at tier 0, shifts to E at tier 1 (ThermalCap >= L2), UI/CRITICAL unaffected.
    sheng_clpc_decide(99, 0, 0, &d);
    assert(d.role == SHENG_CLPC_ROLE_DEFAULT);
    assert(d.tg_preferred_pset == SHENG_CLPC_PSET_P);
    assert(d.overrides[SHENG_PC_NONUI] == SHENG_CLPC_PSET_NONE);

    sheng_clpc_decide(99, 0, 1, &d);
    assert(d.overrides[SHENG_PC_NONUI] == SHENG_CLPC_PSET_E);

    sheng_clpc_decide(100, SHENG_TG_FLAG_APPLICATION, 1, &d);
    assert(d.overrides[SHENG_PC_NONUI] == SHENG_CLPC_PSET_NONE);

    // 4. Hysteresis & X3 rotation bitmask disjointness ((preferred & migration) == 0).
    unsigned tier = sheng_clpc_next_tier(0, 1);
    assert(tier == 0);
    tier = sheng_clpc_next_tier(tier, 2);
    assert(tier == 1);
    tier = sheng_clpc_next_tier(tier, 1);
    assert(tier == 1);
    tier = sheng_clpc_next_tier(tier, 0);
    assert(tier == 0);

    uint64_t pref = 0, mig = 0;
    bool rot = sheng_clpc_x3_rotate(false, 0, 65000, 66000, &pref, &mig);
    assert(!rot && pref == 0xf8ull && mig == 0ull && (pref & mig) == 0);
    rot = sheng_clpc_x3_rotate(rot, 0, 67000, 73000, &pref, &mig);
    assert(rot && pref == 0x78ull && mig == 0x80ull && (pref & mig) == 0);
    rot = sheng_clpc_x3_rotate(rot, 1, 66000, 70000, &pref, &mig);
    assert(rot && pref == 0x78ull && mig == 0x80ull);
    rot = sheng_clpc_x3_rotate(rot, 0, 64000, 67000, &pref, &mig);
    assert(!rot && pref == 0xf8ull && mig == 0ull);

    printf("clpc_policy_host: all checks passed\n");
    return 0;
}

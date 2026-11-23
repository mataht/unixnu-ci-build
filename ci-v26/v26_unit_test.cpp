#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "NativeMetalObjects.h"
#include "QcomTeeWire.h"
#include "ShengBattmgr.h"
#include "TouchMouse.h"
#include "WifiHotspotIe.h"
#include "WifiRaFilter.h"
#include "WifiScanPolicy.h"
#include "Wn8030.h"

struct bp_panic_info { unsigned height, width, depth, stride; uintptr_t frame; };
static inline void bp_panic_bind(struct bp_panic_info *p, uintptr_t base, unsigned w, unsigned h, unsigned rb) {
    p->height = h; p->width = w; p->depth = 32; p->stride = rb;
    __atomic_store_n(&p->frame, base, __ATOMIC_RELEASE);
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// ---- Test 1: TouchMouse.h (89B descriptor, 1-finger tap/drag, 2-finger natural scroll, 2-finger right-click) ----
static void test_v26_touch_mouse() {
    assert(sizeof(kTouchMouseDescriptor) == 89);
    // Top-level Application Collection MUST be UsagePage(0x01 Generic Desktop), Usage(0x02 Mouse)
    assert(kTouchMouseDescriptor[0] == 0x05 && kTouchMouseDescriptor[1] == 0x01);
    assert(kTouchMouseDescriptor[2] == 0x09 && kTouchMouseDescriptor[3] == 0x02);

    assert(touch_mouse_scale(0, 30480) == 0);
    assert(touch_mouse_scale(30479, 30480) == 32767);
    assert(touch_mouse_scale(0, 20320) == 0);
    assert(touch_mouse_scale(20319, 20320) == 32767);

    TouchMouseState st;
    TouchMouseOut out;
    int xs[2] = {500, 0}, ys[2] = {200, 0};
    touch_mouse_step(&st, 1, xs, ys, &out);
    assert(out.count == 1 && out.reports[0][1] == 0);
    const uint16_t expX = touch_mouse_scale(500, 30480);
    const uint16_t expY = touch_mouse_scale(200, 20320);
    assert(rd_le16(&out.reports[0][2]) == expX && rd_le16(&out.reports[0][4]) == expY);

    touch_mouse_step(&st, 0, xs, ys, &out);
    assert(out.count == 2 && out.reports[0][1] == 1 && out.reports[1][1] == 0);

    // 2-finger tap right-click
    xs[0] = 4000; ys[0] = 4000;
    touch_mouse_step(&st, 1, xs, ys, &out);
    xs[1] = 4500; ys[1] = 4000;
    touch_mouse_step(&st, 2, xs, ys, &out);
    touch_mouse_step(&st, 1, xs, ys, &out);
    assert(out.count == 0 && st.buttons == 0);
    touch_mouse_step(&st, 0, xs, ys, &out);
    assert(out.count == 2 && out.reports[0][1] == 2 && out.reports[1][1] == 0);

    // 2-finger natural scroll
    xs[0] = 8000; ys[0] = 8000; xs[1] = 8600; ys[1] = 8000;
    touch_mouse_step(&st, 2, xs, ys, &out);
    ys[0] = 7840; ys[1] = 7840;
    touch_mouse_step(&st, 2, xs, ys, &out);
    assert(out.count == 1 && out.reports[0][1] == 0 && (int8_t)out.reports[0][6] == 2);
    touch_mouse_step(&st, 0, xs, ys, &out);
    assert(out.count == 0 && st.buttons == 0);
}

// ---- Test 2: Two-Phase Unconditional `_boot_progress_panic_vinfo` Binding (`enabled == 0`) ----
struct MockPanicBind {
    int enabled = 0;
    unsigned panic_anchor_pinned = 0;
    struct bp_panic_info panic_vinfo = {0, 0, 0, 0, 0};

    void bind_early(uintptr_t base, unsigned w, unsigned h, unsigned rb, unsigned depth) {
        if (!enabled) {
            if (base >= 0xffff000000000000ULL && depth == 32 && w >= 720 && w <= 4096 &&
                h >= 560 && h <= 4096 && rb >= w * 4 && rb <= 65536 &&
                base <= UINTPTR_MAX - (uint64_t)h * rb &&
                !__atomic_load_n(&panic_anchor_pinned, __ATOMIC_ACQUIRE)) {
                bp_panic_bind(&panic_vinfo, base, w, h, rb);
            }
            return;
        }
    }
    int pulse_anchor(uintptr_t base, uint64_t bytes, unsigned w, unsigned h, unsigned rb) {
        if (!enabled) {
            if (base >= 0xffff000000000000ULL && !(base & 3) && w >= 720 && w <= 4096 &&
                h >= 560 && h <= 4096 && rb >= w * 4 && !(rb & 3) && rb <= 65536 &&
                bytes >= (uint64_t)h * rb && base <= UINTPTR_MAX - (uint64_t)h * rb &&
                !__atomic_exchange_n(&panic_anchor_pinned, 1u, __ATOMIC_ACQ_REL)) {
                bp_panic_bind(&panic_vinfo, base, w, h, rb);
                return 1;
            }
            return 0;
        }
        return 0;
    }
};

static void test_v26_two_phase_panic_vinfo() {
    MockPanicBind m;
    const uintptr_t early_gop_va = 0xfffffe9000100000ULL;
    const uintptr_t retained_anchor_va = 0xfffffe9100200000ULL;
    const uintptr_t offscreen_iosurface_va = 0xfffffe9200300000ULL;

    // Phase 1: early XNU initialize_screen binds GOP VA even with enabled == 0
    m.bind_early(early_gop_va, 3048, 2032, 3048 * 4, 32);
    assert(m.panic_vinfo.frame == early_gop_va && m.panic_vinfo.depth == 32);

    // Phase 2: LegacyDisplayProbe registers permanent diagnosticAnchorMapping (returns 1 to retain mapping!)
    assert(m.pulse_anchor(retained_anchor_va, 3048ULL * 2032 * 4, 3048, 2032, 3048 * 4) == 1);
    assert(m.panic_vinfo.frame == retained_anchor_va && m.panic_anchor_pinned == 1);

    // Subsequent NativeLegacyDisplay setConsoleInfo(offscreen IOSurface) does NOT overwrite retained physical GOP!
    m.bind_early(offscreen_iosurface_va, 3048, 2032, 3048 * 4, 32);
    assert(m.panic_vinfo.frame == retained_anchor_va);
}

// ---- Test 3: ShengBattmgr un-coalesced 0xe4=0 -> ACK -> 0xe4=1 + #147 dual-gauge / wattage ----
static void test_v26_battmgr() {
    ShengBattmgr bm;
    sheng_battmgr_init(&bm);
    uint8_t req[SHENG_BATTMGR_REQUEST_BYTES];
    uint8_t ack[24];
    memset(ack, 0, sizeof(ack));
    wr_le32(ack, SHENG_BATTMGR_OWNER);
    wr_le32(ack + 4, SHENG_BATTMGR_TYPE_REQUEST);
    wr_le32(ack + 8, SHENG_BATTMGR_OP_BOARD_SET);
    wr_le32(ack + 12, SHENG_BATTMGR_KEYBOARD_PLUGIN);
    wr_le32(ack + 20, 0);

    sheng_battmgr_keyboard(&bm, 1);
    assert(sheng_battmgr_next(&bm, req) == SHENG_BATTMGR_REQUEST_BYTES && rd_le32(req + 20) == 1u);
    assert(sheng_battmgr_receive(&bm, ack, sizeof(ack)) == BmAnswer && bm.keyboard_acked == 1);

    assert(sheng_battmgr_next(&bm, req) == SHENG_BATTMGR_REQUEST_BYTES && bm.in_flight == 1);
    sheng_battmgr_keyboard_pulse_off(&bm);
    sheng_battmgr_keyboard(&bm, 0);
    sheng_battmgr_keyboard(&bm, 1);
    sheng_battmgr_timeout(&bm);

    assert(sheng_battmgr_next(&bm, req) == SHENG_BATTMGR_REQUEST_BYTES && rd_le32(req + 20) == 0u);
    assert(sheng_battmgr_receive(&bm, ack, sizeof(ack)) == BmAnswer && bm.keyboard_acked == 0 && bm.keyboard_force_off == 0);
    assert(sheng_battmgr_next(&bm, req) == SHENG_BATTMGR_REQUEST_BYTES && rd_le32(req + 20) == 1u);
    assert(sheng_battmgr_receive(&bm, ack, sizeof(ack)) == BmAnswer && bm.keyboard_acked == 1);
}

// ---- Test 4: absentd `-shutdownstall` unconditional fast-exit even when ppid == 1 and XPC_SERVICE_NAME is set ----
static int mock_absentd_gate(int ppid, const char *xpc_label, int argc, const char * const *argv) {
    const char *arg0 = (argc > 0 && argv && argv[0]) ? argv[0] : NULL;
    const char *arg0_base = arg0 ? strrchr(arg0, '/') : NULL;
    arg0_base = arg0_base ? arg0_base + 1 : arg0;
    for (int i = 1; i < argc; ++i) {
        if (argv && argv[i] && !strcmp(argv[i], "-shutdownstall")) return 0; // immediate exit 0
    }
    if (!xpc_label || !*xpc_label || (xpc_label[0] == '0' && !xpc_label[1])) return 0;
    if (arg0_base && !strcmp(arg0_base, "seputil") &&
        (ppid != 1 || strcmp(xpc_label, "com.apple.prngseedd") != 0)) return 0;
    return 1; // stays alive in dispatch_main()
}

static void test_v26_absentd_gate() {
    const char *early_spindump[] = {"/usr/sbin/spindump", "-shutdownstall", "2", "-timelimit", "5"};
    assert(mock_absentd_gate(1, "com.apple.spindump.shutdownstall", 5, early_spindump) == 0);
    const char *daemon_spindump[] = {"/usr/sbin/spindump"};
    assert(mock_absentd_gate(1, "com.apple.spindump", 1, daemon_spindump) == 1);
    const char *daemon_lskdd[] = {"/usr/libexec/lskdd"};
    assert(mock_absentd_gate(1, "com.apple.lskdd", 1, daemon_lskdd) == 1);
    const char *child_seputil[] = {"/usr/libexec/seputil", "--load"};
    assert(mock_absentd_gate(42, "com.apple.seputil", 2, child_seputil) == 0);
    const char *prngseedd[] = {"/usr/libexec/seputil", "--prng"};
    assert(mock_absentd_gate(1, "com.apple.prngseedd", 2, prngseedd) == 1);
}

int main() {
    test_v26_touch_mouse();
    test_v26_two_phase_panic_vinfo();
    test_v26_battmgr();
    test_v26_absentd_gate();
    printf("ALL_V26_CPP_UNIT_TESTS_PASSED\n");
    return 0;
}

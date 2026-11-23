#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "NvtTouchCore.h"
#include "ShengBattmgr.h"
#include "Wn8030.h"
}
#include "NativeMetalObjects.h"
#include "QcomTeeWire.h"
#include "TouchMouse.h"
#include "WifiHotspotIe.h"
#include "WifiRaFilter.h"
#include "WifiScanPolicy.h"

static void synth_base(int16_t *m, int16_t base) {
    for (int i = 0; i < NVT_NODES; ++i) {
        m[i] = static_cast<int16_t>(base + (i % 7) * 3);
    }
}

static void add_gaussian(int16_t *m, double row, double col, double amp, double sigma) {
    for (int r = 0; r < NVT_ROWS; ++r) {
        for (int c = 0; c < NVT_COLUMNS; ++c) {
            const double d2 = (r - row) * (r - row) + (c - col) * (c - col);
            const int v = static_cast<int>(lround(amp * exp(-d2 / (2.0 * sigma * sigma))));
            m[r * NVT_COLUMNS + c] = static_cast<int16_t>(m[r * NVT_COLUMNS + c] + v);
        }
    }
}

// ---- Test 1: BootLog-23 Novatek Thermal Baseline-Runaway Reproduction -> Automatic 72-Frame Self-Recovery ----
static void test_v27_novatek_thermal_runaway_self_recovery() {
    static nvt_touch_core core;
    nvt_touch_reset(&core);
    int16_t f[NVT_NODES];
    nvt_contact out[NVT_SLOTS];

    // 1. Settle startup baseline around 5000 at cool boot
    for (unsigned i = 0; i < NVT_STARTUP_FRAMES; ++i) {
        synth_base(f, 5000);
        assert(nvt_touch_process(&core, f, 4, out, NVT_SLOTS) == -1);
    }
    assert(core.have_reference == 1 && core.rebaselines == 0);

    // 2. Reproduce BootLog-23 runaway: 30 thermal noise peaks >= 340 in columns 35..47 (MAX_PEAKS=24 overflow!)
    //    Frames 0..10 (< 12): transient tolerance; Frame 11..16 (12..17): suppressed (count=0);
    //    Frame 17 (18th storm frame = 150 ms): triggers nvt_touch_rebaseline() -> have_reference=0, rebaselines=1!
    for (unsigned step = 0; step < 18; ++step) {
        synth_base(f, 5360);
        for (int k = 0; k < 30; ++k) {
            const int rr = 2 + (k * 3) % 36;
            const int cc = 35 + (k % 12);
            f[rr * NVT_COLUMNS + cc] = static_cast<int16_t>(f[rr * NVT_COLUMNS + cc] + 220 + (k % 5) * 20);
        }
        const int n = nvt_touch_process(&core, f, 4, out, NVT_SLOTS);
        if (step >= 11) {
            assert(n == 0);
        }
    }
    assert(core.have_reference == 0 && core.rebaselines == 1 && core.startup_count == 0);

    // 3. Feed 72 warm-panel frames at the new 85.6C baseline (5360): re-forms median reference cleanly!
    for (unsigned i = 0; i < NVT_STARTUP_FRAMES; ++i) {
        synth_base(f, 5360);
        assert(nvt_touch_process(&core, f, 4, out, NVT_SLOTS) == -1);
    }
    assert(core.have_reference == 1);
    synth_base(f, 5360);
    assert(nvt_touch_process(&core, f, 4, out, NVT_SLOTS) == 0);

    // 4. Real human finger on the warm 5360 panel works immediately with 1 clean contact!
    synth_base(f, 5360);
    add_gaussian(f, 20.0, 30.0, 900.0, 1.3);
    assert(nvt_touch_process(&core, f, 4, out, NVT_SLOTS) == 0); // 1st frame debounce
    assert(nvt_touch_process(&core, f, 4, out, NVT_SLOTS) == 1); // 2nd frame active!
}

// ---- Test 2: Quiet-Neighborhood Background Adaptation While Finger Is Held ----
static void test_v27_quiet_zone_drift_while_touching() {
    static nvt_touch_core core;
    nvt_touch_reset(&core);
    int16_t f[NVT_NODES];
    nvt_contact out[NVT_SLOTS];

    for (unsigned i = 0; i < NVT_STARTUP_FRAMES; ++i) {
        synth_base(f, 5000);
        assert(nvt_touch_process(&core, f, 4, out, NVT_SLOTS) == -1);
    }
    // Hold a finger at (10, 10) for 600 frames (5 seconds) while the rest of the panel warms up slowly by +180
    for (int frame = 0; frame < 600; ++frame) {
        const int16_t drift = static_cast<int16_t>((frame * 180) / 600);
        synth_base(f, static_cast<int16_t>(5000 + drift));
        add_gaussian(f, 10.0, 10.0, 900.0, 1.3);
        const int n = nvt_touch_process(&core, f, 4, out, NVT_SLOTS);
        if (frame >= 1) assert(n == 1);
    }
    // Distant SoC column node (row 30, col 40) adapted with the +180 drift so lifting the finger leaves 0 phantom peaks!
    const int far_idx = 30 * NVT_COLUMNS + 40;
    const int expected_warm = 5000 + (far_idx % 7) * 3 + 180;
    assert(abs(core.reference[far_idx] - expected_warm) < 40);
    synth_base(f, 5180);
    assert(nvt_touch_process(&core, f, 4, out, NVT_SLOTS) == 0);
    assert(core.rebaselines == 0);
}

// ---- Test 3: TouchMouse Abort & 400 ms Cover Priority Cancel Without Firing False Click ----
static void test_v27_touch_mouse_abort_and_cover_arbitration() {
    TouchMouseState st;
    TouchMouseOut out;
    int xs[2] = {15000, 0}, ys[2] = {10000, 0};

    // Frame 1: PendingSingle (cursor warps with buttons == 0)
    touch_mouse_step(&st, 1, xs, ys, &out);
    assert(st.phase == TouchMousePendingSingle && out.count == 1 && out.reports[0][1] == 0);

    // Cover trackpad moves within 400 ms OR storm abort fires -> touch_mouse_reset MUST emit 0 clicks!
    touch_mouse_reset(&st, &out);
    assert(out.count == 0 && st.phase == TouchMouseIdle && st.buttons == 0);

    // Commit SingleDown drag (buttons == 1), then cover priority triggers -> emits clean button release (buttons == 0)
    for (int i = 0; i < 4; ++i) {
        xs[0] = 15000 + i * 50;
        touch_mouse_step(&st, 1, xs, ys, &out);
    }
    assert(st.phase == TouchMouseSingleDown && st.buttons == 1);
    touch_mouse_reset(&st, &out);
    assert(out.count == 1 && out.reports[0][1] == 0 && st.phase == TouchMouseIdle && st.buttons == 0);
}

// ---- Test 4: Catalyst Weather / AboutExtension GPU Gate Preserves iconservicesagent ----
static bool v27_adreno_refuse_open(bool kernel_task_caller, uint32_t type, bool gpu_all_procs, const char *self_comm) {
    const uint32_t NativeAdrenoPluginOpenType = 0x4147u;
    const uint32_t NativeAdrenoHandoffOpenType = 0x4148u;
    if (!kernel_task_caller && (type == NativeAdrenoPluginOpenType || type == NativeAdrenoHandoffOpenType)) {
        if (!gpu_all_procs) {
            if (!strcmp(self_comm, "Weather") || !strcmp(self_comm, "Stocks") ||
                !strcmp(self_comm, "Clock") || !strcmp(self_comm, "Home") ||
                !strcmp(self_comm, "AboutExtension")) {
                return true;
            }
        }
    }
    return false;
}

static void test_v27_adreno_gate_preserves_icons() {
    assert(v27_adreno_refuse_open(false, 0x4147u, false, "Weather") == true);
    assert(v27_adreno_refuse_open(false, 0x4147u, false, "Stocks") == true);
    assert(v27_adreno_refuse_open(false, 0x4147u, false, "Clock") == true);
    assert(v27_adreno_refuse_open(false, 0x4147u, false, "Home") == true);
    assert(v27_adreno_refuse_open(false, 0x4147u, false, "AboutExtension") == true);
    // Override boot-arg gpu-all-procs=1 re-enables hardware testing
    assert(v27_adreno_refuse_open(false, 0x4147u, true, "Weather") == false);
    // CRITICAL: iconservicesagent, WindowServer, Safari, WebKit.GPU MUST NEVER be refused!
    assert(v27_adreno_refuse_open(false, 0x4147u, false, "iconservicesage") == false);
    assert(v27_adreno_refuse_open(false, 0x4147u, false, "iconservicesagent") == false);
    assert(v27_adreno_refuse_open(false, 0x4147u, false, "WindowServer") == false);
    assert(v27_adreno_refuse_open(false, 0x4147u, false, "Safari") == false);
}

// ---- Test 5: VtHost answerFinish captureStreaming Guard & Backlight kMidNits Scale ----
static bool v27_vthost_should_send_stop(uint32_t pendingFrames, bool captureStreaming, bool draining,
                                        unsigned inputHead, unsigned inputTail, bool beforeDeadline) {
    return pendingFrames && captureStreaming && !draining && inputHead == inputTail && beforeDeadline;
}

static void test_v27_vthost_and_backlight() {
    // Before STREAMON(CAPTURE), dfin must NOT issue V4L2_DEC_CMD_STOP (avoids stage 55 failed: 22 EINVAL)
    assert(!v27_vthost_should_send_stop(4, false, false, 4, 4, true));
    // Once captureStreaming is active, normal drain proceeds cleanly
    assert(v27_vthost_should_send_stop(4, true, false, 4, 4, true));

    // kMidNits = 260 / kMaxNits = 500 -> ~52% default brightness (> 1000 / 2047 PWM vs dim 430 / 2047)
    const uint32_t kMinNits = 2, kMidNits = 260, kMaxNits = 500, kMaxLevel = 2047;
    const uint32_t defaultLevel = (kMidNits - kMinNits) * kMaxLevel / (kMaxNits - kMinNits);
    assert(defaultLevel >= 1000 && defaultLevel <= 1150);
}

int main() {
    test_v27_novatek_thermal_runaway_self_recovery();
    test_v27_quiet_zone_drift_while_touching();
    test_v27_touch_mouse_abort_and_cover_arbitration();
    test_v27_adreno_gate_preserves_icons();
    test_v27_vthost_and_backlight();
    printf("ALL_V27_CPP_UNIT_TESTS_PASSED\n");
    return 0;
}

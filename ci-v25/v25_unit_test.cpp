#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "NativeMetalObjects.h"
#include "ShengBattmgr.h"
#include "TouchMouse.h"
#include "WifiHotspotIe.h"
#include "WifiRaFilter.h"
#include "WifiScanPolicy.h"

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

// ---- Test 1: TouchMouse.h (1-finger tap, 1-finger drag, 2-finger natural scroll, 2-finger right-click, mode reset) ----
static void test_v25_touch_mouse() {
    assert(sizeof(kTouchMouseDescriptor) == 89);

    // Boundary scaling: 0..30479 -> 0..32767, 0..20319 -> 0..32767
    assert(touch_mouse_scale(0, 30480) == 0);
    assert(touch_mouse_scale(30479, 30480) == 32767);
    assert(touch_mouse_scale(0, 20320) == 0);
    assert(touch_mouse_scale(20319, 20320) == 32767);

    // 1a. Quick 1-finger tap on Apple Menu (50.0px, 20.0px -> 500, 200 in 0.1px)
    TouchMouseState st;
    TouchMouseOut out;
    int xs[2] = {500, 0}, ys[2] = {200, 0};
    touch_mouse_step(&st, 1, xs, ys, &out);
    assert(out.count == 1 && out.reports[0][1] == 0); // hover warp, no premature click yet
    const uint16_t expX = touch_mouse_scale(500, 30480);
    const uint16_t expY = touch_mouse_scale(200, 20320);
    assert(rd_le16(&out.reports[0][2]) == expX && rd_le16(&out.reports[0][4]) == expY);

    // Lift after 1 frame -> click + release at exact target
    touch_mouse_step(&st, 0, xs, ys, &out);
    assert(out.count == 2);
    assert(out.reports[0][1] == 1 && rd_le16(&out.reports[0][2]) == expX);
    assert(out.reports[1][1] == 0 && rd_le16(&out.reports[1][2]) == expX);
    assert(st.phase == TouchMouseIdle && st.buttons == 0);

    // 1b. Single-finger drag (exceeds 8.0px = 80 units on frame 2)
    xs[0] = 1000; ys[0] = 1000;
    touch_mouse_step(&st, 1, xs, ys, &out);
    assert(out.count == 1 && out.reports[0][1] == 0);
    xs[0] = 1120; ys[0] = 1000; // +12.0px
    touch_mouse_step(&st, 1, xs, ys, &out);
    assert(st.phase == TouchMouseSingleDown && st.buttons == 1);
    assert(out.count == 2 && out.reports[0][1] == 1 && out.reports[1][1] == 1);

    // Mode switch reset while Button 1 is held -> cleanly emits button-up!
    touch_mouse_reset(&st, &out);
    assert(out.count == 1 && out.reports[0][1] == 0 && st.buttons == 0 && st.phase == TouchMouseIdle);

    // 1c. Two-finger tap where Finger 2 lands 1 frame after Finger 1 -> NO Button-1 click, emits Button-2 Right Click!
    xs[0] = 4000; ys[0] = 4000;
    touch_mouse_step(&st, 1, xs, ys, &out);
    assert(out.count == 1 && out.reports[0][1] == 0); // no left click!
    xs[1] = 4500; ys[1] = 4000;
    touch_mouse_step(&st, 2, xs, ys, &out);
    assert(st.phase == TouchMouseTwoFinger && out.count == 0);
    touch_mouse_step(&st, 2, xs, ys, &out);
    assert(out.count == 0);
    // Finger 2 lifts 1 frame before Finger 1 -> must NOT click Button 1!
    touch_mouse_step(&st, 1, xs, ys, &out);
    assert(out.count == 0 && st.buttons == 0);
    // All fingers lift -> emits Right Click (Button 2 = 0x02) + release!
    touch_mouse_step(&st, 0, xs, ys, &out);
    assert(out.count == 2 && out.reports[0][1] == 2 && out.reports[1][1] == 0);

    // 1d. Two-finger natural scroll: moving fingers UP (ys decreases by 160 = 16px) -> positive Wheel (+2), zero click on lift!
    xs[0] = 8000; ys[0] = 8000; xs[1] = 8600; ys[1] = 8000;
    touch_mouse_step(&st, 2, xs, ys, &out);
    ys[0] = 7840; ys[1] = 7840; // moved UP 16px
    touch_mouse_step(&st, 2, xs, ys, &out);
    assert(out.count == 1 && out.reports[0][1] == 0);
    assert((int8_t)out.reports[0][6] == 2 && (int8_t)out.reports[0][7] == 0);
    // Lift after scrolling -> must NOT emit right click!
    touch_mouse_step(&st, 0, xs, ys, &out);
    assert(out.count == 0 && st.buttons == 0);
}

// ---- Test 2: Real ShengBattmgr.c un-coalesced 0xe4=0 -> ACK -> 0xe4=1 cover power cycle ----
static void test_v25_battmgr_pulse_off() {
    ShengBattmgr bm;
    sheng_battmgr_init(&bm);
    uint8_t req[SHENG_BATTMGR_REQUEST_BYTES];
    uint8_t ack[24];
    memset(ack, 0, sizeof(ack));
    wr_le32(ack, SHENG_BATTMGR_OWNER);
    wr_le32(ack + 4, SHENG_BATTMGR_TYPE_REQUEST);
    wr_le32(ack + 8, SHENG_BATTMGR_OP_BOARD_SET);
    wr_le32(ack + 12, SHENG_BATTMGR_KEYBOARD_PLUGIN);
    wr_le32(ack + 20, 0); // success result = 0

    // Initial attach: wanted=1 -> sends 0xe4=1 -> ACK
    sheng_battmgr_keyboard(&bm, 1);
    assert(sheng_battmgr_next(&bm, req) == SHENG_BATTMGR_REQUEST_BYTES);
    assert(rd_le32(req + 8) == SHENG_BATTMGR_OP_BOARD_SET && rd_le32(req + 16) == SHENG_BATTMGR_KEYBOARD_PLUGIN);
    assert(rd_le32(req + 20) == 1u);
    assert(sheng_battmgr_receive(&bm, ack, sizeof(ack)) == BmAnswer);
    assert(bm.keyboard_acked == 1);

    // Start a normal battery field read so bm.in_flight == 1 when cover cycle pulses 0 -> 1 rapidly!
    assert(sheng_battmgr_next(&bm, req) == SHENG_BATTMGR_REQUEST_BYTES);
    assert(bm.in_flight == 1);

    // Rapid 0 -> 1 pulse arrives while battery field read is in flight!
    sheng_battmgr_keyboard_pulse_off(&bm);
    sheng_battmgr_keyboard(&bm, 0);
    sheng_battmgr_keyboard(&bm, 1);
    assert(bm.keyboard_wanted == 1 && bm.keyboard_force_off == 1);

    // Finish the in-flight battery field read via timeout so next slot opens
    sheng_battmgr_timeout(&bm);
    assert(bm.in_flight == 0);

    // MUST send 0xe4 = 0 FIRST (never coalesced away by keyboard_wanted == 1)!
    assert(sheng_battmgr_next(&bm, req) == SHENG_BATTMGR_REQUEST_BYTES);
    assert(rd_le32(req + 8) == SHENG_BATTMGR_OP_BOARD_SET);
    assert(rd_le32(req + 16) == SHENG_BATTMGR_KEYBOARD_PLUGIN);
    assert(rd_le32(req + 20) == 0u);
    assert(sheng_battmgr_receive(&bm, ack, sizeof(ack)) == BmAnswer);
    assert(bm.keyboard_acked == 0 && bm.keyboard_force_off == 0);

    // Immediately next call MUST send 0xe4 = 1 to power cover WN8012 back up!
    assert(sheng_battmgr_next(&bm, req) == SHENG_BATTMGR_REQUEST_BYTES);
    assert(rd_le32(req + 8) == SHENG_BATTMGR_OP_BOARD_SET);
    assert(rd_le32(req + 16) == SHENG_BATTMGR_KEYBOARD_PLUGIN);
    assert(rd_le32(req + 20) == 1u);
    assert(sheng_battmgr_receive(&bm, ack, sizeof(ack)) == BmAnswer);
    assert(bm.keyboard_acked == 1 && bm.keyboard_sets == 3);
}

// ---- Test 3: Keyboard 240s MiAuth Refresh & 500ms Bounce Debounce Logic ----
struct MockCoverCycle {
    bool attached = true, detachPending = false, keyOrBtnHeld = false;
    uint8_t cyclePhase = 0;
    uint64_t coverEpochMs = 0, lastInputMs = 0, cycleDueMs = 0, detachDueMs = 0;
    uint32_t authSinceEpoch = 0, coverCycles = 0, hidTerminates = 0, detaches = 0;

    void startCycle(uint64_t now) {
        if (!attached || cyclePhase != 0) return;
        cyclePhase = 1;
        cycleDueMs = now + 350;
        detachPending = false;
        keyOrBtnHeld = false;
    }
    void onAttachFrame(int att, uint64_t now) {
        if (att == 1) {
            detachPending = false;
            if (!attached) { attached = true; coverEpochMs = now; authSinceEpoch = 0; }
            else if (cyclePhase != 0) { cyclePhase = 0; coverEpochMs = now; authSinceEpoch = 0; }
        } else if (att == 0 && attached && cyclePhase == 0) {
            keyOrBtnHeld = false;
            if (!detachPending) { detachPending = true; detachDueMs = now + 500; }
        }
    }
    void onAuthReq(uint64_t now) {
        if (attached && cyclePhase == 0) {
            ++authSinceEpoch;
            if (authSinceEpoch >= 4 || (now - coverEpochMs) >= 220000) startCycle(now);
        }
    }
    void tick(uint64_t now) {
        if (detachPending && cyclePhase == 0 && now >= detachDueMs) {
            attached = false; detachPending = false; ++detaches;
            // Persistent HID: hidTerminates stays 0!
        }
        if (cyclePhase == 1 && now >= cycleDueMs) {
            cyclePhase = 2; cycleDueMs = now + 450; coverEpochMs = now; authSinceEpoch = 0; ++coverCycles;
        } else if (cyclePhase == 2 && now >= cycleDueMs) {
            cyclePhase = 0; coverEpochMs = now; authSinceEpoch = 0;
        } else if (attached && cyclePhase == 0) {
            uint64_t age = now - coverEpochMs, idle = now - lastInputMs;
            if (age >= 232000 || (age >= 180000 && !keyOrBtnHeld && idle >= 1500)) startCycle(now);
        }
    }
};

static void test_v25_keyboard_miauth_and_debounce() {
    MockCoverCycle c;
    // 120ms Pogo bounce -> absorbed by 500ms debounce, 0 detaches, 0 HID terminates!
    c.onAttachFrame(0, 10000);
    assert(c.detachPending);
    c.onAttachFrame(1, 10120);
    c.tick(10600);
    assert(c.attached && !c.detachPending && c.detaches == 0 && c.hidTerminates == 0);

    // Held key at 181s suppresses proactive cycle until key is released & idle >= 1500ms!
    c.keyOrBtnHeld = true;
    c.lastInputMs = 181000;
    c.tick(181500);
    assert(c.cyclePhase == 0);

    // Release key and go idle 1.6s at 183s -> proactive 350ms refresh cycle fires invisibly before 240s mute!
    c.keyOrBtnHeld = false;
    c.lastInputMs = 181400;
    c.tick(183000);
    assert(c.cyclePhase == 1);
    c.tick(183350);
    assert(c.cyclePhase == 2 && c.coverCycles == 1);
    c.onAttachFrame(1, 183500);
    assert(c.cyclePhase == 0 && c.coverEpochMs == 183500 && c.authSinceEpoch == 0 && c.hidTerminates == 0);

    // Continuous typing past 180s -> 4th 0x24 auth request triggers immediate cycle before permanent lockup!
    c.keyOrBtnHeld = true;
    c.onAuthReq(243500);
    c.onAuthReq(303500);
    c.onAuthReq(363500);
    assert(c.cyclePhase == 0);
    c.onAuthReq(403500);
    assert(c.cyclePhase == 1);
}

int main() {
    test_v25_touch_mouse();
    test_v25_battmgr_pulse_off();
    test_v25_keyboard_miauth_and_debounce();
    printf("ALL_V25_CPP_UNIT_TESTS_PASSED\n");
    return 0;
}

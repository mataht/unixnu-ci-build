// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native macOS Absolute Pointer + Two-Finger Smooth Scroll / Right-Click translator for the 3048x2032 touchscreen.
// Matches in-kernel IOHIDEventDriver (SupportsIOFastPath=true, score 2000 -> dispatchAbsolutePointerEvent + handleScrollReport).
#ifndef SHENG_TOUCH_MOUSE_H
#define SHENG_TOUCH_MOUSE_H

#include <stddef.h>
#include <stdint.h>

static const unsigned char kTouchMouseDescriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,             // UsagePage(Generic Desktop), Usage(Mouse), Collection(Application)
    0x85, 0x01,                                     //   Report ID (1)
    0x09, 0x01, 0xa1, 0x00,                         //   Usage(Pointer), Collection(Physical)
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02,             //     UsagePage(Button), UsageMin(1), UsageMax(2)
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, //     Logical(0..1), 2 x 1-bit
    0x81, 0x02,                                     //     Input(Data,Var,Abs)
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03,             //     Input(Const) 6-bit pad
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,             //     UsagePage(Generic Desktop), Usage(X), Usage(Y)
    0x15, 0x00, 0x26, 0xff, 0x7f,                   //     Logical(0..32767)
    0x35, 0x00, 0x46, 0xff, 0x7f,                   //     Physical(0..32767)
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,             //     Input(Data,Var,Abs) 2 x uint16
    0x35, 0x00, 0x45, 0x00,                         //     Reset Physical Min/Max
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7f,             //     Usage(Wheel), Logical(-127..127)
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06,             //     Input(Data,Var,Rel) int8 vertical scroll
    0x05, 0x0c, 0x0a, 0x38, 0x02,                   //     UsagePage(Consumer), Usage(AC Pan 0x0238)
    0x15, 0x81, 0x25, 0x7f,                         //     Logical(-127..127)
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06,             //     Input(Data,Var,Rel) int8 horizontal scroll
    0xc0, 0xc0
};

enum {
    kTouchMouseReportBytes = 8,
    kTouchMouseAxisXMax = 30480,
    kTouchMouseAxisYMax = 20320,
    kTouchMouseDragUnits = 80,      // 8.0 px in 0.1px units
    kTouchMouseScrollUnits = 80,    // 8.0 px per wheel step
    kTouchMouseTapMaxUnits = 120,   // 12.0 px max travel for 2-finger right-click tap
    kTouchMousePendingFrames = 3,   // ~24 ms @ 120 Hz before committing single-finger left press
    kTouchMouseRightTapFrames = 30  // ~250 ms @ 120 Hz max duration for 2-finger right-click tap
};

enum TouchMousePhase {
    TouchMouseIdle = 0,
    TouchMousePendingSingle = 1,
    TouchMouseSingleDown = 2,
    TouchMouseTwoFinger = 3
};

struct TouchMouseOut {
    uint8_t count;
    uint8_t reports[3][kTouchMouseReportBytes];
};

struct TouchMouseState {
    uint8_t phase = TouchMouseIdle;
    uint8_t buttons = 0;
    uint16_t lastX = 0, lastY = 0;
    uint16_t startX = 0, startY = 0;
    int32_t rawStartX = 0, rawStartY = 0;
    int32_t prevCx = 0, prevCy = 0;
    int32_t scrollX = 0, scrollY = 0;
    uint32_t frames = 0;
    uint32_t maxTravel = 0;
    bool scrolled = false;
};

static inline uint16_t touch_mouse_scale(int32_t value, int32_t maximum) {
    if (value <= 0 || maximum <= 1) return 0;
    if (value >= maximum - 1) return 32767u;
    return static_cast<uint16_t>((static_cast<int64_t>(value) * 32767 + (maximum - 1) / 2) / (maximum - 1));
}

static inline int32_t touch_mouse_abs32(int32_t v) {
    return v < 0 ? -v : v;
}

static inline int8_t touch_mouse_take_scroll(int32_t *accum) {
    int32_t steps = *accum / kTouchMouseScrollUnits;
    if (steps > 127) steps = 127;
    if (steps < -127) steps = -127;
    *accum -= steps * kTouchMouseScrollUnits;
    return static_cast<int8_t>(steps);
}

static inline void touch_mouse_emit(TouchMouseState *s, TouchMouseOut *out, uint8_t buttons, uint16_t x, uint16_t y,
                                    int8_t wheel, int8_t pan) {
    if (out->count >= 3) return;
    uint8_t *r = out->reports[out->count++];
    r[0] = 1;
    r[1] = buttons;
    r[2] = static_cast<uint8_t>(x & 0xffu);
    r[3] = static_cast<uint8_t>((x >> 8) & 0xffu);
    r[4] = static_cast<uint8_t>(y & 0xffu);
    r[5] = static_cast<uint8_t>((y >> 8) & 0xffu);
    r[6] = static_cast<uint8_t>(wheel);
    r[7] = static_cast<uint8_t>(pan);
    s->buttons = buttons;
    s->lastX = x;
    s->lastY = y;
}

static inline void touch_mouse_reset(TouchMouseState *s, TouchMouseOut *out) {
    out->count = 0;
    if (s->buttons != 0) {
        touch_mouse_emit(s, out, 0, s->lastX, s->lastY, 0, 0);
    }
    s->phase = TouchMouseIdle;
    s->scrollX = 0;
    s->scrollY = 0;
    s->frames = 0;
    s->maxTravel = 0;
    s->scrolled = false;
}

static inline void touch_mouse_step(TouchMouseState *s, unsigned fingerCount, const int *xs, const int *ys, TouchMouseOut *out) {
    out->count = 0;
    if (fingerCount == 0) {
        if (s->phase == TouchMousePendingSingle) {
            // Quick single-finger tap (< kTouchMousePendingFrames): click + release at touch-down point.
            touch_mouse_emit(s, out, 1, s->startX, s->startY, 0, 0);
            touch_mouse_emit(s, out, 0, s->startX, s->startY, 0, 0);
        } else if (s->phase == TouchMouseSingleDown) {
            touch_mouse_emit(s, out, 0, s->lastX, s->lastY, 0, 0);
        } else if (s->phase == TouchMouseTwoFinger) {
            if (!s->scrolled && s->frames <= kTouchMouseRightTapFrames && s->maxTravel < kTouchMouseTapMaxUnits) {
                touch_mouse_emit(s, out, 2, s->startX, s->startY, 0, 0);
                touch_mouse_emit(s, out, 0, s->startX, s->startY, 0, 0);
            } else if (s->buttons != 0) {
                touch_mouse_emit(s, out, 0, s->lastX, s->lastY, 0, 0);
            }
        } else if (s->buttons != 0) {
            touch_mouse_emit(s, out, 0, s->lastX, s->lastY, 0, 0);
        }
        s->phase = TouchMouseIdle;
        s->scrollX = 0;
        s->scrollY = 0;
        s->frames = 0;
        s->maxTravel = 0;
        s->scrolled = false;
        return;
    }

    if (fingerCount >= 2 && s->phase != TouchMouseSingleDown) {
        const int32_t cx = (xs[0] + xs[1]) / 2;
        const int32_t cy = (ys[0] + ys[1]) / 2;
        if (s->phase != TouchMouseTwoFinger) {
            s->phase = TouchMouseTwoFinger;
            if (s->frames == 0) {
                s->startX = touch_mouse_scale(xs[0], kTouchMouseAxisXMax);
                s->startY = touch_mouse_scale(ys[0], kTouchMouseAxisYMax);
                touch_mouse_emit(s, out, 0, s->startX, s->startY, 0, 0);
            }
            s->rawStartX = cx;
            s->rawStartY = cy;
            s->prevCx = cx;
            s->prevCy = cy;
            s->scrollX = 0;
            s->scrollY = 0;
            s->maxTravel = 0;
            s->scrolled = false;
            ++s->frames;
            return;
        }
        ++s->frames;
        const uint32_t dist = static_cast<uint32_t>(touch_mouse_abs32(cx - s->rawStartX) + touch_mouse_abs32(cy - s->rawStartY));
        if (dist > s->maxTravel) s->maxTravel = dist;
        // Natural scroll sign matches Wn8030.c: finger moving up (cy < prevCy) -> positive sy; moving right (cx > prevCx) -> positive sx.
        s->scrollY += (s->prevCy - cy);
        s->scrollX += (cx - s->prevCx);
        s->prevCx = cx;
        s->prevCy = cy;
        const int8_t sy = touch_mouse_take_scroll(&s->scrollY);
        const int8_t sx = touch_mouse_take_scroll(&s->scrollX);
        if (sy != 0 || sx != 0) {
            s->scrolled = true;
            touch_mouse_emit(s, out, 0, s->lastX, s->lastY, sy, sx);
        }
        return;
    }

    // Single-finger contact (or secondary finger landed during an already-committed SingleDown drag: track finger 0).
    const int32_t rx = xs[0], ry = ys[0];
    const uint16_t sx = touch_mouse_scale(rx, kTouchMouseAxisXMax);
    const uint16_t sy = touch_mouse_scale(ry, kTouchMouseAxisYMax);

    if (s->phase == TouchMouseTwoFinger) {
        // One of the two fingers lifted at the end of a scroll/two-finger gesture: wait for full lift without clicking.
        return;
    }

    if (s->phase == TouchMouseIdle) {
        s->phase = TouchMousePendingSingle;
        s->startX = sx;
        s->startY = sy;
        s->rawStartX = rx;
        s->rawStartY = ry;
        s->frames = 1;
        s->maxTravel = 0;
        // Warp cursor immediately to touch point without pressing Button 1 yet (in case 2nd finger lands next frame).
        touch_mouse_emit(s, out, 0, sx, sy, 0, 0);
        return;
    }

    if (s->phase == TouchMousePendingSingle) {
        ++s->frames;
        const int32_t travel = touch_mouse_abs32(rx - s->rawStartX) + touch_mouse_abs32(ry - s->rawStartY);
        if (travel >= kTouchMouseDragUnits || s->frames >= kTouchMousePendingFrames) {
            s->phase = TouchMouseSingleDown;
            touch_mouse_emit(s, out, 1, s->startX, s->startY, 0, 0);
            if (sx != s->startX || sy != s->startY) {
                touch_mouse_emit(s, out, 1, sx, sy, 0, 0);
            }
        }
        return;
    }

    // TouchMouseSingleDown
    ++s->frames;
    if (sx != s->lastX || sy != s->lastY || s->buttons != 1) {
        touch_mouse_emit(s, out, 1, sx, sy, 0, 0);
    }
}

#endif  // SHENG_TOUCH_MOUSE_H

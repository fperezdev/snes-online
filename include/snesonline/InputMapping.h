#pragma once

#include <cstdint>

#include "snesonline/InputBits.h"

namespace snesonline {

struct Stick2f {
    float x;
    float y;
};

// Android MotionEvent AXIS_X/Y typically: x right = +, y down = +.
inline uint16_t mapAndroidAxesToDpad(Stick2f stick, float deadzone = 0.35f) noexcept {
    uint16_t mask = 0;

    if (stick.x <= -deadzone) mask |= SNES_LEFT;
    else if (stick.x >= deadzone) mask |= SNES_RIGHT;

    if (stick.y <= -deadzone) mask |= SNES_UP;       // y negative = up
    else if (stick.y >= deadzone) mask |= SNES_DOWN; // y positive = down

    return mask;
}

// iOS GameController extendedGamepad thumbsticks: y up = +.
inline uint16_t mapIOSThumbstickToDpad(Stick2f stick, float deadzone = 0.35f) noexcept {
    uint16_t mask = 0;

    if (stick.x <= -deadzone) mask |= SNES_LEFT;
    else if (stick.x >= deadzone) mask |= SNES_RIGHT;

    if (stick.y >= deadzone) mask |= SNES_UP;        // y positive = up
    else if (stick.y <= -deadzone) mask |= SNES_DOWN; // y negative = down

    return mask;
}

// Utility: ensure you never set both directions on one axis.
inline uint16_t sanitizeDpad(uint16_t mask) noexcept {
    const bool left = (mask & SNES_LEFT) != 0;
    const bool right = (mask & SNES_RIGHT) != 0;
    if (left && right) mask &= ~(SNES_LEFT | SNES_RIGHT);

    const bool up = (mask & SNES_UP) != 0;
    const bool down = (mask & SNES_DOWN) != 0;
    if (up && down) mask &= ~(SNES_UP | SNES_DOWN);

    return mask;
}

} // namespace snesonline

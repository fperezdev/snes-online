#pragma once

#include <cstdint>

namespace snesonline {

// 16-bit SNES-style bitmask.
// Bit positions chosen to match the user's ordering.
enum SnesInputBit : uint16_t {
    SNES_B      = (1u << 0),
    SNES_Y      = (1u << 1),
    SNES_SELECT = (1u << 2),
    SNES_START  = (1u << 3),
    SNES_UP     = (1u << 4),
    SNES_DOWN   = (1u << 5),
    SNES_LEFT   = (1u << 6),
    SNES_RIGHT  = (1u << 7),
    SNES_A      = (1u << 8),
    SNES_X      = (1u << 9),
    SNES_L      = (1u << 10),
    SNES_R      = (1u << 11),
};

} // namespace snesonline

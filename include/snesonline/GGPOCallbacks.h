#pragma once

#include "snesonline/GGPOFwd.h"

namespace snesonline {

// GGPO callback glue; implemented in src/GGPOCallbacks.cpp
struct GGPOCallbacks {
    static GGPOSessionCallbacks make() noexcept;

    // GGPO callbacks are plain C function pointers. For callbacks which need to call
    // back into GGPO (e.g. advance_frame during rollback), we keep a single active
    // session pointer here.
    static void setActiveSession(GGPOSession* session) noexcept;

    struct EventState {
        bool running = false;
        bool connectionInterrupted = false;
        bool disconnected = false;
        // If >0, GGPO suggests we sleep to let the other side catch up.
        int timesyncFramesAhead = 0;
    };

    // Returns and clears the latest event state since the last call.
    static EventState drainEvents() noexcept;
};

} // namespace snesonline

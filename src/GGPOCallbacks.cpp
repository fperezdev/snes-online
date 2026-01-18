#include "snesonline/GGPOCallbacks.h"

#include "snesonline/EmulatorEngine.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <atomic>

namespace snesonline {

#if defined(SNESONLINE_ENABLE_GGPO) && SNESONLINE_ENABLE_GGPO

static GGPOSession* g_activeSession = nullptr;

static std::atomic<bool> g_evRunning{false};
static std::atomic<bool> g_evInterrupted{false};
static std::atomic<bool> g_evDisconnected{false};
static std::atomic<int> g_evTimesyncFramesAhead{0};

// NOTE: These functions are written to match common GGPO callback signatures.
// Depending on your GGPO fork/version, you may need small signature tweaks.

static bool __cdecl begin_game_cb(const char* /*game*/) {
    return true;
}

static bool __cdecl save_game_state_cb(unsigned char** buffer, int* len, int* checksum, int /*frame*/) {
    if (!buffer || !len || !checksum) return false;

    SaveState state;
    if (!EmulatorEngine::instance().saveState(state)) return false;

    // GGPO expects to own the buffer and later call free_buffer.
    unsigned char* out = static_cast<unsigned char*>(std::malloc(state.sizeBytes));
    if (!out) return false;
    std::memcpy(out, state.buffer.data(), state.sizeBytes);

    *buffer = out;
    *len = static_cast<int>(state.sizeBytes);
    *checksum = static_cast<int>(state.checksum);
    return true;
}

static bool __cdecl load_game_state_cb(unsigned char* buffer, int len) {
    if (!buffer || len <= 0) return false;

    SaveState state;
    if (!state.buffer.allocate(static_cast<std::size_t>(len), 64)) return false;
    std::memcpy(state.buffer.data(), buffer, static_cast<std::size_t>(len));
    state.sizeBytes = static_cast<std::size_t>(len);

    return EmulatorEngine::instance().loadState(state);
}

static bool __cdecl log_game_state_cb(char* /*filename*/, unsigned char* /*buffer*/, int /*len*/) {
    return true;
}

static void __cdecl free_buffer_cb(void* buffer) {
    std::free(buffer);
}

static bool __cdecl advance_frame_cb(int /*flags*/) {
    // Called by GGPO during rollback/catch-up. We must advance exactly one frame
    // using synchronized inputs, then notify GGPO that we advanced.
    if (!g_activeSession) return true;

    int disconnectFlags = 0;
    uint16_t inputs[2] = {0, 0};
    if (ggpo_synchronize_input(g_activeSession, inputs, static_cast<int>(sizeof(inputs)), &disconnectFlags) != GGPO_OK) {
        return true;
    }

    EmulatorEngine::instance().setInputMask(0, inputs[0]);
    EmulatorEngine::instance().setInputMask(1, inputs[1]);
    EmulatorEngine::instance().advanceFrame();

    ggpo_advance_frame(g_activeSession);
    return true;
}

static bool __cdecl on_event_cb_impl(GGPOEvent* info) {
    if (!info) return true;

    switch (info->code) {
        case GGPO_EVENTCODE_RUNNING:
            g_evRunning.store(true, std::memory_order_relaxed);
            g_evInterrupted.store(false, std::memory_order_relaxed);
            g_evDisconnected.store(false, std::memory_order_relaxed);
            break;
        case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
            g_evInterrupted.store(true, std::memory_order_relaxed);
            break;
        case GGPO_EVENTCODE_CONNECTION_RESUMED:
            g_evInterrupted.store(false, std::memory_order_relaxed);
            g_evRunning.store(true, std::memory_order_relaxed);
            break;
        case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
            g_evDisconnected.store(true, std::memory_order_relaxed);
            g_evRunning.store(false, std::memory_order_relaxed);
            break;
        case GGPO_EVENTCODE_TIMESYNC:
            g_evTimesyncFramesAhead.store(info->u.timesync.frames_ahead, std::memory_order_relaxed);
            break;
    }

    return true;
}

GGPOSessionCallbacks GGPOCallbacks::make() noexcept {
    GGPOSessionCallbacks cb{};
    cb.begin_game = &begin_game_cb;
    cb.save_game_state = &save_game_state_cb;
    cb.load_game_state = &load_game_state_cb;
    cb.log_game_state = &log_game_state_cb;
    cb.free_buffer = &free_buffer_cb;
    cb.advance_frame = &advance_frame_cb;
    cb.on_event = &on_event_cb_impl;
    return cb;
}

void GGPOCallbacks::setActiveSession(GGPOSession* session) noexcept {
    g_activeSession = session;
}

GGPOCallbacks::EventState GGPOCallbacks::drainEvents() noexcept {
    EventState st{};
    st.running = g_evRunning.exchange(false, std::memory_order_relaxed);
    st.connectionInterrupted = g_evInterrupted.exchange(false, std::memory_order_relaxed);
    st.disconnected = g_evDisconnected.exchange(false, std::memory_order_relaxed);
    st.timesyncFramesAhead = g_evTimesyncFramesAhead.exchange(0, std::memory_order_relaxed);
    return st;
}

#else

GGPOSessionCallbacks GGPOCallbacks::make() noexcept {
    // GGPO disabled: return an empty callback table.
    return {};
}

void GGPOCallbacks::setActiveSession(GGPOSession* /*session*/) noexcept {
}

GGPOCallbacks::EventState GGPOCallbacks::drainEvents() noexcept {
    return {};
}

#endif

} // namespace snesonline

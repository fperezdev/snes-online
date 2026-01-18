#pragma once

#include <cstdint>
#include <chrono>
#include <string>

#include "snesonline/GGPOFwd.h"

namespace snesonline {

// Thin wrapper around GGPO session lifetime + input exchange.
// This is intentionally minimal; your app is expected to wire transport + UI.
class NetplaySession {
public:
    NetplaySession() noexcept;
    ~NetplaySession() noexcept;

    NetplaySession(const NetplaySession&) = delete;
    NetplaySession& operator=(const NetplaySession&) = delete;

    struct Config {
        const char* gameName = "snes-online";
        // If empty and localPlayerNum==1, the session can listen and auto-discover the peer from the first UDP packet.
        const char* remoteIp = "127.0.0.1";
        uint16_t remotePort = 7000;
        uint16_t localPort = 7000;
        // Must be 1 or 2. The other side should use the opposite.
        uint8_t localPlayerNum = 1;
    };

    bool start(const Config& cfg) noexcept;
    void stop() noexcept;

    // If netplay is enabled but the peer isn't running yet, GGPO will not provide synchronized inputs.
    // Use these to drive basic UI messaging (e.g., window title / one-time dialog).
    bool hasSynchronized() const noexcept { return hasSynchronized_; }
    bool waitingForPeer() const noexcept { return waitingForPeer_; }
    bool disconnected() const noexcept { return disconnected_; }
    bool reconnecting() const noexcept { return reconnecting_; }

    // Called once per frame by the platform loop.
    // For now, this only advances the emulator frame with synchronized inputs.
    void tick() noexcept;

    // App feeds local input here; remote input comes from GGPO.
    void setLocalInput(uint16_t mask) noexcept;

private:
    struct OwnedConfig {
        std::string gameName;
        std::string remoteIp;
        uint16_t remotePort = 7000;
        uint16_t localPort = 7000;
        uint8_t localPlayerNum = 1;
    } lastCfg_;

    GGPOSession* session_ = nullptr;
    uint16_t localMask_ = 0;

    // Last synchronized inputs for the current frame.
    uint16_t syncedMasks_[2] = {0, 0};

    // GGPO player handles are usually int; keep as int to avoid hard GGPO deps in headers.
    int localPlayerHandle_ = -1;
    int remotePlayerHandle_ = -1;

    bool hasSynchronized_ = false;
    bool waitingForPeer_ = false;
    bool disconnected_ = false;
    bool reconnecting_ = false;
    bool interrupted_ = false;

#if defined(SNESONLINE_ENABLE_GGPO) && SNESONLINE_ENABLE_GGPO
#if defined(_WIN32)
    using UdpSocketHandle = uint64_t;
    static constexpr UdpSocketHandle kInvalidUdpSocket = ~0ull;
#else
    using UdpSocketHandle = int;
    static constexpr UdpSocketHandle kInvalidUdpSocket = -1;
#endif
    UdpSocketHandle listenSock_ = kInvalidUdpSocket;
    bool listenForPeer_ = false;

    bool startGgpoSession_() noexcept;
    void closeListenSocket_() noexcept;
    void pollListenSocket_() noexcept;
#endif

    std::chrono::steady_clock::time_point nextReconnectAttempt_{};
    uint32_t reconnectBackoffMs_ = 1000;
};

} // namespace snesonline

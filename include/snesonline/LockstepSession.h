#pragma once

#include <cstdint>
#include <chrono>
#include <string>

namespace snesonline {

// Simple UDP lockstep netplay compatible with the Android implementation.
// Packet format:
//   u32 frame (big-endian)
//   u16 inputMask (big-endian)
//   u16 reserved (0)
class LockstepSession {
public:
    LockstepSession() noexcept;
    ~LockstepSession() noexcept;

    LockstepSession(const LockstepSession&) = delete;
    LockstepSession& operator=(const LockstepSession&) = delete;

    struct Config {
        // If empty and localPlayerNum==1, the session will auto-discover the peer from the first UDP packet.
        const char* remoteHost = "";
        uint16_t remotePort = 7000;
        uint16_t localPort = 7000;
        uint8_t localPlayerNum = 1; // 1 or 2

        // Optional: server-assisted first connection (UDP hole-punch helper).
        // If enabled, the session will send a UDP rendezvous message to the room server and, if it
        // receives a peer endpoint, it will use that endpoint to initiate the first P2P handshake.
        // Protocol is implemented by tools/room_server/server.js (UDP: "SNO_PUNCH1 CODE").
        bool serverAssistFirstConnect = false;
        const char* roomServerHost = ""; // hostname or IPv4
        uint16_t roomServerPort = 0; // 0 => disable
        const char* roomCode = ""; // 8-12 chars
    };

    bool start(const Config& cfg) noexcept;
    void stop() noexcept;

    void setLocalInput(uint16_t mask) noexcept;
    void tick() noexcept;

    bool waitingForPeer() const noexcept { return waitingForPeer_; }
    bool connected() const noexcept { return connected_; }

    uint64_t recvCount() const noexcept { return recvCount_; }
    // Returns -1 if never received anything.
    int64_t lastRecvAgeMs() const noexcept;

    uint32_t localFrame() const noexcept { return frame_; }
    uint32_t lastRemoteFrame() const noexcept { return lastRemoteFrame_; }
    uint32_t maxRemoteFrame() const noexcept { return maxRemoteFrame_; }

    // For UI/debug.
    std::string peerEndpoint() const;

private:
#if defined(_WIN32)
    using SocketHandle = uint64_t;
    static constexpr SocketHandle kInvalidSocket = ~0ull;
#else
    using SocketHandle = int;
    static constexpr SocketHandle kInvalidSocket = -1;
#endif

    bool openSocket_(uint16_t localPort) noexcept;
    void closeSocket_() noexcept;
    void pumpRecv_() noexcept;
    void sendLocal_() noexcept;

    struct Peer {
        uint32_t ipv4_be = 0; // network order
        uint16_t port_be = 0; // network order
        bool valid() const noexcept { return ipv4_be != 0 && port_be != 0; }
    } peer_;

    bool discoverPeer_ = false;

    SocketHandle sock_ = kInvalidSocket;
    uint16_t localPort_ = 7000;
    uint16_t remotePort_ = 7000;
    uint8_t localPlayerNum_ = 1;

    uint16_t localMask_ = 0;
    uint32_t frame_ = 0;

    static constexpr uint32_t kBufN = 256;
    uint16_t remoteMask_[kBufN] = {};
    uint32_t remoteFrameTag_[kBufN] = {};

    uint16_t sentMask_[kBufN] = {};
    uint32_t sentFrameTag_[kBufN] = {};

    bool waitingForPeer_ = false;
    bool connected_ = false;
    std::chrono::steady_clock::time_point lastRecv_{};

    // Used to prevent running faster than real-time when catch-up is enabled.
    std::chrono::steady_clock::time_point startTime_{};

    uint64_t recvCount_ = 0;

    uint32_t lastRemoteFrame_ = 0;
    uint32_t maxRemoteFrame_ = 0;
};

} // namespace snesonline

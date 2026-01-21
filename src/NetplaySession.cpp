#include "snesonline/NetplaySession.h"

#include "snesonline/EmulatorEngine.h"
#include "snesonline/GGPOCallbacks.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <mutex>

#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <WinSock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#endif

#if defined(SNESONLINE_ENABLE_GGPO) && SNESONLINE_ENABLE_GGPO
#include <ggponet.h>
#endif

namespace snesonline {

#if defined(_WIN32)
static bool ensureWinSockInitialized() noexcept {
    static std::once_flag once;
    static int startupResult = -1;

    std::call_once(once, []() {
        WSADATA wsa{};
        startupResult = WSAStartup(MAKEWORD(2, 2), &wsa);
    });

    return startupResult == 0;
}
#endif

static bool isValidIpv4Address(const char* ip) noexcept {
    if (!ip || !ip[0]) return false;

    in_addr addr{};
#if defined(_WIN32)
    return InetPtonA(AF_INET, ip, &addr) == 1;
#else
    return inet_pton(AF_INET, ip, &addr) == 1;
#endif
}

static std::string resolveToIpv4String(const std::string& hostOrIp) {
    if (hostOrIp.empty()) return {};
    if (isValidIpv4Address(hostOrIp.c_str())) return hostOrIp;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(hostOrIp.c_str(), nullptr, &hints, &res) != 0 || !res) return {};

    std::string out;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (!ai->ai_addr || ai->ai_family != AF_INET) continue;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
        char buf[INET_ADDRSTRLEN] = {};
#if defined(_WIN32)
        if (InetNtopA(AF_INET, const_cast<IN_ADDR*>(&sin->sin_addr), buf, INET_ADDRSTRLEN)) {
            out = buf;
            break;
        }
#else
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, INET_ADDRSTRLEN)) {
            out = buf;
            break;
        }
#endif
    }

    freeaddrinfo(res);
    return out;
}

#if defined(SNESONLINE_ENABLE_GGPO) && SNESONLINE_ENABLE_GGPO

#if !defined(_WIN32)
#include <unistd.h>
#include <fcntl.h>
#endif

void NetplaySession::closeListenSocket_() noexcept {
    if (listenSock_ == kInvalidUdpSocket) return;

#if defined(_WIN32)
    const SOCKET s = static_cast<SOCKET>(listenSock_);
    closesocket(s);
#else
    close(listenSock_);
#endif
    listenSock_ = kInvalidUdpSocket;
    listenForPeer_ = false;
}

static bool setSocketNonBlocking(
#if defined(_WIN32)
    SOCKET s
#else
    int s
#endif
) noexcept {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool NetplaySession::startGgpoSession_() noexcept {
#if defined(_WIN32)
    if (!ensureWinSockInitialized()) {
        return false;
    }
#endif

    GGPOSessionCallbacks cb = GGPOCallbacks::make();

    const int localPlayerNum = (lastCfg_.localPlayerNum == 2) ? 2 : 1;
    const int remotePlayerNum = (localPlayerNum == 1) ? 2 : 1;

    const char* gameName = lastCfg_.gameName.c_str();
    const int localPort = static_cast<int>(lastCfg_.localPort ? lastCfg_.localPort : 7000);
    const int frameDelay = static_cast<int>(lastCfg_.frameDelay);

    GGPOErrorCode err = ggpo_start_session(&session_, &cb, gameName, 2, static_cast<int>(sizeof(uint16_t)), localPort);
    if (err != GGPO_OK) {
        session_ = nullptr;
        return false;
    }

    GGPOCallbacks::setActiveSession(session_);

    GGPOPlayer p1{};
    p1.size = sizeof(p1);
    p1.type = GGPO_PLAYERTYPE_LOCAL;
    p1.player_num = localPlayerNum;
    GGPOPlayerHandle p1Handle{};
    err = ggpo_add_player(session_, &p1, &p1Handle);
    if (err != GGPO_OK) {
        stop();
        return false;
    }
    localPlayerHandle_ = static_cast<int>(p1Handle);

    if (frameDelay > 0) {
        // Best-effort: this simply adds local input latency to reduce visible rollbacks.
        (void)ggpo_set_frame_delay(session_, p1Handle, frameDelay);
    }

    GGPOPlayer p2{};
    p2.size = sizeof(p2);
    p2.type = GGPO_PLAYERTYPE_REMOTE;
    p2.player_num = remotePlayerNum;

    const std::string resolvedIp = resolveToIpv4String(lastCfg_.remoteIp);
    if (resolvedIp.empty() || lastCfg_.remotePort == 0) {
        stop();
        return false;
    }
    std::strncpy(p2.u.remote.ip_address, resolvedIp.c_str(), sizeof(p2.u.remote.ip_address) - 1);
    p2.u.remote.ip_address[sizeof(p2.u.remote.ip_address) - 1] = '\0';
    p2.u.remote.port = static_cast<unsigned short>(lastCfg_.remotePort);

    GGPOPlayerHandle p2Handle{};
    err = ggpo_add_player(session_, &p2, &p2Handle);
    if (err != GGPO_OK) {
        stop();
        return false;
    }
    remotePlayerHandle_ = static_cast<int>(p2Handle);

    ggpo_set_disconnect_timeout(session_, 3000);
    ggpo_set_disconnect_notify_start(session_, 750);

    syncedMasks_[0] = 0;
    syncedMasks_[1] = 0;
    hasSynchronized_ = false;
    waitingForPeer_ = true;
    disconnected_ = false;
    reconnecting_ = false;
    interrupted_ = false;
    return true;
}

void NetplaySession::pollListenSocket_() noexcept {
    if (!listenForPeer_ || session_ || listenSock_ == kInvalidUdpSocket) return;

#if defined(_WIN32)
    const SOCKET s = static_cast<SOCKET>(listenSock_);
    sockaddr_in from{};
    int fromLen = sizeof(from);
    char buf[256];
    const int n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n <= 0) {
        return;
    }

    char ipBuf[INET_ADDRSTRLEN] = {};
    if (!InetNtopA(AF_INET, const_cast<IN_ADDR*>(&from.sin_addr), ipBuf, INET_ADDRSTRLEN)) {
        return;
    }
    const uint16_t port = static_cast<uint16_t>(ntohs(from.sin_port));
    if (!ipBuf[0] || port == 0) {
        return;
    }

    // Lock onto the first peer we observe.
    lastCfg_.remoteIp = ipBuf;
    lastCfg_.remotePort = port;
    closeListenSocket_();

    // Now that we have a concrete endpoint, start GGPO.
    (void)startGgpoSession_();
#else
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    char buf[256];
    const int n = recvfrom(listenSock_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n <= 0) {
        return;
    }

    char ipBuf[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &from.sin_addr, ipBuf, INET_ADDRSTRLEN)) {
        return;
    }
    const uint16_t port = static_cast<uint16_t>(ntohs(from.sin_port));
    if (!ipBuf[0] || port == 0) {
        return;
    }

    lastCfg_.remoteIp = ipBuf;
    lastCfg_.remotePort = port;
    closeListenSocket_();
    (void)startGgpoSession_();
#endif
}

#endif // SNESONLINE_ENABLE_GGPO

NetplaySession::NetplaySession() noexcept = default;
NetplaySession::~NetplaySession() noexcept { stop(); }

bool NetplaySession::start(const Config& cfg) noexcept {
    stop();

    lastCfg_.gameName = (cfg.gameName && cfg.gameName[0]) ? cfg.gameName : "snes-online";
    // Allow empty remoteIp for host auto-discovery.
    lastCfg_.remoteIp = (cfg.remoteIp != nullptr) ? cfg.remoteIp : std::string();
    lastCfg_.remotePort = cfg.remotePort;
    lastCfg_.localPort = cfg.localPort;
    lastCfg_.frameDelay = cfg.frameDelay;
    lastCfg_.localPlayerNum = (cfg.localPlayerNum == 2) ? 2 : 1;

    reconnectBackoffMs_ = 1000;
    nextReconnectAttempt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(reconnectBackoffMs_);

#if defined(SNESONLINE_ENABLE_GGPO) && SNESONLINE_ENABLE_GGPO
    const int localPort = static_cast<int>(lastCfg_.localPort ? lastCfg_.localPort : 7000);

    // If host (Player 1) doesn't know the peer yet, listen for the first incoming UDP packet and auto-discover.
    if (lastCfg_.remoteIp.empty()) {
        if (lastCfg_.localPlayerNum != 1) {
            // Avoid deadlock: if both sides are waiting without a target, nothing will happen.
            return false;
        }

#if defined(_WIN32)
        if (!ensureWinSockInitialized()) {
            return false;
        }
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<u_short>(localPort));
        if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            closesocket(s);
            return false;
        }
        (void)setSocketNonBlocking(s);
        listenSock_ = static_cast<UdpSocketHandle>(s);
#else
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(localPort));
        if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(s);
            return false;
        }
        (void)setSocketNonBlocking(s);
        listenSock_ = s;
#endif

        listenForPeer_ = true;
        hasSynchronized_ = false;
        waitingForPeer_ = true;
        disconnected_ = false;
        reconnecting_ = false;
        interrupted_ = false;
        return true;
    }

    return startGgpoSession_();
#else
    (void)cfg;
    return false;
#endif
}

void NetplaySession::stop() noexcept {
#if defined(SNESONLINE_ENABLE_GGPO) && SNESONLINE_ENABLE_GGPO
    closeListenSocket_();
    if (session_) {
        GGPOCallbacks::setActiveSession(nullptr);
        ggpo_close_session(session_);
        session_ = nullptr;
    }
#endif

    localPlayerHandle_ = -1;
    remotePlayerHandle_ = -1;
    syncedMasks_[0] = 0;
    syncedMasks_[1] = 0;
    hasSynchronized_ = false;
    waitingForPeer_ = false;
    disconnected_ = false;
    reconnecting_ = false;
    interrupted_ = false;
}

void NetplaySession::setLocalInput(uint16_t mask) noexcept {
    localMask_ = mask;
}

void NetplaySession::tick() noexcept {
#if defined(SNESONLINE_ENABLE_GGPO) && SNESONLINE_ENABLE_GGPO
    if (listenForPeer_ && !session_) {
        pollListenSocket_();
        // Freeze simulation until a peer is discovered.
        waitingForPeer_ = true;
        return;
    }
    if (!session_) {
        waitingForPeer_ = false;
        EmulatorEngine::instance().setLocalInputMask(localMask_);
        EmulatorEngine::instance().advanceFrame();
        return;
    }

    // Drain GGPO events and convert them into app behavior.
    const auto ev = GGPOCallbacks::drainEvents();
    if (ev.timesyncFramesAhead > 0) {
        const int ms = (1000 * ev.timesyncFramesAhead) / 60;
        if (ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }
    if (ev.connectionInterrupted) {
        interrupted_ = true;
        waitingForPeer_ = true;
    }
    if (ev.disconnected) {
        disconnected_ = true;
        reconnecting_ = true;
        interrupted_ = false;
        waitingForPeer_ = true;
    }
    if (ev.running) {
        disconnected_ = false;
        reconnecting_ = false;
        interrupted_ = false;
        waitingForPeer_ = false;
    }

    // Freeze simulation on network interruption to avoid divergence.
    if (interrupted_) {
        ggpo_idle(session_, 1);
        return;
    }

    // Auto-reconnect by restarting the session on both ends.
    if (reconnecting_) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextReconnectAttempt_) {
            Config c{};
            c.gameName = lastCfg_.gameName.c_str();
            c.remoteIp = lastCfg_.remoteIp.c_str();
            c.remotePort = lastCfg_.remotePort;
            c.localPort = lastCfg_.localPort;
            c.frameDelay = lastCfg_.frameDelay;
            c.localPlayerNum = lastCfg_.localPlayerNum;

            const bool ok = start(c);
            if (!ok) {
                reconnectBackoffMs_ = (reconnectBackoffMs_ < 8000) ? (reconnectBackoffMs_ * 2) : 8000;
                nextReconnectAttempt_ = now + std::chrono::milliseconds(reconnectBackoffMs_);
            }
        }

        if (session_) ggpo_idle(session_, 1);
        return;
    }

    // Submit local input (16-bit mask) for the local player.
    // GGPO may reject input during synchronization; treat that as "no tick".
    GGPOErrorCode err = ggpo_add_local_input(
        session_,
        static_cast<GGPOPlayerHandle>(localPlayerHandle_),
        &localMask_,
        static_cast<int>(sizeof(localMask_)));

    if (err != GGPO_OK) {
        // Usually means we're not synchronized yet (peer not running / handshake pending).
        waitingForPeer_ = true;
        // Let GGPO process network/timeouts.
        ggpo_idle(session_, 1);
        return;
    }

    // Pull synchronized inputs for both players.
    int disconnectFlags = 0;
    uint16_t inputs[2] = {0, 0};
    err = ggpo_synchronize_input(session_, inputs, static_cast<int>(sizeof(inputs)), &disconnectFlags);
    if (err != GGPO_OK) {
        waitingForPeer_ = true;
        ggpo_idle(session_, 1);
        return;
    }

    hasSynchronized_ = true;
    waitingForPeer_ = false;

    // Feed EmulatorEngine ports then advance exactly one frame.
    syncedMasks_[0] = inputs[0];
    syncedMasks_[1] = inputs[1];
    EmulatorEngine::instance().setInputMask(0, syncedMasks_[0]);
    EmulatorEngine::instance().setInputMask(1, syncedMasks_[1]);

    EmulatorEngine::instance().advanceFrame();

    // Notify GGPO that we advanced exactly one frame.
    ggpo_advance_frame(session_);

    // Allow GGPO to pump internal state.
    ggpo_idle(session_, 0);
    return;
#endif
    EmulatorEngine::instance().setLocalInputMask(localMask_);
    EmulatorEngine::instance().advanceFrame();
}

} // namespace snesonline

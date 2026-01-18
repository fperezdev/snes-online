#include "snesonline/LockstepSession.h"

#include "snesonline/EmulatorEngine.h"

#include <cstdint>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace snesonline {

namespace {

#if defined(_WIN32)
static bool ensureWinSockInitialized() noexcept {
    static bool ok = false;
    static bool tried = false;
    if (tried) return ok;
    tried = true;
    WSADATA wsa{};
    ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    return ok;
}
#endif

static bool isValidIpv4Address(const char* ip) noexcept {
    if (!ip || !ip[0]) return false;
#if defined(_WIN32)
    IN_ADDR addr{};
    return InetPtonA(AF_INET, ip, &addr) == 1;
#else
    in_addr addr{};
    return inet_pton(AF_INET, ip, &addr) == 1;
#endif
}

static bool resolveIpv4ToSockaddr(const char* hostOrIp, sockaddr_in& out, uint16_t port) noexcept {
    if (!hostOrIp || !hostOrIp[0]) return false;

    // Fast path: numeric IPv4.
    if (isValidIpv4Address(hostOrIp)) {
        out = {};
        out.sin_family = AF_INET;
        out.sin_port = htons(port);
#if defined(_WIN32)
        InetPtonA(AF_INET, hostOrIp, &out.sin_addr);
#else
        inet_pton(AF_INET, hostOrIp, &out.sin_addr);
#endif
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* res = nullptr;
    if (getaddrinfo(hostOrIp, nullptr, &hints, &res) != 0 || !res) return false;

    bool ok = false;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (!ai->ai_addr || ai->ai_family != AF_INET) continue;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
        out = *sin;
        out.sin_port = htons(port);
        ok = true;
        break;
    }

    freeaddrinfo(res);
    return ok;
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

#pragma pack(push, 1)
struct Packet {
    uint32_t frame_be;
    uint16_t mask_be;
    uint16_t reserved_be;
};
#pragma pack(pop)

static bool parsePeerLine(const char* data, int len, sockaddr_in& outPeer) noexcept {
    if (!data || len <= 0) return false;
    // Expect: "SNO_PEER1 ip port\n"
    if (len < 10) return false;
    if (std::memcmp(data, "SNO_PEER1", 9) != 0) return false;

    // Copy into a small NUL-terminated buffer for sscanf.
    char buf[128] = {};
    const int n = (len >= static_cast<int>(sizeof(buf))) ? (static_cast<int>(sizeof(buf)) - 1) : len;
    std::memcpy(buf, data, static_cast<size_t>(n));
    buf[n] = '\0';

    char ip[64] = {};
    int port = 0;
    if (std::sscanf(buf, "SNO_PEER1 %63s %d", ip, &port) != 2) return false;
    if (port < 1 || port > 65535) return false;

    sockaddr_in peer{};
    if (!resolveIpv4ToSockaddr(ip, peer, static_cast<uint16_t>(port))) return false;
    outPeer = peer;
    return true;
}

static bool doServerAssistPunch(
#if defined(_WIN32)
    uint64_t sock,
#else
    int sock,
#endif
    const char* roomServerHost,
    uint16_t roomServerPort,
    const char* roomCode,
    sockaddr_in& outPeer
) noexcept {
    if (!roomServerHost || !roomServerHost[0] || roomServerPort == 0) return false;
    if (!roomCode || !roomCode[0]) return false;

    sockaddr_in server{};
    if (!resolveIpv4ToSockaddr(roomServerHost, server, roomServerPort)) return false;

    char msg[64] = {};
    std::snprintf(msg, sizeof(msg), "SNO_PUNCH1 %s\n", roomCode);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    auto nextSend = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextSend) {
#if defined(_WIN32)
            SOCKET s = static_cast<SOCKET>(sock);
            (void)sendto(s, msg, static_cast<int>(std::strlen(msg)), 0, reinterpret_cast<const sockaddr*>(&server), sizeof(server));
#else
            (void)sendto(sock, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr*>(&server), sizeof(server));
#endif
            nextSend = now + std::chrono::milliseconds(250);
        }

        // Poll for replies.
        for (int i = 0; i < 8; ++i) {
            char buf[256] = {};
            sockaddr_in from{};
#if defined(_WIN32)
            int fromLen = sizeof(from);
            SOCKET s = static_cast<SOCKET>(sock);
            const int n = recvfrom(s, buf, static_cast<int>(sizeof(buf)), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
#else
            socklen_t fromLen = sizeof(from);
            const int n = static_cast<int>(recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
#endif
            if (n <= 0) break;

            // Only accept replies from the room server IP.
            if (from.sin_addr.s_addr != server.sin_addr.s_addr) continue;

            sockaddr_in peer{};
            if (parsePeerLine(buf, n, peer)) {
                outPeer = peer;
                return true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return false;
}

} // namespace

static constexpr uint32_t kInputDelayFrames = 5;
static constexpr uint32_t kResendWindow = 16;
static constexpr int kSocketBufBytes = 1 << 20;

LockstepSession::LockstepSession() noexcept {
    // Mark tags as invalid.
    for (uint32_t& t : remoteFrameTag_) t = 0xFFFFFFFFu;
    for (uint32_t& t : sentFrameTag_) t = 0xFFFFFFFFu;
}

LockstepSession::~LockstepSession() noexcept { stop(); }

bool LockstepSession::openSocket_(uint16_t localPort) noexcept {
    closeSocket_();

#if defined(_WIN32)
    if (!ensureWinSockInitialized()) return false;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&kSocketBufBytes), sizeof(kSocketBufBytes));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&kSocketBufBytes), sizeof(kSocketBufBytes));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(static_cast<u_short>(localPort));

    if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        closesocket(s);
        return false;
    }

    (void)setSocketNonBlocking(s);
    sock_ = static_cast<SocketHandle>(s);
    return true;
#else
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &kSocketBufBytes, sizeof(kSocketBufBytes));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &kSocketBufBytes, sizeof(kSocketBufBytes));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(localPort);

    if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        ::close(s);
        return false;
    }

    (void)setSocketNonBlocking(s);
    sock_ = s;
    return true;
#endif
}

void LockstepSession::closeSocket_() noexcept {
    if (sock_ == kInvalidSocket) return;
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(sock_));
#else
    ::close(sock_);
#endif
    sock_ = kInvalidSocket;
}

bool LockstepSession::start(const Config& cfg) noexcept {
    stop();

    localPort_ = (cfg.localPort != 0) ? cfg.localPort : 7000;
    remotePort_ = (cfg.remotePort != 0) ? cfg.remotePort : 7000;
    localPlayerNum_ = (cfg.localPlayerNum == 2) ? 2 : 1;

    for (uint32_t& t : remoteFrameTag_) t = 0xFFFFFFFFu;
    std::memset(remoteMask_, 0, sizeof(remoteMask_));
    for (uint32_t& t : sentFrameTag_) t = 0xFFFFFFFFu;
    std::memset(sentMask_, 0, sizeof(sentMask_));
    frame_ = 0;

    // Prime our local input history for the first few frames. With input delay, the first
    // kInputDelayFrames frames would otherwise have no locally-buffered input.
    for (uint32_t f = 0; f < kInputDelayFrames; ++f) {
        const uint32_t idx = f % kBufN;
        sentFrameTag_[idx] = f;
        sentMask_[idx] = 0;
    }

    peer_ = {};
    discoverPeer_ = false;
    waitingForPeer_ = true;
    connected_ = false;
    lastRecv_ = {};

    const bool hasRemoteHost = (cfg.remoteHost && cfg.remoteHost[0]);
    if (!hasRemoteHost) {
        if (localPlayerNum_ != 1) {
            // Avoid deadlock: both sides can't be waiting without a target.
            return false;
        }
        discoverPeer_ = true;
    } else {
        sockaddr_in remote{};
        if (!resolveIpv4ToSockaddr(cfg.remoteHost, remote, remotePort_)) {
            return false;
        }
        peer_.ipv4_be = remote.sin_addr.s_addr;
        peer_.port_be = remote.sin_port;
    }

    if (!openSocket_(localPort_)) {
        return false;
    }

    // Optional: server-assisted first connection (UDP punch helper).
    if (cfg.serverAssistFirstConnect && cfg.roomServerPort != 0 && cfg.roomServerHost && cfg.roomServerHost[0] && cfg.roomCode && cfg.roomCode[0]) {
        sockaddr_in peer{};
        if (doServerAssistPunch(sock_, cfg.roomServerHost, cfg.roomServerPort, cfg.roomCode, peer)) {
            peer_.ipv4_be = peer.sin_addr.s_addr;
            peer_.port_be = peer.sin_port;
            discoverPeer_ = false;
            waitingForPeer_ = false;
        }
    }

    startTime_ = std::chrono::steady_clock::now();

    // If peer is already configured, we can start sending immediately.
    if (!discoverPeer_ && peer_.valid()) {
        waitingForPeer_ = false;
    }

    return true;
}

void LockstepSession::stop() noexcept {
    closeSocket_();
    peer_ = {};
    discoverPeer_ = false;
    waitingForPeer_ = false;
    connected_ = false;
    localMask_ = 0;
    frame_ = 0;
    startTime_ = {};

    for (uint32_t& t : remoteFrameTag_) t = 0xFFFFFFFFu;
    std::memset(remoteMask_, 0, sizeof(remoteMask_));
    for (uint32_t& t : sentFrameTag_) t = 0xFFFFFFFFu;
    std::memset(sentMask_, 0, sizeof(sentMask_));
}

void LockstepSession::setLocalInput(uint16_t mask) noexcept { localMask_ = mask; }

void LockstepSession::pumpRecv_() noexcept {
    if (sock_ == kInvalidSocket) return;

#if defined(_WIN32)
    SOCKET s = static_cast<SOCKET>(sock_);
    while (true) {
        Packet p{};
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(s, reinterpret_cast<char*>(&p), static_cast<int>(sizeof(p)), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) break;
        if (n != static_cast<int>(sizeof(p))) continue;

        // Learn/refresh peer endpoint from observed packets (NATs may rewrite source ports).
        if (discoverPeer_) {
            if (!peer_.valid()) {
                peer_.ipv4_be = from.sin_addr.s_addr;
                peer_.port_be = from.sin_port;
            } else if (peer_.ipv4_be == from.sin_addr.s_addr) {
                peer_.port_be = from.sin_port;
            }
        } else {
            if (peer_.valid() && peer_.ipv4_be == from.sin_addr.s_addr) {
                peer_.port_be = from.sin_port;
            }
        }

        const uint32_t f = ntohl(p.frame_be);
        const uint16_t m = ntohs(p.mask_be);
        const uint32_t idx = f % kBufN;
        remoteFrameTag_[idx] = f;
        remoteMask_[idx] = m;

        lastRemoteFrame_ = f;
        if (recvCount_ == 0 || f > maxRemoteFrame_) maxRemoteFrame_ = f;

        connected_ = true;
        waitingForPeer_ = false;
        lastRecv_ = std::chrono::steady_clock::now();
        recvCount_++;
    }
#else
    while (true) {
        Packet p{};
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        const int n = static_cast<int>(recvfrom(sock_, &p, sizeof(p), 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
        if (n <= 0) break;
        if (n != static_cast<int>(sizeof(p))) continue;

        // Learn/refresh peer endpoint from observed packets (NATs may rewrite source ports).
        if (discoverPeer_) {
            if (!peer_.valid()) {
                peer_.ipv4_be = from.sin_addr.s_addr;
                peer_.port_be = from.sin_port;
            } else if (peer_.ipv4_be == from.sin_addr.s_addr) {
                peer_.port_be = from.sin_port;
            }
        } else {
            if (peer_.valid() && peer_.ipv4_be == from.sin_addr.s_addr) {
                peer_.port_be = from.sin_port;
            }
        }

        const uint32_t f = ntohl(p.frame_be);
        const uint16_t m = ntohs(p.mask_be);
        const uint32_t idx = f % kBufN;
        remoteFrameTag_[idx] = f;
        remoteMask_[idx] = m;

        lastRemoteFrame_ = f;
        if (recvCount_ == 0 || f > maxRemoteFrame_) maxRemoteFrame_ = f;

        connected_ = true;
        waitingForPeer_ = false;
        lastRecv_ = std::chrono::steady_clock::now();
        recvCount_++;
    }
#endif

    if (discoverPeer_ && peer_.valid()) {
        // Once discovered, stop being in waiting state.
        waitingForPeer_ = false;
    }
}

int64_t LockstepSession::lastRecvAgeMs() const noexcept {
    if (recvCount_ == 0) return -1;
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRecv_).count();
    return static_cast<int64_t>(ms);
}

void LockstepSession::sendLocal_() noexcept {
    if (sock_ == kInvalidSocket) return;

    const uint32_t targetFrame = frame_ + kInputDelayFrames;
    const uint32_t sidx = targetFrame % kBufN;
    sentFrameTag_[sidx] = targetFrame;
    sentMask_[sidx] = localMask_;

    // Host in discover mode must wait until a peer is known.
    if (discoverPeer_ && !peer_.valid()) return;
    if (!peer_.valid()) return;

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = peer_.ipv4_be;
    to.sin_port = peer_.port_be;

    const uint32_t start = (targetFrame >= (kResendWindow - 1)) ? (targetFrame - (kResendWindow - 1)) : 0u;
    for (uint32_t f = start; f <= targetFrame; ++f) {
        const uint32_t i = f % kBufN;
        if (sentFrameTag_[i] != f) continue;
        Packet p{};
        p.frame_be = htonl(f);
        p.mask_be = htons(sentMask_[i]);
        p.reserved_be = 0;

#if defined(_WIN32)
        SOCKET s = static_cast<SOCKET>(sock_);
        sendto(s, reinterpret_cast<const char*>(&p), static_cast<int>(sizeof(p)), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#else
        sendto(sock_, &p, sizeof(p), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#endif
    }
}

void LockstepSession::tick() noexcept {
    // Pump network.
    pumpRecv_();

    // Time-based pacing: only simulate frames that are "due" according to wall clock.
    // This preserves ~60fps average while still allowing bounded catch-up after stalls.
    static constexpr uint32_t kMaxCatchUpFrames = 4;
    const auto now = std::chrono::steady_clock::now();
    if (startTime_.time_since_epoch().count() == 0) startTime_ = now;

    const uint64_t elapsedNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - startTime_).count());
    const uint32_t desiredFrames = static_cast<uint32_t>((elapsedNs * 60ull) / 1000000000ull) + 1u;
    const uint32_t behind = (desiredFrames > frame_) ? (desiredFrames - frame_) : 0u;
    const uint32_t toRun = (behind > kMaxCatchUpFrames) ? kMaxCatchUpFrames : behind;

    for (uint32_t step = 0; step < toRun; ++step) {
        // Always send (except host waiting for first peer packet).
        sendLocal_();

        const uint32_t need = frame_;
        const uint32_t idx = need % kBufN;
        if (remoteFrameTag_[idx] != need || sentFrameTag_[idx] != need) {
            waitingForPeer_ = true;
            break;
        }

        const uint16_t localMask = sentMask_[idx];
        const uint16_t remoteMask = remoteMask_[idx];
        const bool localIsP1 = (localPlayerNum_ == 1);

        EmulatorEngine::instance().setInputMask(0, localIsP1 ? localMask : remoteMask);
        EmulatorEngine::instance().setInputMask(1, localIsP1 ? remoteMask : localMask);
        EmulatorEngine::instance().advanceFrame();
        frame_++;

        waitingForPeer_ = false;
    }
}

std::string LockstepSession::peerEndpoint() const {
    if (!peer_.valid()) return {};

    char ip[INET_ADDRSTRLEN] = {};
#if defined(_WIN32)
    IN_ADDR a{};
    a.s_addr = peer_.ipv4_be;
    if (!InetNtopA(AF_INET, &a, ip, INET_ADDRSTRLEN)) return {};
#else
    in_addr a{};
    a.s_addr = peer_.ipv4_be;
    if (!inet_ntop(AF_INET, &a, ip, INET_ADDRSTRLEN)) return {};
#endif

    const uint16_t port = ntohs(peer_.port_be);
    char out[64] = {};
#if defined(_WIN32)
    std::snprintf(out, sizeof(out), "%s:%u", ip, static_cast<unsigned>(port));
#else
    std::snprintf(out, sizeof(out), "%s:%u", ip, static_cast<unsigned>(port));
#endif
    return std::string(out);
}

} // namespace snesonline

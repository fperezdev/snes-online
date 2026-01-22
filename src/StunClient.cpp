#include "snesonline/StunClient.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace snesonline {

namespace {

static constexpr uint32_t kStunMagicCookie = 0x2112A442u;
static constexpr uint16_t kStunBindingRequest = 0x0001u;
static constexpr uint16_t kStunBindingSuccess = 0x0101u;

static constexpr uint16_t kAttrMappedAddress = 0x0001u;
static constexpr uint16_t kAttrXorMappedAddress = 0x0020u;

#pragma pack(push, 1)
struct StunHeader {
    uint16_t type_be;
    uint16_t length_be;
    uint32_t cookie_be;
    uint8_t txid[12];
};
#pragma pack(pop)

static uint16_t readBE16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) << 8u | static_cast<uint16_t>(p[1]));
}

static uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24u) | (static_cast<uint32_t>(p[1]) << 16u) | (static_cast<uint32_t>(p[2]) << 8u) |
           static_cast<uint32_t>(p[3]);
}

static void writeBE16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
    p[1] = static_cast<uint8_t>(v & 0xFFu);
}

static void writeBE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
    p[3] = static_cast<uint8_t>(v & 0xFFu);
}

static void randomBytes(uint8_t* dst, size_t n) {
    std::random_device rd;
    for (size_t i = 0; i < n; ++i) dst[i] = static_cast<uint8_t>(rd());
}

static bool resolveUdp(const char* host, uint16_t port, sockaddr_storage& out, socklen_t& outLen) {
    if (!host || !host[0]) return false;

    // Numeric fast-path.
    {
        in_addr a4{};
        if (inet_pton(AF_INET, host, &a4) == 1) {
            sockaddr_in sin{};
            sin.sin_family = AF_INET;
            sin.sin_addr = a4;
            sin.sin_port = htons(port);
            std::memset(&out, 0, sizeof(out));
            std::memcpy(&out, &sin, sizeof(sin));
            outLen = static_cast<socklen_t>(sizeof(sin));
            return true;
        }
    }
    {
        in6_addr a6{};
        if (inet_pton(AF_INET6, host, &a6) == 1) {
            sockaddr_in6 sin6{};
            sin6.sin6_family = AF_INET6;
            sin6.sin6_addr = a6;
            sin6.sin6_port = htons(port);
            std::memset(&out, 0, sizeof(out));
            std::memcpy(&out, &sin6, sizeof(sin6));
            outLen = static_cast<socklen_t>(sizeof(sin6));
            return true;
        }
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;

    const addrinfo* best4 = nullptr;
    const addrinfo* best6 = nullptr;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (!ai->ai_addr) continue;
        if (ai->ai_family == AF_INET && !best4) best4 = ai;
        else if (ai->ai_family == AF_INET6 && !best6) best6 = ai;
    }

    const addrinfo* pick = best4 ? best4 : best6;
    if (!pick) {
        freeaddrinfo(res);
        return false;
    }

    std::memset(&out, 0, sizeof(out));
    std::memcpy(&out, pick->ai_addr, static_cast<size_t>(pick->ai_addrlen));
    outLen = static_cast<socklen_t>(pick->ai_addrlen);

    if (pick->ai_family == AF_INET) {
        auto* sin = reinterpret_cast<sockaddr_in*>(&out);
        sin->sin_port = htons(port);
    } else if (pick->ai_family == AF_INET6) {
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&out);
        sin6->sin6_port = htons(port);
    } else {
        freeaddrinfo(res);
        return false;
    }

    freeaddrinfo(res);
    return true;
}

static bool parseMappedAddressAttr(uint16_t attrType, const uint8_t* v, uint16_t len, const uint8_t txid[12], StunMappedAddress& out) {
    // Both MAPPED-ADDRESS and XOR-MAPPED-ADDRESS:
    //  0: reserved (0)
    //  1: family (0x01 = IPv4, 0x02 = IPv6)
    //  2-3: port
    //  4..: address
    if (!v || len < 8) return false;
    const uint8_t family = v[1];

    uint16_t port = readBE16(v + 2);
    if (attrType == kAttrXorMappedAddress) {
        port = static_cast<uint16_t>(port ^ static_cast<uint16_t>((kStunMagicCookie >> 16u) & 0xFFFFu));
    }

    if (family == 0x01) {
        if (len < 8) return false;
        uint32_t addr = readBE32(v + 4);
        if (attrType == kAttrXorMappedAddress) {
            addr = addr ^ kStunMagicCookie;
        }

        in_addr a{};
        a.s_addr = htonl(addr);
        char ipBuf[64] = {};
        if (!inet_ntop(AF_INET, &a, ipBuf, sizeof(ipBuf))) return false;
        out.ip = std::string(ipBuf);
        out.port = port;
        return out.port >= 1 && out.port <= 65535;
    }

    if (family == 0x02) {
        if (len < 20) return false;
        uint8_t addr[16] = {};
        std::memcpy(addr, v + 4, 16);
        if (attrType == kAttrXorMappedAddress) {
            uint8_t key[16] = {};
            writeBE32(key, kStunMagicCookie);
            std::memcpy(key + 4, txid, 12);
            for (int i = 0; i < 16; ++i) addr[i] = static_cast<uint8_t>(addr[i] ^ key[i]);
        }

        in6_addr a6{};
        std::memcpy(&a6, addr, 16);
        char ipBuf[128] = {};
        if (!inet_ntop(AF_INET6, &a6, ipBuf, sizeof(ipBuf))) return false;
        out.ip = std::string(ipBuf);
        out.port = port;
        return out.port >= 1 && out.port <= 65535;
    }

    return false;
}

#if defined(_WIN32)
struct WsaInit {
    bool ok = false;
    WsaInit() {
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    ~WsaInit() {
        if (ok) WSACleanup();
    }
};
static void closesock(int s) { closesocket(static_cast<SOCKET>(s)); }
#else
static void closesock(int s) { close(s); }
#endif

} // namespace

bool stunDiscoverMappedAddress(const char* stunHost, uint16_t stunPort, uint16_t localBindPort, StunMappedAddress& out, int timeoutMs) {
    out = {};

#if defined(_WIN32)
    WsaInit wsa;
    if (!wsa.ok) return false;
#endif

    sockaddr_storage dst{};
    socklen_t dstLen = 0;
    if (!resolveUdp(stunHost, stunPort, dst, dstLen)) return false;

    const int family = reinterpret_cast<const sockaddr*>(&dst)->sa_family;
    const int s = static_cast<int>(::socket(family, SOCK_DGRAM, IPPROTO_UDP));
    if (s < 0) return false;

    if (family == AF_INET6) {
        // Best-effort: allow dual-stack if the platform supports it.
        int v6only = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    }

    if (family == AF_INET) {
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        bindAddr.sin_port = htons(localBindPort);
        if (::bind(s, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
            closesock(s);
            return false;
        }
    } else if (family == AF_INET6) {
        sockaddr_in6 bindAddr6{};
        bindAddr6.sin6_family = AF_INET6;
        bindAddr6.sin6_addr = in6addr_any;
        bindAddr6.sin6_port = htons(localBindPort);
        if (::bind(s, reinterpret_cast<const sockaddr*>(&bindAddr6), sizeof(bindAddr6)) != 0) {
            closesock(s);
            return false;
        }
    } else {
        closesock(s);
        return false;
    }

    // Retry a few times to reduce flakiness on lossy networks.
    // Keep the overall time bounded by splitting timeoutMs across attempts.
    if (timeoutMs < 200) timeoutMs = 200;
    static constexpr int kAttempts = 3;
    const int perAttemptMs = (timeoutMs + (kAttempts - 1)) / kAttempts;

    // Build binding request once (fixed txid) so we only accept matching responses.
    std::array<uint8_t, sizeof(StunHeader)> req{};
    StunHeader hdr{};
    hdr.type_be = htons(kStunBindingRequest);
    hdr.length_be = htons(0);
    hdr.cookie_be = htonl(kStunMagicCookie);
    randomBytes(hdr.txid, sizeof(hdr.txid));
    std::memcpy(req.data(), &hdr, sizeof(hdr));

    std::array<uint8_t, 1500> resp{};

    for (int attempt = 0; attempt < kAttempts; ++attempt) {
#if defined(_WIN32)
        DWORD tv = static_cast<DWORD>(perAttemptMs);
        setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_sec = perAttemptMs / 1000;
        tv.tv_usec = (perAttemptMs % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        const auto sent = ::sendto(s, reinterpret_cast<const char*>(req.data()), static_cast<int>(req.size()), 0,
                       reinterpret_cast<const sockaddr*>(&dst), dstLen);
        if (sent < 0 || static_cast<size_t>(sent) != req.size()) {
            // If sending fails, retries are unlikely to help.
            closesock(s);
            return false;
        }

        sockaddr_storage from{};
#if defined(_WIN32)
        int fromLen = static_cast<int>(sizeof(from));
        const auto n = ::recvfrom(s, reinterpret_cast<char*>(resp.data()), static_cast<int>(resp.size()), 0,
                                  reinterpret_cast<sockaddr*>(&from), &fromLen);
#else
        socklen_t fromLen = sizeof(from);
        const auto n = ::recvfrom(s, resp.data(), resp.size(), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
#endif

        if (n < static_cast<decltype(n)>(sizeof(StunHeader))) {
            continue; // timeout or short packet
        }

        StunHeader rh{};
        std::memcpy(&rh, resp.data(), sizeof(StunHeader));

        const uint16_t type = ntohs(rh.type_be);
        const uint16_t len = ntohs(rh.length_be);
        const uint32_t cookie = ntohl(rh.cookie_be);

        if (type != kStunBindingSuccess) continue;
        if (cookie != kStunMagicCookie) continue;
        if (std::memcmp(rh.txid, hdr.txid, sizeof(hdr.txid)) != 0) continue;

        const auto total = static_cast<decltype(n)>(sizeof(StunHeader)) + static_cast<decltype(n)>(len);
        if (total > n) continue;

        // Parse attributes.
        const uint8_t* p = resp.data() + sizeof(StunHeader);
        size_t remain = len;

        bool got = false;
        StunMappedAddress best{};

        while (remain >= 4) {
            const uint16_t at = readBE16(p);
            const uint16_t alen = readBE16(p + 2);
            p += 4;
            remain -= 4;
            if (alen > remain) break;

            if (at == kAttrXorMappedAddress) {
                StunMappedAddress tmp{};
                if (parseMappedAddressAttr(at, p, alen, rh.txid, tmp)) {
                    best = tmp;
                    got = true;
                    // Prefer XOR-MAPPED; we can stop.
                    break;
                }
            } else if (at == kAttrMappedAddress && !got) {
                StunMappedAddress tmp{};
                if (parseMappedAddressAttr(at, p, alen, rh.txid, tmp)) {
                    best = tmp;
                    got = true;
                }
            }

            const size_t padded = (static_cast<size_t>(alen) + 3u) & ~3u;
            if (padded > remain) break;
            p += padded;
            remain -= padded;
        }

        if (got) {
            out = best;
            closesock(s);
            return true;
        }
    }

    closesock(s);
    return false;
}

bool stunDiscoverMappedAddressDefault(uint16_t localBindPort, StunMappedAddress& out, int timeoutPerServerMs) {
    static constexpr std::array<std::pair<const char*, uint16_t>, 5> kServers = {
        std::pair<const char*, uint16_t>{"stun.cloudflare.com", 3478},
        std::pair<const char*, uint16_t>{"stun.l.google.com", 19302},
        std::pair<const char*, uint16_t>{"stun1.l.google.com", 19302},
        std::pair<const char*, uint16_t>{"stun2.l.google.com", 19302},
        std::pair<const char*, uint16_t>{"global.stun.twilio.com", 3478},
    };

    for (const auto& s : kServers) {
        if (stunDiscoverMappedAddress(s.first, s.second, localBindPort, out, timeoutPerServerMs)) return true;
    }
    return false;
}

} // namespace snesonline

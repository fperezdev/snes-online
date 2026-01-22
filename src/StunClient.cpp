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

static bool resolveIpv4Udp(const char* host, uint16_t port, sockaddr_in& out) {
    if (!host || !host[0]) return false;

    // Numeric fast-path.
    in_addr a{};
    if (inet_pton(AF_INET, host, &a) == 1) {
        out = {};
        out.sin_family = AF_INET;
        out.sin_addr = a;
        out.sin_port = htons(port);
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;

    bool ok = false;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (!ai->ai_addr || ai->ai_family != AF_INET) continue;
        out = *reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
        out.sin_port = htons(port);
        ok = true;
        break;
    }

    freeaddrinfo(res);
    return ok;
}

static bool parseMappedAddressAttr(uint16_t attrType, const uint8_t* v, uint16_t len, const uint8_t txid[12], StunMappedAddress& out) {
    // Both MAPPED-ADDRESS and XOR-MAPPED-ADDRESS:
    //  0: reserved (0)
    //  1: family (0x01 = IPv4, 0x02 = IPv6)
    //  2-3: port
    //  4..: address
    if (!v || len < 8) return false;
    const uint8_t family = v[1];
    if (family != 0x01) return false; // IPv4 only

    uint16_t port = readBE16(v + 2);
    uint32_t addr = readBE32(v + 4);

    if (attrType == kAttrXorMappedAddress) {
        port = static_cast<uint16_t>(port ^ static_cast<uint16_t>((kStunMagicCookie >> 16u) & 0xFFFFu));
        addr = addr ^ kStunMagicCookie;
        (void)txid;
    }

    in_addr a{};
    a.s_addr = htonl(addr);
    char ipBuf[64] = {};
    if (!inet_ntop(AF_INET, &a, ipBuf, sizeof(ipBuf))) return false;

    out.ip = std::string(ipBuf);
    out.port = port;
    return out.port >= 1 && out.port <= 65535;
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

    sockaddr_in dst{};
    if (!resolveIpv4Udp(stunHost, stunPort, dst)) return false;

    const int s = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (s < 0) return false;

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(localBindPort);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
        closesock(s);
        return false;
    }

#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // Build binding request.
    std::array<uint8_t, sizeof(StunHeader)> req{};
    StunHeader hdr{};
    hdr.type_be = htons(kStunBindingRequest);
    hdr.length_be = htons(0);
    hdr.cookie_be = htonl(kStunMagicCookie);
    randomBytes(hdr.txid, sizeof(hdr.txid));
    std::memcpy(req.data(), &hdr, sizeof(hdr));

    const auto sent = ::sendto(s, reinterpret_cast<const char*>(req.data()), static_cast<int>(req.size()), 0,
                               reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    if (sent < 0 || static_cast<size_t>(sent) != req.size()) {
        closesock(s);
        return false;
    }

    std::array<uint8_t, 1500> resp{};
    sockaddr_in from{};
#if defined(_WIN32)
    int fromLen = sizeof(from);
    const auto n = ::recvfrom(s, reinterpret_cast<char*>(resp.data()), static_cast<int>(resp.size()), 0,
                              reinterpret_cast<sockaddr*>(&from), &fromLen);
#else
    socklen_t fromLen = sizeof(from);
    const auto n = ::recvfrom(s, resp.data(), resp.size(), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
#endif

    closesock(s);

    if (n < static_cast<decltype(n)>(sizeof(StunHeader))) return false;

    StunHeader rh{};
    std::memcpy(&rh, resp.data(), sizeof(StunHeader));

    const uint16_t type = ntohs(rh.type_be);
    const uint16_t len = ntohs(rh.length_be);
    const uint32_t cookie = ntohl(rh.cookie_be);

    if (type != kStunBindingSuccess) return false;
    if (cookie != kStunMagicCookie) return false;
    if (std::memcmp(rh.txid, hdr.txid, sizeof(hdr.txid)) != 0) return false;

    const auto total = static_cast<decltype(n)>(sizeof(StunHeader)) + static_cast<decltype(n)>(len);
    if (total > n) return false;

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

    if (!got) return false;
    out = best;
    return true;
}

bool stunDiscoverMappedAddressDefault(uint16_t localBindPort, StunMappedAddress& out, int timeoutPerServerMs) {
    static constexpr std::array<std::pair<const char*, uint16_t>, 3> kServers = {
        std::pair<const char*, uint16_t>{"stun.cloudflare.com", 3478},
        std::pair<const char*, uint16_t>{"stun.l.google.com", 19302},
        std::pair<const char*, uint16_t>{"global.stun.twilio.com", 3478},
    };

    for (const auto& s : kServers) {
        if (stunDiscoverMappedAddress(s.first, s.second, localBindPort, out, timeoutPerServerMs)) return true;
    }
    return false;
}

} // namespace snesonline

#import "SNESOnlineIOSNative.h"

#import <Foundation/Foundation.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>

#include "snesonline/EmulatorEngine.h"
#include "snesonline/InputBits.h"
#include "snesonline/StunClient.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

static inline uint32_t read_u32_be_(const uint8_t* p) noexcept {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return ntohl(v);
}

static inline uint16_t read_u16_be_(const uint8_t* p) noexcept {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return ntohs(v);
}

static inline void write_u32_be_(uint8_t* p, uint32_t v) noexcept {
    const uint32_t be = htonl(v);
    std::memcpy(p, &be, sizeof(be));
}

static inline void write_u16_be_(uint8_t* p, uint16_t v) noexcept {
    const uint16_t be = htons(v);
    std::memcpy(p, &be, sizeof(be));
}

static uint32_t crc32_(const void* data, std::size_t sizeBytes) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < sizeBytes; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) {
            const uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static bool readFile_(const char* path, std::vector<uint8_t>& out) noexcept {
    out.clear();
    if (!path || !path[0]) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    const long sz = std::ftell(f);
    if (sz <= 0) {
        std::fclose(f);
        return false;
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }

    try {
        out.resize(static_cast<std::size_t>(sz));
    } catch (...) {
        std::fclose(f);
        out.clear();
        return false;
    }

    const std::size_t n = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (n != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

static bool writeFile_(const char* path, const void* data, std::size_t sizeBytes) noexcept {
    if (!path || !path[0] || !data || sizeBytes == 0) return false;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const std::size_t n = std::fwrite(data, 1, sizeBytes, f);
    std::fclose(f);
    return n == sizeBytes;
}

static bool loadStateBytes_(const uint8_t* data, std::size_t sizeBytes) noexcept {
    if (!data || sizeBytes == 0) return false;
    snesonline::SaveState st;
    if (!st.buffer.allocate(sizeBytes)) return false;
    std::memcpy(st.buffer.data(), data, sizeBytes);
    st.sizeBytes = sizeBytes;
    st.checksum = crc32_(data, sizeBytes);
    return snesonline::EmulatorEngine::instance().loadState(st);
}

static bool ensureDir_(const std::string& dir) noexcept {
    if (dir.empty()) return false;
    if (::mkdir(dir.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

static std::string dirname_(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

static std::string basenameNoExt_(const std::string& path) {
    const auto slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    if (name.empty()) name = "game";
    return name;
}

static std::string makeDefaultSaveRamPathFromRom_(const char* romPath) {
    if (!romPath || !romPath[0]) return {};
    @autoreleasepool {
        NSArray* dirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString* docs = (dirs.count > 0) ? dirs[0] : NSTemporaryDirectory();
        NSString* savesDir = [docs stringByAppendingPathComponent:@"saves"];
        (void)ensureDir_(std::string([savesDir UTF8String]));

        std::string rom(romPath);
        const std::string base = basenameNoExt_(rom);
        NSString* file = [NSString stringWithFormat:@"%s.srm", base.c_str()];
        NSString* full = [savesDir stringByAppendingPathComponent:file];
        return std::string([full UTF8String]);
    }
}

static constexpr unsigned kRetroMemorySaveRam_ = 0; // RETRO_MEMORY_SAVE_RAM

static std::string g_saveRamPath;
static uint32_t g_saveRamLastCrc = 0;
static std::chrono::steady_clock::time_point g_saveRamLastCheck{};
static std::chrono::steady_clock::time_point g_saveRamLastFlush{};

static bool applySaveRamBytes_(const uint8_t* data, std::size_t sizeBytes) noexcept {
    auto& core = snesonline::EmulatorEngine::instance().core();
    void* mem = core.memoryData(kRetroMemorySaveRam_);
    const std::size_t memSize = core.memorySize(kRetroMemorySaveRam_);
    if (!mem || memSize == 0) return false;
    const std::size_t copyN = (sizeBytes < memSize) ? sizeBytes : memSize;
    if (copyN > 0) std::memcpy(mem, data, copyN);
    if (copyN < memSize) std::memset(static_cast<uint8_t*>(mem) + copyN, 0, memSize - copyN);
    return true;
}

static bool loadSaveRamFromFile_(const std::string& path) noexcept {
    if (path.empty()) return false;
    std::vector<uint8_t> bytes;
    if (!readFile_(path.c_str(), bytes)) return false;
    return applySaveRamBytes_(bytes.data(), bytes.size());
}

static bool getSaveRamBytes_(std::vector<uint8_t>& out) noexcept {
    out.clear();
    auto& core = snesonline::EmulatorEngine::instance().core();
    void* mem = core.memoryData(kRetroMemorySaveRam_);
    const std::size_t memSize = core.memorySize(kRetroMemorySaveRam_);
    if (!mem || memSize == 0) return false;
    try {
        out.resize(memSize);
    } catch (...) {
        out.clear();
        return false;
    }
    std::memcpy(out.data(), mem, memSize);
    return true;
}

static void maybeFlushSaveRam_(bool force) noexcept {
    if (g_saveRamPath.empty()) return;
    auto& core = snesonline::EmulatorEngine::instance().core();
    void* mem = core.memoryData(kRetroMemorySaveRam_);
    const std::size_t memSize = core.memorySize(kRetroMemorySaveRam_);
    if (!mem || memSize == 0) return;

    const auto now = std::chrono::steady_clock::now();
    if (!force) {
        if (g_saveRamLastCheck.time_since_epoch().count() != 0 && (now - g_saveRamLastCheck) < std::chrono::milliseconds(500)) return;
        g_saveRamLastCheck = now;
    }

    const uint32_t crc = crc32_(mem, memSize);
    if (!force && crc == g_saveRamLastCrc) return;
    if (!force && g_saveRamLastFlush.time_since_epoch().count() != 0 && (now - g_saveRamLastFlush) < std::chrono::milliseconds(1000)) return;

    if (writeFile_(g_saveRamPath.c_str(), mem, memSize)) {
        g_saveRamLastCrc = crc;
        g_saveRamLastFlush = now;
    }
}

std::atomic<uint16_t> g_inputMask{0};
std::atomic<bool> g_running{false};
std::atomic<bool> g_paused{false};
std::thread g_loopThread;

struct UdpNetplay {
    int sock = -1;
    sockaddr_in6 remote{};
    uint16_t localPort = 7000;
    uint16_t remotePort = 7000;
    uint8_t localPlayerNum = 1;
    uint32_t frame = 0;

    bool discoverPeer = false;
    bool requireSecret = false;
    uint16_t secret16 = 0;
    uint32_t secret32 = 0;

    static constexpr uint32_t kBufN = 256;
    static constexpr uint32_t kInputDelayFrames = 5;
    static constexpr uint32_t kResendWindow = 16;

    static constexpr uint32_t kHashIntervalFrames = 60;

    std::array<uint16_t, kBufN> remoteMask{};
    std::array<uint32_t, kBufN> remoteFrameTag{};

    std::array<uint16_t, kBufN> sentMask{};
    std::array<uint32_t, kBufN> sentFrameTag{};

    std::array<uint32_t, kBufN> localHash{};
    std::array<uint32_t, kBufN> localHashTag{};
    std::array<uint32_t, kBufN> remoteHash{};
    std::array<uint32_t, kBufN> remoteHashTag{};

    bool pendingResyncHost = false;
    std::chrono::steady_clock::time_point lastResyncRequestSent{};
    std::chrono::steady_clock::time_point lastResyncTriggered{};

    bool hasPeer = false;
    std::chrono::steady_clock::time_point lastRecv{};
    std::chrono::steady_clock::time_point lastKeepAliveSent{};

    // While hosting and waiting for the first inbound packet, we still need outbound UDP traffic
    // to keep the NAT mapping alive so the joiner can reach us.
    sockaddr_in6 natPunchTarget{};
    bool natPunchReady = false;
    std::chrono::steady_clock::time_point lastNatPunchSent{};

    struct Packet {
        uint32_t magic_be;
        uint32_t frame_be;
        uint16_t mask_be;
        uint16_t reserved_be;
    };

    static constexpr uint32_t kMagicInput = 0x534E4F49u;     // 'SNOI'
    static constexpr uint32_t kMagicHash = 0x534E4F48u;      // 'SNOH'
    static constexpr uint32_t kMagicKeepAlive = 0x534E4F4Bu; // 'SNOK'
    static constexpr uint32_t kMagicResyncReq = 0x534E4F51u; // 'SNOQ'
    static constexpr uint32_t kMagicStateInfo = 0x534E4F53u; // 'SNOS'
    static constexpr uint32_t kMagicStateChunk = 0x534E4F43u;// 'SNOC'
    static constexpr uint32_t kMagicStateAck = 0x534E4F41u;  // 'SNOA'

    static constexpr uint32_t kMagicSaveRamInfo = 0x534E4F52u; // 'SNOR'
    static constexpr uint32_t kMagicSaveRamChunk = 0x534E4F44u;// 'SNOD'
    static constexpr uint32_t kMagicSaveRamAck = 0x534E4F45u;  // 'SNOE'

    bool wantStateSync = false;
    bool isHost = false;
    bool peerStateReady = false;
    bool selfStateReady = true;

    bool joinAwaitingStateOffer = false;
    std::chrono::steady_clock::time_point joinWaitStateOfferDeadline{};

    bool joinAwaitingSaveRamOffer = false;
    std::chrono::steady_clock::time_point joinWaitSaveRamOfferDeadline{};

    std::vector<uint8_t> stateTx;
    uint32_t stateSize = 0;
    uint32_t stateCrc = 0;
    uint16_t stateChunkSize = 1024;
    uint16_t stateChunkCount = 0;
    std::chrono::steady_clock::time_point lastInfoSent{};
    std::chrono::steady_clock::time_point stateSyncDeadline{};
    uint16_t nextChunkToSend = 0;

    std::vector<uint8_t> stateRx;
    std::vector<uint8_t> stateRxHave;
    uint16_t stateRxHaveCount = 0;

    bool wantSaveRamSync = false;
    bool saveRamGate = false;
    bool peerSaveRamReady = true;
    bool selfSaveRamReady = true;

    std::vector<uint8_t> saveRamTx;
    uint32_t saveRamSize = 0;
    uint32_t saveRamCrc = 0;
    uint16_t saveRamChunkSize = 1024;
    uint16_t saveRamChunkCount = 0;
    std::chrono::steady_clock::time_point lastSaveRamInfoSent{};
    uint16_t nextSaveRamChunkToSend = 0;

    std::vector<uint8_t> saveRamRx;
    std::vector<uint8_t> saveRamRxHave;
    uint16_t saveRamRxHaveCount = 0;

    void configureStateSyncHost(std::vector<uint8_t>&& bytes) noexcept {
        stateTx = std::move(bytes);
        stateSize = static_cast<uint32_t>(stateTx.size());
        stateCrc = stateSize ? crc32_(stateTx.data(), stateTx.size()) : 0;
        stateChunkSize = 1024;
        stateChunkCount = static_cast<uint16_t>((stateSize + stateChunkSize - 1u) / stateChunkSize);
        wantStateSync = (stateSize > 0);
        isHost = true;
        peerStateReady = !wantStateSync;
        selfStateReady = true;
        nextChunkToSend = 0;
        lastInfoSent = {};
        stateSyncDeadline = wantStateSync ? (std::chrono::steady_clock::now() + std::chrono::seconds(10)) : std::chrono::steady_clock::time_point{};
    }

    void configureStateSyncJoiner() noexcept {
        isHost = false;
        wantStateSync = false;
        peerStateReady = true;
        selfStateReady = true;
        joinAwaitingStateOffer = true;
        joinWaitStateOfferDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        stateSyncDeadline = {};
        stateRx.clear();
        stateRxHave.clear();
        stateRxHaveCount = 0;
        stateSize = 0;
        stateCrc = 0;
        stateChunkSize = 1024;
        stateChunkCount = 0;
    }

    bool queueSaveRamSync(std::vector<uint8_t>&& bytes, bool gateUntilAck) noexcept {
        if (bytes.empty()) return false;
        const uint32_t crc = crc32_(bytes.data(), bytes.size());
        if (wantSaveRamSync && crc == saveRamCrc && static_cast<uint32_t>(bytes.size()) == saveRamSize) {
            return true;
        }

        saveRamTx = std::move(bytes);
        saveRamSize = static_cast<uint32_t>(saveRamTx.size());
        saveRamCrc = saveRamSize ? crc32_(saveRamTx.data(), saveRamTx.size()) : 0;
        saveRamChunkSize = 1024;
        saveRamChunkCount = static_cast<uint16_t>((saveRamSize + saveRamChunkSize - 1u) / saveRamChunkSize);

        wantSaveRamSync = (saveRamSize > 0);
        saveRamGate = gateUntilAck;
        peerSaveRamReady = !saveRamGate;
        nextSaveRamChunkToSend = 0;
        lastSaveRamInfoSent = {};
        return wantSaveRamSync;
    }

    void configureSaveRamSyncJoiner() noexcept {
        joinAwaitingSaveRamOffer = false;
        joinWaitSaveRamOfferDeadline = {};
        wantSaveRamSync = false;
        saveRamGate = false;
        peerSaveRamReady = true;
        selfSaveRamReady = true;
        saveRamRx.clear();
        saveRamRxHave.clear();
        saveRamRxHaveCount = 0;
        saveRamSize = 0;
        saveRamCrc = 0;
        saveRamChunkSize = 1024;
        saveRamChunkCount = 0;
    }

    static bool setNonBlocking(int fd) noexcept {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) return false;
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    static bool resolveIpAnyToIn6_(const char* host, sockaddr_in6& out, uint16_t port) noexcept {
        if (!host || !host[0]) return false;

        std::string h(host);
        if (h.size() >= 2 && h.front() == '[' && h.back() == ']') {
            h = h.substr(1, h.size() - 2);
        }

        // Numeric IPv6.
        in6_addr addr6{};
        if (inet_pton(AF_INET6, h.c_str(), &addr6) == 1) {
            out = {};
            out.sin6_family = AF_INET6;
            out.sin6_addr = addr6;
            out.sin6_port = htons(port);
            return true;
        }

        // Numeric IPv4 -> v4-mapped IPv6.
        in_addr addr4{};
        if (inet_pton(AF_INET, h.c_str(), &addr4) == 1) {
            out = {};
            out.sin6_family = AF_INET6;
            out.sin6_port = htons(port);
            out.sin6_addr = {};
            out.sin6_addr.s6_addr[10] = 0xff;
            out.sin6_addr.s6_addr[11] = 0xff;
            std::memcpy(&out.sin6_addr.s6_addr[12], &addr4, 4);
            return true;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(h.c_str(), nullptr, &hints, &res) != 0 || !res) return false;

        bool ok = false;
        // Prefer IPv6 if available.
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            if (!ai->ai_addr) continue;
            if (ai->ai_family == AF_INET6) {
                const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(ai->ai_addr);
                out = *sin6;
                out.sin6_port = htons(port);
                ok = true;
                break;
            }
        }
        if (!ok) {
            for (addrinfo* ai = res; ai; ai = ai->ai_next) {
                if (!ai->ai_addr) continue;
                if (ai->ai_family == AF_INET) {
                    const auto* sin = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
                    out = {};
                    out.sin6_family = AF_INET6;
                    out.sin6_port = htons(port);
                    out.sin6_addr = {};
                    out.sin6_addr.s6_addr[10] = 0xff;
                    out.sin6_addr.s6_addr[11] = 0xff;
                    std::memcpy(&out.sin6_addr.s6_addr[12], &sin->sin_addr, 4);
                    ok = true;
                    break;
                }
            }
        }

        freeaddrinfo(res);
        return ok;
    }

    static bool parseHostPort_(const char* s, std::string& outHost, uint16_t& outPort) noexcept {
        outHost.clear();
        outPort = 0;
        if (!s || !s[0]) return false;
        std::string t(s);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '\n')) t.pop_back();
        size_t b = 0;
        while (b < t.size() && (t[b] == ' ' || t[b] == '\t' || t[b] == '\r' || t[b] == '\n')) ++b;
        t = t.substr(b);
        if (t.empty()) return false;

        std::string host;
        uint16_t port = 0;
        if (!t.empty() && t.front() == '[') {
            const auto rb = t.find(']');
            if (rb == std::string::npos) return false;
            host = t.substr(1, rb - 1);
            if (rb + 1 >= t.size() || t[rb + 1] != ':') return false;
            try {
                const int p = std::stoi(t.substr(rb + 2));
                if (p >= 1 && p <= 65535) port = static_cast<uint16_t>(p);
            } catch (...) {
                port = 0;
            }
        } else {
            const auto colon = t.rfind(':');
            if (colon == std::string::npos || colon + 1 >= t.size()) return false;
            host = t.substr(0, colon);
            try {
                const int p = std::stoi(t.substr(colon + 1));
                if (p >= 1 && p <= 65535) port = static_cast<uint16_t>(p);
            } catch (...) {
                port = 0;
            }
        }

        if (host.empty() || port == 0) return false;
        outHost = host;
        outPort = port;
        return true;
    }

    static bool parseRoomServerUrl(const char* url, std::string& outHost, uint16_t& outPort) noexcept {
        outHost.clear();
        outPort = 0;
        if (!url || !url[0]) return false;
        std::string s(url);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
        size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
        s = s.substr(b);

        const auto pos = s.find("://");
        if (pos != std::string::npos) s = s.substr(pos + 3);
        if (s.empty()) return false;

        const auto slash = s.find('/');
        if (slash != std::string::npos) s = s.substr(0, slash);

        std::string host = s;
        uint16_t port = 0;
        if (!s.empty() && s.front() == '[') {
            const auto rb = s.find(']');
            if (rb == std::string::npos) return false;
            host = s.substr(1, rb - 1);
            if (rb + 1 < s.size() && s[rb + 1] == ':' && rb + 2 < s.size()) {
                try {
                    const int p = std::stoi(s.substr(rb + 2));
                    if (p >= 1 && p <= 65535) port = static_cast<uint16_t>(p);
                } catch (...) {
                    port = 0;
                }
            }
        } else {
            const auto colon = s.rfind(':');
            if (colon != std::string::npos && colon + 1 < s.size()) {
                host = s.substr(0, colon);
                try {
                    const int p = std::stoi(s.substr(colon + 1));
                    if (p >= 1 && p <= 65535) port = static_cast<uint16_t>(p);
                } catch (...) {
                    port = 0;
                }
            }
        }
        if (host.empty()) return false;
        if (port == 0) port = 8787;
        outHost = host;
        outPort = port;
        return true;
    }

    bool serverAssistPunch(const char* roomServerUrl, const char* roomCodeRaw) noexcept {
        if (sock < 0) return false;
        if (!roomServerUrl || !roomServerUrl[0]) return false;
        if (!roomCodeRaw || !roomCodeRaw[0]) return false;

        std::string host;
        uint16_t port = 0;
        if (!parseRoomServerUrl(roomServerUrl, host, port)) return false;

        sockaddr_in6 server{};
        if (!resolveIpAnyToIn6_(host.c_str(), server, port)) return false;

        std::string code;
        code.reserve(16);
        for (const char* p = roomCodeRaw; *p; ++p) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            if (ok) code.push_back(c);
        }
        if (code.size() < 8 || code.size() > 12) return false;

        char msg[64] = {};
        std::snprintf(msg, sizeof(msg), "SNO_PUNCH1 %s\n", code.c_str());

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        auto nextSend = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() < deadline) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextSend) {
                (void)sendto(sock, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr*>(&server), sizeof(server));
                nextSend = now + std::chrono::milliseconds(250);
            }

            for (int i = 0; i < 8; ++i) {
                char buf[256] = {};
                sockaddr_in6 from{};
                socklen_t fromLen = sizeof(from);
                const int n = static_cast<int>(recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
                if (n <= 0) break;
                if (from.sin6_port != server.sin6_port || from.sin6_scope_id != server.sin6_scope_id || std::memcmp(&from.sin6_addr, &server.sin6_addr, sizeof(in6_addr)) != 0) continue;
                if (n < 10) continue;
                if (std::memcmp(buf, "SNO_PEER1", 9) != 0) continue;

                buf[(n >= static_cast<int>(sizeof(buf))) ? (static_cast<int>(sizeof(buf)) - 1) : n] = 0;
                const char* payload = buf + 9;
                while (*payload == ' ' || *payload == '\t') ++payload;
                std::string peerHost;
                uint16_t peerPort = 0;
                if (!parseHostPort_(payload, peerHost, peerPort)) continue;
                sockaddr_in6 dst{};
                if (!resolveIpAnyToIn6_(peerHost.c_str(), dst, peerPort)) continue;
                remote = dst;
                remotePort = peerPort;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool start(const char* remoteHost, uint16_t rPort, uint16_t lPort, uint8_t lPlayer, const char* roomServerUrl, const char* roomCode, const char* sharedSecret) noexcept {
        stop();

        localPort = (lPort != 0) ? lPort : 7000;
        remotePort = (rPort != 0) ? rPort : 7000;
        localPlayerNum = (lPlayer == 2) ? 2 : 1;

        // This is a gameplay role (P1 = host, P2 = joiner), independent of whether state sync is active.
        isHost = (localPlayerNum == 1);

        // Reset sync gating state; later we may enable state/SRAM sync explicitly.
        wantStateSync = false;
        peerStateReady = true;
        selfStateReady = true;
        joinAwaitingStateOffer = false;
        joinWaitStateOfferDeadline = {};
        stateSyncDeadline = {};

        wantSaveRamSync = false;
        saveRamGate = false;
        peerSaveRamReady = true;
        selfSaveRamReady = true;
        joinAwaitingSaveRamOffer = false;
        joinWaitSaveRamOfferDeadline = {};

        remoteFrameTag.fill(0xFFFFFFFFu);
        remoteMask.fill(0);
        sentFrameTag.fill(0xFFFFFFFFu);
        sentMask.fill(0);
        localHashTag.fill(0xFFFFFFFFu);
        remoteHashTag.fill(0xFFFFFFFFu);
        frame = 0;

        for (uint32_t f = 0; f < kInputDelayFrames; ++f) {
            const uint32_t idx = f % kBufN;
            sentFrameTag[idx] = f;
            sentMask[idx] = 0;
        }

        pendingResyncHost = false;
        lastResyncRequestSent = {};
        lastResyncTriggered = {};

        hasPeer = false;
        lastRecv = {};

        natPunchTarget = {};
        natPunchReady = false;
        lastNatPunchSent = {};

        discoverPeer = false;

        requireSecret = (sharedSecret && sharedSecret[0]);
        secret32 = requireSecret ? crc32_(sharedSecret, std::strlen(sharedSecret)) : 0u;
        uint32_t mix = secret32 ^ (secret32 >> 16);
        secret16 = static_cast<uint16_t>(mix & 0xFFFFu);
        if (requireSecret && secret16 == 0) secret16 = 1;

        sock = ::socket(AF_INET6, SOCK_DGRAM, 0);
        if (sock < 0) return false;

        int v6only = 0;
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

        int buf = 1 << 20;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));

        sockaddr_in6 local{};
        local.sin6_family = AF_INET6;
        local.sin6_addr = in6addr_any;
        local.sin6_port = htons(localPort);
        if (bind(sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            stop();
            return false;
        }

        setNonBlocking(sock);

        if (remoteHost && remoteHost[0]) {
            if (!resolveIpAnyToIn6_(remoteHost, remote, remotePort)) {
                stop();
                return false;
            }
        } else {
            discoverPeer = (localPlayerNum == 1);
            remote = {};
            remote.sin6_family = AF_INET6;
            remote.sin6_port = 0;
            remote.sin6_addr = in6addr_loopback;

            // Host: pick a best-effort public UDP target to keep the NAT mapping alive.
            // This does not need to respond; any outbound traffic from this bound socket is enough.
            if (discoverPeer) {
                static constexpr const char* kNatPunchHost = "stun.l.google.com";
                static constexpr uint16_t kNatPunchPort = 19302;
                sockaddr_in6 tmp{};
                if (resolveIpAnyToIn6_(kNatPunchHost, tmp, kNatPunchPort)) {
                    natPunchTarget = tmp;
                    natPunchReady = true;
                }
            }
        }

        // Joiner: if no direct host endpoint is known, use the room-server rendezvous.
        if ((remoteHost == nullptr || !remoteHost[0]) && roomServerUrl && roomServerUrl[0] && roomCode && roomCode[0]) {
            (void)serverAssistPunch(roomServerUrl, roomCode);
        }

        // Joiner must wait for a host state offer.
        if (localPlayerNum == 2) {
            configureStateSyncJoiner();
            configureSaveRamSyncJoiner();
        }
        return true;
    }

    void stop() noexcept {
        if (sock >= 0) {
            close(sock);
            sock = -1;
        }
        hasPeer = false;
        wantStateSync = false;
        wantSaveRamSync = false;
    }

    bool readyToRun() noexcept {
        if (!hasPeer) return false;
        const auto now = std::chrono::steady_clock::now();

        // Joiner: allow starting even if host never offers a state.
        if (!isHost && joinAwaitingStateOffer) {
            if (now < joinWaitStateOfferDeadline) {
                return false;
            }
            joinAwaitingStateOffer = false;
        }

        // State sync gating (with timeout fallback).
        if (isHost && wantStateSync) {
            if (!peerStateReady && stateSyncDeadline.time_since_epoch().count() != 0 && now > stateSyncDeadline) {
                // Peer never acked (likely couldn't load the state). Fall back to starting from reset.
                wantStateSync = false;
                peerStateReady = true;
                selfStateReady = true;
                stateTx.clear();
                stateSize = 0;
                stateCrc = 0;
                stateChunkCount = 0;
                stateSyncDeadline = {};
            }
            return peerStateReady;
        }
        if (!isHost && wantStateSync) {
            if (!selfStateReady && stateSyncDeadline.time_since_epoch().count() != 0 && now > stateSyncDeadline) {
                // We received a state offer but couldn't successfully load it. Fall back to starting from reset.
                wantStateSync = false;
                selfStateReady = true;
                peerStateReady = true;
                stateRx.clear();
                stateRxHave.clear();
                stateRxHaveCount = 0;
                stateSize = 0;
                stateCrc = 0;
                stateChunkCount = 0;
                stateSyncDeadline = {};
            }
            return selfStateReady;
        }

        // SaveRAM gating only applies when explicitly requested.
        if (isHost && saveRamGate) return peerSaveRamReady;
        if (!isHost && saveRamGate) return selfSaveRamReady;
        return true;
    }

    void markPeerConnected_(const sockaddr_in6& from) noexcept {
        if (!hasPeer) {
            hasPeer = true;
            lastRecv = std::chrono::steady_clock::now();
            if (discoverPeer) {
                remote = from;
                remotePort = ntohs(from.sin6_port);
            }
        }

        // Keep peer IP pinned but allow port to float (NATs may rewrite source ports).
        if (hasPeer && remote.sin6_family == AF_INET6 && from.sin6_family == AF_INET6) {
            if (remote.sin6_scope_id == from.sin6_scope_id && std::memcmp(&remote.sin6_addr, &from.sin6_addr, sizeof(in6_addr)) == 0) {
                remote.sin6_port = from.sin6_port;
                remotePort = ntohs(from.sin6_port);
            }
        }
        lastRecv = std::chrono::steady_clock::now();
    }

    void requestResyncJoiner_() noexcept {
        if (sock < 0 || !hasPeer) return;
        const auto now = std::chrono::steady_clock::now();
        if (lastResyncRequestSent.time_since_epoch().count() != 0 && (now - lastResyncRequestSent) < std::chrono::milliseconds(800)) return;
        lastResyncRequestSent = now;
        uint8_t req[8] = {};
        write_u32_be_(req, kMagicResyncReq);
        write_u32_be_(req + 4, frame);
        sendto(sock, req, sizeof(req), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
    }

    void performResyncHostIfPending_() noexcept {
        if (!pendingResyncHost) return;
        const auto now = std::chrono::steady_clock::now();
        if (lastResyncTriggered.time_since_epoch().count() != 0 && (now - lastResyncTriggered) < std::chrono::seconds(2)) {
            pendingResyncHost = false;
            return;
        }
        lastResyncTriggered = now;
        pendingResyncHost = false;

        snesonline::SaveState st;
        if (snesonline::EmulatorEngine::instance().saveState(st) && st.sizeBytes > 0 && st.buffer.data()) {
            std::vector<uint8_t> bytes(st.sizeBytes);
            std::memcpy(bytes.data(), st.buffer.data(), st.sizeBytes);
            configureStateSyncHost(std::move(bytes));
            peerStateReady = false;
        }
    }

    void recordLocalHashForCompletedFrame_(uint32_t completedFrame) noexcept {
        auto& core = snesonline::EmulatorEngine::instance().core();
        void* mem = core.memoryData(2 /* RETRO_MEMORY_SYSTEM_RAM */);
        const std::size_t memSize = core.memorySize(2);
        uint32_t h = 0;
        if (mem && memSize > 0) {
            h = crc32_(mem, memSize);
        }
        const uint32_t idx = completedFrame % kBufN;
        localHashTag[idx] = completedFrame;
        localHash[idx] = h;
    }

    void maybeSendLocalHashForCompletedFrame_(uint32_t completedFrame) noexcept {
        if (sock < 0 || !hasPeer) return;
        if ((completedFrame % kHashIntervalFrames) != 0) return;
        const uint32_t idx = completedFrame % kBufN;
        if (localHashTag[idx] != completedFrame) return;
        uint8_t pkt[12] = {};
        write_u32_be_(pkt, kMagicHash);
        write_u32_be_(pkt + 4, completedFrame);
        write_u32_be_(pkt + 8, localHash[idx]);
        sendto(sock, pkt, sizeof(pkt), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
    }

    void pumpStateSyncSend() noexcept {
        if (sock < 0 || !hasPeer) return;
        if (!wantStateSync || stateTx.empty() || stateChunkCount == 0) return;
        if (peerStateReady) return;

        // Android wire format (for cross-platform):
        // - StateInfo: 16 bytes: magic, size, crc, chunkSize, chunkCount
        // - StateChunk: 12 + payload: magic, idx, chunkCount, payloadSize, reserved, payload
        const auto now = std::chrono::steady_clock::now();
        if (lastInfoSent.time_since_epoch().count() == 0 || (now - lastInfoSent) > std::chrono::milliseconds(500)) {
            uint8_t info[16] = {};
            write_u32_be_(info, kMagicStateInfo);
            write_u32_be_(info + 4, stateSize);
            write_u32_be_(info + 8, stateCrc);
            write_u16_be_(info + 12, stateChunkSize);
            write_u16_be_(info + 14, stateChunkCount);
            sendto(sock, info, sizeof(info), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
            lastInfoSent = now;
        }

        for (int i = 0; i < 8; ++i) {
            const uint16_t chunk = static_cast<uint16_t>(nextChunkToSend % stateChunkCount);
            const uint32_t off = static_cast<uint32_t>(chunk) * static_cast<uint32_t>(stateChunkSize);
            if (off >= stateSize) break;
            const uint32_t payloadN = std::min<uint32_t>(stateChunkSize, stateSize - off);
            const uint16_t payloadSz = static_cast<uint16_t>(payloadN);

            std::vector<uint8_t> pkt;
            pkt.resize(12u + payloadN);
            write_u32_be_(pkt.data(), kMagicStateChunk);
            write_u16_be_(pkt.data() + 4, chunk);
            write_u16_be_(pkt.data() + 6, stateChunkCount);
            write_u16_be_(pkt.data() + 8, payloadSz);
            write_u16_be_(pkt.data() + 10, 0);
            std::memcpy(pkt.data() + 12, stateTx.data() + off, payloadN);
            sendto(sock, pkt.data(), pkt.size(), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
            nextChunkToSend = static_cast<uint16_t>(nextChunkToSend + 1);
        }
    }

    void pumpSaveRamSyncSend() noexcept {
        if (sock < 0 || !hasPeer) return;
        if (!wantSaveRamSync || saveRamTx.empty() || saveRamChunkCount == 0) return;
        if (peerSaveRamReady) return;

        // Android wire format (for cross-platform):
        // - SaveRamInfo: 16 bytes (+ optional 2 byte flags): magic, size, crc, chunkSize, chunkCount, [flags]
        // - SaveRamChunk: 12 + payload: magic, idx, chunkCount, payloadSize, reserved, payload
        const auto now = std::chrono::steady_clock::now();
        if (lastSaveRamInfoSent.time_since_epoch().count() == 0 || (now - lastSaveRamInfoSent) > std::chrono::milliseconds(500)) {
            uint8_t info[18] = {};
            write_u32_be_(info, kMagicSaveRamInfo);
            write_u32_be_(info + 4, saveRamSize);
            write_u32_be_(info + 8, saveRamCrc);
            write_u16_be_(info + 12, saveRamChunkSize);
            write_u16_be_(info + 14, saveRamChunkCount);
            const uint16_t flags = saveRamGate ? 1u : 0u;
            write_u16_be_(info + 16, flags);
            sendto(sock, info, sizeof(info), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
            lastSaveRamInfoSent = now;
        }

        for (int i = 0; i < 8; ++i) {
            const uint16_t chunk = static_cast<uint16_t>(nextSaveRamChunkToSend % saveRamChunkCount);
            const uint32_t off = static_cast<uint32_t>(chunk) * static_cast<uint32_t>(saveRamChunkSize);
            if (off >= saveRamSize) break;
            const uint32_t payloadN = std::min<uint32_t>(saveRamChunkSize, saveRamSize - off);
            const uint16_t payloadSz = static_cast<uint16_t>(payloadN);

            std::vector<uint8_t> pkt;
            pkt.resize(12u + payloadN);
            write_u32_be_(pkt.data(), kMagicSaveRamChunk);
            write_u16_be_(pkt.data() + 4, chunk);
            write_u16_be_(pkt.data() + 6, saveRamChunkCount);
            write_u16_be_(pkt.data() + 8, payloadSz);
            write_u16_be_(pkt.data() + 10, 0);
            std::memcpy(pkt.data() + 12, saveRamTx.data() + off, payloadN);
            sendto(sock, pkt.data(), pkt.size(), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
            nextSaveRamChunkToSend = static_cast<uint16_t>(nextSaveRamChunkToSend + 1);
        }
    }

    void pumpRecv() noexcept {
        if (sock < 0) return;

        for (;;) {
            uint8_t buf[1500] = {};
            sockaddr_in6 from{};
            socklen_t fromLen = sizeof(from);
            const ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n < 0) break;
            if (n < 8) continue;

            const uint32_t magic = read_u32_be_(buf);
            // If we already have a peer, ignore packets from other IPs.
            if (hasPeer && remote.sin6_family == AF_INET6 && from.sin6_family == AF_INET6) {
                if (remote.sin6_scope_id != from.sin6_scope_id || std::memcmp(&remote.sin6_addr, &from.sin6_addr, sizeof(in6_addr)) != 0) {
                    continue;
                }
            }

            // Host auto-discovery: only accept the first peer if it presents the correct secret token.
            if (discoverPeer && !hasPeer) {
                if (magic != kMagicInput) continue;
                if (static_cast<size_t>(n) < sizeof(Packet)) continue;
                if (requireSecret) {
                    const uint16_t tok = read_u16_be_(buf + 10);
                    if (tok != secret16) continue;
                }
            }

            // Validate tokens on keepalives and input packets.
            if (magic == kMagicKeepAlive && requireSecret) {
                const uint32_t tok = read_u32_be_(buf + 4);
                if (tok != secret32) continue;
            }
            if (magic == kMagicInput && requireSecret) {
                if (static_cast<size_t>(n) < sizeof(Packet)) continue;
                const uint16_t tok = read_u16_be_(buf + 10);
                if (tok != secret16) continue;
            }

            // For all other packet types, require a discovered peer first.
            if (!hasPeer && magic != kMagicInput && magic != kMagicKeepAlive) {
                continue;
            }

            markPeerConnected_(from);

            if (magic == kMagicKeepAlive) {
                // Keepalive: refresh peer/lastRecv only.
                continue;
            }

            if (magic == kMagicInput) {
                if (static_cast<size_t>(n) < sizeof(Packet)) continue;
                Packet p{};
                std::memcpy(&p, buf, sizeof(Packet));
                const uint32_t f = ntohl(p.frame_be);
                const uint16_t m = ntohs(p.mask_be);
                const uint32_t idx = f % kBufN;
                remoteFrameTag[idx] = f;
                remoteMask[idx] = m;
                continue;
            }

            if (magic == kMagicHash) {
                if (n < 12) continue;
                const uint32_t f = read_u32_be_(buf + 4);
                const uint32_t h = read_u32_be_(buf + 8);
                const uint32_t idx = f % kBufN;
                remoteHashTag[idx] = f;
                remoteHash[idx] = h;
                if (localHashTag[idx] == f && localHash[idx] != h) {
                    if (localPlayerNum == 2) {
                        requestResyncJoiner_();
                    } else {
                        pendingResyncHost = true;
                    }
                }
                continue;
            }

            if (magic == kMagicResyncReq) {
                if (n < 8) continue;
                if (localPlayerNum == 1) {
                    pendingResyncHost = true;
                }
                continue;
            }

            if (magic == kMagicStateInfo) {
                // Accept both iOS legacy (>=20) and Android (>=16) formats.
                if (n < 16) continue;
                if (localPlayerNum != 2) continue;
                const uint32_t size = read_u32_be_(buf + 4);
                const uint32_t crc = read_u32_be_(buf + 8);
                const uint16_t chunkSize = read_u16_be_(buf + 12);
                const uint16_t chunkCount = read_u16_be_(buf + 14);
                // iOS legacy includes frame at +16; ignore.

                if (size == 0 || chunkSize == 0 || chunkCount == 0) continue;
                stateSize = size;
                stateCrc = crc;
                stateChunkSize = chunkSize;
                stateChunkCount = chunkCount;

                stateRx.assign(stateSize, 0);
                stateRxHave.assign(stateChunkCount, 0);
                stateRxHaveCount = 0;
                selfStateReady = false;
                joinAwaitingStateOffer = false;
                stateSyncDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                continue;
            }

            if (magic == kMagicStateChunk) {
                if (localPlayerNum != 2) continue;
                if (stateChunkCount == 0) continue;
                if (stateRxHave.empty() || stateRx.empty()) continue;

                uint16_t chunk = 0;
                const uint8_t* payload = nullptr;
                uint32_t payloadN = 0;

                if (n >= 12) {
                    // Android format: magic, idx, chunkCount, payloadSize, reserved, payload
                    const uint16_t idx = read_u16_be_(buf + 4);
                    const uint16_t ccnt = read_u16_be_(buf + 6);
                    const uint16_t psz = read_u16_be_(buf + 8);
                    if (ccnt != stateChunkCount) continue;
                    if (idx >= stateChunkCount) continue;
                    if (psz == 0) continue;
                    if (12u + static_cast<uint32_t>(psz) > static_cast<uint32_t>(n)) continue;
                    chunk = idx;
                    payload = buf + 12;
                    payloadN = psz;
                } else if (n >= 10) {
                    // Legacy iOS format: magic, crc, chunk, payload
                    const uint32_t crc = read_u32_be_(buf + 4);
                    const uint16_t idx = read_u16_be_(buf + 8);
                    if (crc != stateCrc) continue;
                    if (idx >= stateChunkCount) continue;
                    chunk = idx;
                    payload = buf + 10;
                    payloadN = static_cast<uint32_t>(n) - 10u;
                } else {
                    continue;
                }

                if (stateRxHave[chunk]) continue;

                const uint32_t off = static_cast<uint32_t>(chunk) * static_cast<uint32_t>(stateChunkSize);
                if (off >= stateSize) continue;
                const uint32_t copyN = std::min<uint32_t>(payloadN, stateSize - off);
                if (copyN == 0) continue;
                std::memcpy(stateRx.data() + off, payload, copyN);
                stateRxHave[chunk] = 1;
                stateRxHaveCount++;

                if (stateRxHaveCount == stateChunkCount) {
                    const uint32_t gotCrc = crc32_(stateRx.data(), stateRx.size());
                    if (gotCrc == stateCrc) {
                        (void)loadStateBytes_(stateRx.data(), stateRx.size());
                        selfStateReady = true;

                        // Android ack format: magic, size, crc
                        uint8_t ack[12] = {};
                        write_u32_be_(ack, kMagicStateAck);
                        write_u32_be_(ack + 4, stateSize);
                        write_u32_be_(ack + 8, stateCrc);
                        sendto(sock, ack, sizeof(ack), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
                    }
                }
                continue;
            }

            if (magic == kMagicStateAck) {
                if (localPlayerNum != 1) continue;
                // Accept Android (12) and legacy iOS (8) ack formats.
                if (n >= 12) {
                    const uint32_t sz = read_u32_be_(buf + 4);
                    const uint32_t crc = read_u32_be_(buf + 8);
                    if (sz == stateSize && crc == stateCrc) {
                        if (!peerStateReady && wantStateSync && !stateTx.empty()) {
                            (void)loadStateBytes_(stateTx.data(), stateTx.size());
                        }
                        peerStateReady = true;
                    }
                } else if (n >= 8) {
                    const uint32_t crc = read_u32_be_(buf + 4);
                    if (crc == stateCrc) {
                        if (!peerStateReady && wantStateSync && !stateTx.empty()) {
                            (void)loadStateBytes_(stateTx.data(), stateTx.size());
                        }
                        peerStateReady = true;
                    }
                }
                continue;
            }

            if (magic == kMagicSaveRamInfo) {
                // Accept both iOS legacy (>=20) and Android (>=16) formats.
                if (n < 16) continue;
                if (localPlayerNum != 2) continue;
                const uint32_t size = read_u32_be_(buf + 4);
                const uint32_t crc = read_u32_be_(buf + 8);
                const uint16_t chunkSize = read_u16_be_(buf + 12);
                const uint16_t chunkCount = read_u16_be_(buf + 14);
                uint16_t flags = 0;
                if (n >= 18) flags = read_u16_be_(buf + 16);

                if (size == 0 || chunkSize == 0 || chunkCount == 0) continue;
                saveRamSize = size;
                saveRamCrc = crc;
                saveRamChunkSize = chunkSize;
                saveRamChunkCount = chunkCount;
                saveRamGate = (flags & 1u) != 0;

                saveRamRx.assign(saveRamSize, 0);
                saveRamRxHave.assign(saveRamChunkCount, 0);
                saveRamRxHaveCount = 0;
                selfSaveRamReady = false;
                joinAwaitingSaveRamOffer = false;
                continue;
            }

            if (magic == kMagicSaveRamChunk) {
                if (localPlayerNum != 2) continue;
                if (saveRamChunkCount == 0) continue;
                if (saveRamRxHave.empty() || saveRamRx.empty()) continue;

                uint16_t chunk = 0;
                const uint8_t* payload = nullptr;
                uint32_t payloadN = 0;

                if (n >= 12) {
                    // Android format: magic, idx, chunkCount, payloadSize, reserved, payload
                    const uint16_t idx = read_u16_be_(buf + 4);
                    const uint16_t ccnt = read_u16_be_(buf + 6);
                    const uint16_t psz = read_u16_be_(buf + 8);
                    if (ccnt != saveRamChunkCount) continue;
                    if (idx >= saveRamChunkCount) continue;
                    if (psz == 0) continue;
                    if (12u + static_cast<uint32_t>(psz) > static_cast<uint32_t>(n)) continue;
                    chunk = idx;
                    payload = buf + 12;
                    payloadN = psz;
                } else if (n >= 10) {
                    // Legacy iOS format: magic, crc, chunk, payload
                    const uint32_t crc = read_u32_be_(buf + 4);
                    const uint16_t idx = read_u16_be_(buf + 8);
                    if (crc != saveRamCrc) continue;
                    if (idx >= saveRamChunkCount) continue;
                    chunk = idx;
                    payload = buf + 10;
                    payloadN = static_cast<uint32_t>(n) - 10u;
                } else {
                    continue;
                }

                if (saveRamRxHave[chunk]) continue;

                const uint32_t off = static_cast<uint32_t>(chunk) * static_cast<uint32_t>(saveRamChunkSize);
                if (off >= saveRamSize) continue;
                const uint32_t copyN = std::min<uint32_t>(payloadN, saveRamSize - off);
                if (copyN == 0) continue;
                std::memcpy(saveRamRx.data() + off, payload, copyN);
                saveRamRxHave[chunk] = 1;
                saveRamRxHaveCount++;

                if (saveRamRxHaveCount == saveRamChunkCount) {
                    const uint32_t gotCrc = crc32_(saveRamRx.data(), saveRamRx.size());
                    if (gotCrc == saveRamCrc) {
                        (void)applySaveRamBytes_(saveRamRx.data(), saveRamRx.size());
                        selfSaveRamReady = true;

                        // Android ack format: magic, size, crc
                        uint8_t ack[12] = {};
                        write_u32_be_(ack, kMagicSaveRamAck);
                        write_u32_be_(ack + 4, saveRamSize);
                        write_u32_be_(ack + 8, saveRamCrc);
                        sendto(sock, ack, sizeof(ack), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
                    }
                }
                continue;
            }

            if (magic == kMagicSaveRamAck) {
                if (localPlayerNum != 1) continue;
                // Accept Android (12) and legacy iOS (8) ack formats.
                if (n >= 12) {
                    const uint32_t sz = read_u32_be_(buf + 4);
                    const uint32_t crc = read_u32_be_(buf + 8);
                    if (sz == saveRamSize && crc == saveRamCrc) peerSaveRamReady = true;
                } else if (n >= 8) {
                    const uint32_t crc = read_u32_be_(buf + 4);
                    if (crc == saveRamCrc) peerSaveRamReady = true;
                }
                continue;
            }
        }

        // Mobile networks can have multi-second stalls; don't drop too aggressively.
        if (hasPeer && (std::chrono::steady_clock::now() - lastRecv) > std::chrono::seconds(20)) {
            hasPeer = false;
        }
    }

    void sendKeepAlive() noexcept {
        if (sock < 0) return;
        if (!hasPeer) return;
        if (remote.sin6_family != AF_INET6 || remote.sin6_port == 0) return;

        const auto now = std::chrono::steady_clock::now();
        if (lastKeepAliveSent.time_since_epoch().count() != 0 && (now - lastKeepAliveSent) < std::chrono::milliseconds(250)) {
            return;
        }
        lastKeepAliveSent = now;

        uint8_t pkt[8] = {};
        write_u32_be_(pkt, kMagicKeepAlive);
        write_u32_be_(pkt + 4, requireSecret ? secret32 : 0u);
        sendto(sock, pkt, sizeof(pkt), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
    }

    void sendNatPunchIfWaitingForPeer() noexcept {
        if (sock < 0) return;
        if (!discoverPeer) return;
        if (hasPeer) return;
        if (!natPunchReady) return;

        const auto now = std::chrono::steady_clock::now();
        if (lastNatPunchSent.time_since_epoch().count() != 0 && (now - lastNatPunchSent) < std::chrono::milliseconds(500)) {
            return;
        }
        lastNatPunchSent = now;

        uint8_t pkt[8] = {};
        write_u32_be_(pkt, kMagicKeepAlive);
        write_u32_be_(pkt + 4, requireSecret ? secret32 : 0u);
        sendto(sock, pkt, sizeof(pkt), 0, reinterpret_cast<const sockaddr*>(&natPunchTarget), sizeof(natPunchTarget));
    }

    void sendLocal(uint16_t localMask) noexcept {
        if (sock < 0) return;

        // Host auto-discovery: don't send inputs until we know where to send them.
        if (discoverPeer && !hasPeer) return;
        if (remote.sin6_family != AF_INET6 || remote.sin6_port == 0) return;

        const uint32_t sendFrame = frame + kInputDelayFrames;
        const uint32_t idx = sendFrame % kBufN;
        sentFrameTag[idx] = sendFrame;
        sentMask[idx] = localMask;

        const uint32_t start = (sendFrame > kResendWindow) ? (sendFrame - kResendWindow) : 0;
        for (uint32_t f = start; f <= sendFrame; ++f) {
            const uint32_t fi = f % kBufN;
            if (sentFrameTag[fi] != f) continue;

            Packet p{};
            p.magic_be = htonl(kMagicInput);
            p.frame_be = htonl(f);
            p.mask_be = htons(sentMask[fi]);
            p.reserved_be = htons(requireSecret ? secret16 : 0);
            sendto(sock, &p, sizeof(p), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
        }
    }

    bool tryGetInputsForCurrentFrame(uint16_t& outLocalMask, uint16_t& outRemoteMask) noexcept {
        const uint32_t need = frame;
        const uint32_t idx = need % kBufN;
        if (remoteFrameTag[idx] != need) return false;
        if (sentFrameTag[idx] != need) return false;
        outLocalMask = sentMask[idx];
        outRemoteMask = remoteMask[idx];
        return true;
    }
};

std::unique_ptr<UdpNetplay> g_netplay;
std::atomic<bool> g_netplayEnabled{false};
std::atomic<int> g_netplayStatus{0};

// --- Video buffer (RGBA8888 stored in uint32 as 0xAARRGGBB) ---
static constexpr int kMaxW = 512;
static constexpr int kMaxH = 512;
alignas(64) static uint32_t g_rgba[kMaxW * kMaxH];
static std::atomic<int> g_videoW{0};
static std::atomic<int> g_videoH{0};

static inline uint32_t toRGBA_fromXRGB8888(uint32_t xrgb) noexcept {
    const uint32_t r = (xrgb >> 8) & 0xFF;
    const uint32_t g = (xrgb >> 16) & 0xFF;
    const uint32_t b = (xrgb >> 24) & 0xFF;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline uint32_t toRGBA_fromRGB565(uint16_t p) noexcept {
    const uint32_t r = (p >> 11) & 0x1F;
    const uint32_t g = (p >> 5) & 0x3F;
    const uint32_t b = (p >> 0) & 0x1F;
    const uint32_t rr = (r << 3) | (r >> 2);
    const uint32_t gg = (g << 2) | (g >> 4);
    const uint32_t bb = (b << 3) | (b >> 2);
    return 0xFF000000u | (rr << 16) | (gg << 8) | bb;
}

static inline uint32_t toRGBA_from0RGB1555(uint16_t p) noexcept {
    const uint32_t r = (p >> 10) & 0x1F;
    const uint32_t g = (p >> 5) & 0x1F;
    const uint32_t b = (p >> 0) & 0x1F;
    const uint32_t rr = (r << 3) | (r >> 2);
    const uint32_t gg = (g << 3) | (g >> 2);
    const uint32_t bb = (b << 3) | (b >> 2);
    return 0xFF000000u | (rr << 16) | (gg << 8) | bb;
}

static void videoSink(void* /*ctx*/, const void* data, unsigned width, unsigned height, std::size_t pitchBytes) noexcept {
    if (!data || width == 0 || height == 0) return;
    if (width > static_cast<unsigned>(kMaxW) || height > static_cast<unsigned>(kMaxH)) return;

    const auto fmt = snesonline::EmulatorEngine::instance().core().pixelFormat();

    if (fmt == snesonline::LibretroCore::PixelFormat::XRGB8888) {
        const auto* src = static_cast<const uint8_t*>(data);
        for (unsigned y = 0; y < height; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(src + y * pitchBytes);
            uint32_t* dst = &g_rgba[y * kMaxW];
            for (unsigned x = 0; x < width; ++x) dst[x] = toRGBA_fromXRGB8888(row[x]);
        }
    } else if (fmt == snesonline::LibretroCore::PixelFormat::RGB565) {
        const auto* src = static_cast<const uint8_t*>(data);
        for (unsigned y = 0; y < height; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(src + y * pitchBytes);
            uint32_t* dst = &g_rgba[y * kMaxW];
            for (unsigned x = 0; x < width; ++x) dst[x] = toRGBA_fromRGB565(row[x]);
        }
    } else {
        const auto* src = static_cast<const uint8_t*>(data);
        for (unsigned y = 0; y < height; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(src + y * pitchBytes);
            uint32_t* dst = &g_rgba[y * kMaxW];
            for (unsigned x = 0; x < width; ++x) dst[x] = toRGBA_from0RGB1555(row[x]);
        }
    }

    g_videoW.store(static_cast<int>(width), std::memory_order_relaxed);
    g_videoH.store(static_cast<int>(height), std::memory_order_relaxed);
}

// --- Audio ring buffer (stereo S16) ---
static constexpr uint32_t kAudioCapacityFrames = 48000;
alignas(64) static int16_t g_audio[kAudioCapacityFrames * 2];
static std::atomic<uint32_t> g_audioW{0};
static std::atomic<uint32_t> g_audioR{0};

static std::size_t audioSink(void* /*ctx*/, const int16_t* stereoFrames, std::size_t frameCount) noexcept {
    if (!stereoFrames || frameCount == 0) return frameCount;
    uint32_t w = g_audioW.load(std::memory_order_relaxed);
    uint32_t r = g_audioR.load(std::memory_order_acquire);
    uint32_t used = w - r;
    if (used > kAudioCapacityFrames) used = kAudioCapacityFrames;
    uint32_t freeFrames = kAudioCapacityFrames - used;

    uint32_t frames = static_cast<uint32_t>(frameCount);
    if (frames > freeFrames) {
        const uint32_t drop = frames - freeFrames;
        g_audioR.store(r + drop, std::memory_order_release);
    }

    w = g_audioW.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t idx = (w + i) % kAudioCapacityFrames;
        g_audio[idx * 2 + 0] = stereoFrames[i * 2 + 0];
        g_audio[idx * 2 + 1] = stereoFrames[i * 2 + 1];
    }
    g_audioW.store(w + frames, std::memory_order_release);
    return frameCount;
}

void loop60fps() {
    using clock = std::chrono::steady_clock;
    constexpr auto frameDur = std::chrono::microseconds(16667);

    // Best-effort: prioritize emulation/audio production.
    (void)setpriority(PRIO_PROCESS, 0, -10);

    auto next = clock::now();
    while (g_running.load(std::memory_order_relaxed)) {
        const uint16_t localMask = g_inputMask.load(std::memory_order_relaxed);
        const bool paused = g_paused.load(std::memory_order_relaxed);

        constexpr int kMaxCatchUpFrames = 60;
        int steps = 0;
        auto now = clock::now();
        while (now >= next && steps < kMaxCatchUpFrames) {
            next += frameDur;

            maybeFlushSaveRam_(false);

            if (g_netplayEnabled.load(std::memory_order_relaxed) && g_netplay) {
                g_netplay->pumpRecv();

                // While hosting and waiting for peer discovery, keep NAT mapping alive.
                g_netplay->sendNatPunchIfWaitingForPeer();

                if (paused) {
                    g_netplay->sendKeepAlive();
                } else {
                    g_netplay->sendLocal(localMask);
                }

                g_netplay->pumpStateSyncSend();
                g_netplay->pumpSaveRamSyncSend();

                g_netplay->performResyncHostIfPending_();

                // Host: if SRAM changes, enqueue an update for the joiner.
                if (g_netplay->localPlayerNum == 1 && !g_saveRamPath.empty()) {
                    auto& core = snesonline::EmulatorEngine::instance().core();
                    void* mem = core.memoryData(kRetroMemorySaveRam_);
                    const std::size_t memSize = core.memorySize(kRetroMemorySaveRam_);
                    if (mem && memSize > 0) {
                        const uint32_t crc = crc32_(mem, memSize);
                        static uint32_t lastSentCrc = 0;
                        static auto lastSentAt = clock::time_point{};
                        const auto now2 = clock::now();
                        if (crc != lastSentCrc && (lastSentAt.time_since_epoch().count() == 0 || (now2 - lastSentAt) > std::chrono::seconds(2))) {
                            std::vector<uint8_t> bytes;
                            if (getSaveRamBytes_(bytes)) {
                                (void)g_netplay->queueSaveRamSync(std::move(bytes), false);
                                lastSentCrc = crc;
                                lastSentAt = now2;
                            }
                        }
                    }
                }

                if (!g_netplay->hasPeer) {
                    g_netplayStatus.store(1, std::memory_order_relaxed);
                    break;
                }

                if (!g_netplay->readyToRun()) {
                    g_netplayStatus.store(4, std::memory_order_relaxed);
                    break;
                }

                if (paused) {
                    g_netplayStatus.store(3, std::memory_order_relaxed);
                    break;
                }

                uint16_t localForFrame = 0;
                uint16_t remoteForFrame = 0;
                if (!g_netplay->tryGetInputsForCurrentFrame(localForFrame, remoteForFrame)) {
                    g_netplayStatus.store(2, std::memory_order_relaxed);
                    break;
                }

                g_netplayStatus.store(3, std::memory_order_relaxed);

                const bool localIsP1 = (g_netplay->localPlayerNum == 1);
                snesonline::EmulatorEngine::instance().setInputMask(0, localIsP1 ? localForFrame : remoteForFrame);
                snesonline::EmulatorEngine::instance().setInputMask(1, localIsP1 ? remoteForFrame : localForFrame);
                snesonline::EmulatorEngine::instance().advanceFrame();
                g_netplay->frame++;

                const uint32_t completedFrame = g_netplay->frame - 1;
                g_netplay->recordLocalHashForCompletedFrame_(completedFrame);
                g_netplay->maybeSendLocalHashForCompletedFrame_(completedFrame);
            } else {
                g_netplayStatus.store(0, std::memory_order_relaxed);
                if (paused) break;
                snesonline::EmulatorEngine::instance().setLocalInputMask(localMask);
                snesonline::EmulatorEngine::instance().advanceFrame();
            }

            steps++;
            now = clock::now();
            if (paused) break;
        }

        std::this_thread::sleep_until(next);
    }
}

} // namespace

int snesonline_ios_get_netplay_status(void) {
    return g_netplayStatus.load(std::memory_order_relaxed);
}

bool snesonline_ios_initialize(const char* corePath,
                              const char* romPath,
                              const char* statePath,
                              const char* savePath,
                              bool enableNetplay,
                              const char* remoteHost,
                              int remotePort,
                              int localPort,
                              int localPlayerNum,
                              const char* roomServerUrl,
                              const char* roomCode,
                              const char* sharedSecret) {
    g_running.store(false, std::memory_order_relaxed);
    if (g_loopThread.joinable()) g_loopThread.join();

    g_paused.store(false, std::memory_order_relaxed);

    g_netplayEnabled.store(false, std::memory_order_relaxed);
    g_netplayStatus.store(0, std::memory_order_relaxed);
    if (g_netplay) {
        g_netplay->stop();
        g_netplay.reset();
    }

    auto& eng = snesonline::EmulatorEngine::instance();
    eng.shutdown();

    if (!corePath || !corePath[0] || !romPath || !romPath[0]) return false;

    if (!eng.initialize(corePath, romPath)) return false;

    eng.core().setVideoSink(nullptr, &videoSink);
    eng.core().setAudioSink(nullptr, &audioSink);

    // Natural saves (SRAM):
    // - Offline: auto-load last per-ROM save (default path).
    // - Netplay: host does NOT auto-load unless savePath is explicitly passed.
    // - Netplay joiner never loads local SRAM (may receive host sync).
    const std::string defaultSavePath = makeDefaultSaveRamPathFromRom_(romPath);
    g_saveRamPath = (savePath && savePath[0]) ? std::string(savePath) : defaultSavePath;

    const bool wantNetplay = enableNetplay;
    const uint8_t pnum = static_cast<uint8_t>((localPlayerNum == 2) ? 2 : 1);

    if (!wantNetplay) {
        (void)loadSaveRamFromFile_(g_saveRamPath);
    } else if (pnum == 1) {
        if (savePath && savePath[0]) {
            (void)loadSaveRamFromFile_(g_saveRamPath);
        }
    }

    // Track current CRC so we only write on changes.
    {
        void* mem = eng.core().memoryData(kRetroMemorySaveRam_);
        const std::size_t memSize = eng.core().memorySize(kRetroMemorySaveRam_);
        g_saveRamLastCrc = (mem && memSize) ? crc32_(mem, memSize) : 0;
        g_saveRamLastCheck = {};
        g_saveRamLastFlush = {};
    }

    bool netplayOk = true;
    if (wantNetplay) {
        const uint16_t rp = static_cast<uint16_t>((remotePort >= 1 && remotePort <= 65535) ? remotePort : 7000);
        const uint16_t lp = static_cast<uint16_t>((localPort >= 1 && localPort <= 65535) ? localPort : 7000);

        auto np = std::make_unique<UdpNetplay>();
        const char* host = (remoteHost && remoteHost[0]) ? remoteHost : "";
        if (np->start(host, rp, lp, pnum, roomServerUrl ? roomServerUrl : "", roomCode ? roomCode : "", sharedSecret ? sharedSecret : "")) {
            // Host: stage the same state for the peer to load.
            if (pnum == 1 && statePath && statePath[0]) {
                std::vector<uint8_t> bytes;
                if (readFile_(statePath, bytes)) {
                    np->configureStateSyncHost(std::move(bytes));
                }
            }

            // Host: if SRAM was explicitly loaded, require peer to receive it before starting.
            if (pnum == 1 && savePath && savePath[0]) {
                std::vector<uint8_t> bytes;
                if (getSaveRamBytes_(bytes)) {
                    (void)np->queueSaveRamSync(std::move(bytes), true);
                }
            }

            // Note: we intentionally do NOT load a saved state locally before netplay starts.
            // The host applies the state only after the peer acknowledges it, so we can fall back
            // cleanly if the peer can't load the state.

            g_netplay = std::move(np);
            g_netplayEnabled.store(true, std::memory_order_relaxed);
        } else {
            netplayOk = false;
        }
    } else {
        // Offline: optional state load.
        if (statePath && statePath[0]) {
            std::vector<uint8_t> bytes;
            if (readFile_(statePath, bytes)) {
                (void)loadStateBytes_(bytes.data(), bytes.size());
            }
        }
    }

    return wantNetplay ? netplayOk : true;
}

void snesonline_ios_shutdown(void) {
    // Flush natural saves (SRAM) on exit.
    maybeFlushSaveRam_(true);

    g_running.store(false, std::memory_order_relaxed);
    if (g_loopThread.joinable()) g_loopThread.join();

    g_netplayEnabled.store(false, std::memory_order_relaxed);
    g_netplayStatus.store(0, std::memory_order_relaxed);
    if (g_netplay) {
        g_netplay->stop();
        g_netplay.reset();
    }

    snesonline::EmulatorEngine::instance().shutdown();
}

void snesonline_ios_start_loop(void) {
    if (g_running.exchange(true, std::memory_order_relaxed)) return;
    g_loopThread = std::thread(loop60fps);
}

void snesonline_ios_stop_loop(void) {
    g_running.store(false, std::memory_order_relaxed);
    if (g_loopThread.joinable()) g_loopThread.join();
}

void snesonline_ios_set_local_input_mask(uint16_t mask) {
    g_inputMask.store(mask, std::memory_order_relaxed);
}

void snesonline_ios_set_paused(bool paused) {
    g_paused.store(paused, std::memory_order_relaxed);
}

bool snesonline_ios_save_state_to_file(const char* statePath) {
    if (!statePath || !statePath[0]) return false;
    snesonline::SaveState st;
    const bool ok = snesonline::EmulatorEngine::instance().saveState(st);
    if (!ok || st.sizeBytes == 0 || !st.buffer.data()) return false;
    return writeFile_(statePath, st.buffer.data(), st.sizeBytes);
}

bool snesonline_ios_load_state_from_file(const char* statePath) {
    if (!statePath || !statePath[0]) return false;

    // Netplay rule: only host can load the canonical state.
    if (g_netplayEnabled.load(std::memory_order_relaxed) && g_netplay && g_netplay->localPlayerNum != 1) {
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!readFile_(statePath, bytes) || bytes.empty()) return false;

    const bool okLoad = loadStateBytes_(bytes.data(), bytes.size());
    if (!okLoad) return false;

    if (g_netplayEnabled.load(std::memory_order_relaxed) && g_netplay && g_netplay->localPlayerNum == 1) {
        g_netplay->configureStateSyncHost(std::move(bytes));
        g_netplay->peerStateReady = false;
    }

    return true;
}

int snesonline_ios_stun_public_udp_port(int localPort) {
    const uint16_t lp = static_cast<uint16_t>((localPort >= 1 && localPort <= 65535) ? localPort : 0);
    if (lp == 0) return 0;
    snesonline::StunMappedAddress out;
    if (!snesonline::stunDiscoverMappedAddressDefault(lp, out, 2000)) return 0;
    return static_cast<int>(out.port);
}

const char* snesonline_ios_stun_mapped_address(int localPort) {
    thread_local std::string s;
    s.clear();
    const uint16_t lp = static_cast<uint16_t>((localPort >= 1 && localPort <= 65535) ? localPort : 0);
    if (lp == 0) return "";
    snesonline::StunMappedAddress out;
    if (!snesonline::stunDiscoverMappedAddressDefault(lp, out, 2000)) return "";
    if (out.ip.empty() || out.port == 0) return "";
    if (out.ip.find(':') != std::string::npos) {
        s = "[" + out.ip + "]:" + std::to_string(out.port);
    } else {
        s = out.ip + ":" + std::to_string(out.port);
    }
    return s.c_str();
}

int snesonline_ios_get_video_width(void) {
    return g_videoW.load(std::memory_order_relaxed);
}

int snesonline_ios_get_video_height(void) {
    return g_videoH.load(std::memory_order_relaxed);
}

const uint32_t* snesonline_ios_get_video_buffer_rgba(void) {
    return g_rgba;
}

int snesonline_ios_get_audio_sample_rate_hz(void) {
    const double hz = snesonline::EmulatorEngine::instance().core().sampleRateHz();
    int out = (int)(hz + 0.5);
    if (out < 8000 || out > 192000) out = 48000;
    return out;
}

int snesonline_ios_pop_audio(int16_t* dstInterleavedStereo, int framesWanted) {
    if (!dstInterleavedStereo || framesWanted <= 0) return 0;

    uint32_t r = g_audioR.load(std::memory_order_relaxed);
    uint32_t w = g_audioW.load(std::memory_order_acquire);
    uint32_t avail = w - r;
    if (avail > kAudioCapacityFrames) avail = kAudioCapacityFrames;

    uint32_t frames = (uint32_t)framesWanted;
    if (frames > avail) frames = avail;

    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t idx = (r + i) % kAudioCapacityFrames;
        dstInterleavedStereo[i * 2 + 0] = g_audio[idx * 2 + 0];
        dstInterleavedStereo[i * 2 + 1] = g_audio[idx * 2 + 1];
    }

    g_audioR.store(r + frames, std::memory_order_release);
    return (int)frames;
}

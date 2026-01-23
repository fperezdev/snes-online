#include <jni.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>

#include "snesonline/EmulatorEngine.h"
#include "snesonline/InputBits.h"
#include "snesonline/InputMapping.h"
#include "snesonline/StunClient.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

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

static std::string makeSaveRamPathFromRom_(const char* romPath) {
    if (!romPath || !romPath[0]) return {};
    std::string rom(romPath);
    const std::string romDir = dirname_(rom);
    const std::string rootDir = dirname_(romDir);
    if (rootDir.empty()) return {};
    const std::string saveDir = rootDir + "/saves";
    (void)ensureDir_(saveDir);
    return saveDir + "/" + basenameNoExt_(rom) + ".srm";
}

static constexpr unsigned kRetroMemorySaveRam_ = 0; // RETRO_MEMORY_SAVE_RAM
static constexpr unsigned kRetroMemorySystemRam_ = 2; // RETRO_MEMORY_SYSTEM_RAM

static std::string g_saveRamPath;
static bool g_saveRamLoadedFromFile = false;
static bool g_saveRamLoadedExplicitly = false;
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
    g_saveRamLoadedFromFile = false;
    if (path.empty()) return false;
    std::vector<uint8_t> bytes;
    if (!readFile_(path.c_str(), bytes)) return false;
    if (!applySaveRamBytes_(bytes.data(), bytes.size())) return false;
    g_saveRamLoadedFromFile = true;
    return true;
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

    // Tiny receive buffer keyed by frame % N.
    static constexpr uint32_t kBufN = 256;
    // Delay inputs slightly to absorb jitter (both peers must use same value).
    static constexpr uint32_t kInputDelayFrames = 5;
    // Re-send a small window of recent frames to survive UDP loss.
    static constexpr uint32_t kResendWindow = 16;

    // Periodically exchange a light-weight checksum so we can detect true emulator desyncs.
    // (Does not affect determinism; it only detects mismatch and triggers a resync.)
    static constexpr uint32_t kHashIntervalFrames = 60; // ~1s @60fps
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
    bool peerStateReady = false; // host waits until peer acks
    bool selfStateReady = true;  // joiner waits until loaded

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

    // SaveRAM (SRAM) sync. Used both for initial join sync and background updates.
    bool wantSaveRamSync = false;
    bool saveRamGate = false; // if true, blocks readyToRun until ack/loaded
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
        // Avoid restarting the same transfer.
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
        if (t.front() == '[') {
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
        // Trim spaces.
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
        size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
        s = s.substr(b);

        // Strip scheme.
        const auto pos = s.find("://");
        if (pos != std::string::npos) s = s.substr(pos + 3);
        if (s.empty()) return false;

        // Strip path.
        const auto slash = s.find('/');
        if (slash != std::string::npos) s = s.substr(0, slash);

        // host:port or [ipv6]:port
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

        // Normalize code (A-Z0-9).
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

                // Ensure NUL-terminated.
                buf[(n >= static_cast<int>(sizeof(buf))) ? (static_cast<int>(sizeof(buf)) - 1) : n] = 0;
                const char* payload = buf + 9;
                while (*payload == ' ' || *payload == '\t') ++payload;
                std::string peerHost;
                uint16_t peerPort = 0;
                if (!parseHostPort_(payload, peerHost, peerPort)) continue;

                sockaddr_in6 peer{};
                if (!resolveIpAnyToIn6_(peerHost.c_str(), peer, peerPort)) continue;

                remote = peer;
                remotePort = peerPort;
                discoverPeer = false;
                // Allow sending immediately.
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        return false;
    }

    bool start(const char* remoteHost, uint16_t rPort, uint16_t lPort, uint8_t lPlayer, const char* roomServerUrl, const char* roomCode, const char* sharedSecret) noexcept {
        stop();

        localPort = (lPort != 0) ? lPort : 7000;
        remotePort = (rPort != 0) ? rPort : 7000;
        localPlayerNum = (lPlayer == 2) ? 2 : 1;

        // Gameplay role (P1 = host, P2 = joiner), independent of whether state sync is active.
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
        localHash.fill(0);
        remoteHashTag.fill(0xFFFFFFFFu);
        remoteHash.fill(0);
        frame = 0;

        // Prime our local input history for the first few frames so the sim can start immediately.
        for (uint32_t f = 0; f < kInputDelayFrames; ++f) {
            const uint32_t idx = f % kBufN;
            sentFrameTag[idx] = f;
            sentMask[idx] = 0;
        }

        hasPeer = false;
        lastRecv = {};

        pendingResyncHost = false;
        lastResyncRequestSent = {};
        lastResyncTriggered = {};

        discoverPeer = false;

        requireSecret = (sharedSecret && sharedSecret[0]);
        secret32 = requireSecret ? crc32_(sharedSecret, std::strlen(sharedSecret)) : 0u;
        uint32_t mix = secret32 ^ (secret32 >> 16);
        secret16 = static_cast<uint16_t>(mix & 0xFFFFu);
        if (requireSecret && secret16 == 0) secret16 = 1;

        const bool hasRemoteHost = (remoteHost && remoteHost[0]);
        if (!hasRemoteHost) {
            // Host convenience: allow Player 1 to leave remoteHost empty and auto-discover from the first packet.
            if (localPlayerNum != 1) {
                return false;
            }
            discoverPeer = true;
            remote = {};
            remote.sin6_family = AF_INET6;
            remote.sin6_port = 0;
        } else {
            if (!resolveIpAnyToIn6_(remoteHost, remote, remotePort)) {
                return false;
            }
        }

        sock = ::socket(AF_INET6, SOCK_DGRAM, 0);
        if (sock < 0) {
            sock = -1;
            return false;
        }

        int v6only = 0;
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

        // Reuse address helps quick restarts.
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        // Larger buffers reduce drops on jittery Wi-Fi.
        int bufBytes = 1 << 20;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufBytes, sizeof(bufBytes));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufBytes, sizeof(bufBytes));

        sockaddr_in6 local{};
        local.sin6_family = AF_INET6;
        local.sin6_addr = in6addr_any;
        local.sin6_port = htons(localPort);
        if (bind(sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            stop();
            return false;
        }

        if (!setNonBlocking(sock)) {
            stop();
            return false;
        }

        // Room flow does not use server-assisted peer rendezvous here.

        // Joiner waits briefly for a possible state offer.
        if (localPlayerNum == 2) {
            configureStateSyncJoiner();
            configureSaveRamSyncJoiner();
        }

        return true;
    }

    void stop() noexcept {
        if (sock >= 0) {
            ::close(sock);
            sock = -1;
        }
        hasPeer = false;
    }

    void pumpRecv() noexcept {
        if (sock < 0) return;
        while (true) {
            uint8_t buf[1500] = {};
            sockaddr_in6 from{};
            socklen_t fromLen = sizeof(from);
            const int n = static_cast<int>(recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
            if (n < 0) {
                break;
            }
            if (n < 4) continue;

            const uint32_t magic = read_u32_be_(buf);

            // Host auto-discovery: only accept the first peer if it presents the correct secret token.
            if (discoverPeer && !hasPeer) {
                if (magic != kMagicInput) continue;
                if (n != static_cast<int>(sizeof(Packet))) continue;
                if (requireSecret) {
                    const uint16_t tok = read_u16_be_(buf + 10);
                    if (tok != secret16) continue;
                }
            }

            // Validate token on input packets (best-effort). If it doesn't match, ignore.
            if (magic == kMagicInput && n == static_cast<int>(sizeof(Packet)) && requireSecret) {
                const uint16_t tok = read_u16_be_(buf + 10);
                if (tok != secret16) continue;
            }

            // Validate token on keepalives.
            if (magic == kMagicKeepAlive && requireSecret) {
                if (n < 8) continue;
                const uint32_t tok = read_u32_be_(buf + 4);
                if (tok != secret32) continue;
            }

            // Learn/refresh peer endpoint from observed packets (NATs may rewrite source ports).
            if (discoverPeer) {
                if (!hasPeer) {
                    // Lock onto the first peer we observe.
                    remote = from;
                    remotePort = static_cast<uint16_t>(ntohs(from.sin6_port));
                } else if (remote.sin6_scope_id == from.sin6_scope_id && std::memcmp(&remote.sin6_addr, &from.sin6_addr, sizeof(in6_addr)) == 0) {
                    remote.sin6_port = from.sin6_port;
                    remotePort = static_cast<uint16_t>(ntohs(from.sin6_port));
                }
            } else {
                // If user configured a host/IP, keep IP pinned but let the port float if NAT changes it.
                if (remote.sin6_scope_id == from.sin6_scope_id && std::memcmp(&remote.sin6_addr, &from.sin6_addr, sizeof(in6_addr)) == 0) {
                    remote.sin6_port = from.sin6_port;
                    remotePort = static_cast<uint16_t>(ntohs(from.sin6_port));
                }
            }

            hasPeer = true;
            lastRecv = std::chrono::steady_clock::now();

            if (magic == kMagicKeepAlive) {
                // Keepalive: refresh peer/lastRecv only.
                continue;
            }

            if (magic == kMagicInput) {
                if (n != static_cast<int>(sizeof(Packet))) continue;
                const uint32_t f = read_u32_be_(buf + 4);
                const uint16_t m = read_u16_be_(buf + 8);
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

                // If we have a local hash for the same completed frame, compare.
                if (localHashTag[idx] == f && localHash[idx] != 0 && h != 0 && localHash[idx] != h) {
                    if (localPlayerNum == 1) {
                        pendingResyncHost = true;
                    } else {
                        // Ask host to resync. Cooldown prevents spamming.
                        const auto now = std::chrono::steady_clock::now();
                        if (lastResyncRequestSent.time_since_epoch().count() == 0 || (now - lastResyncRequestSent) > std::chrono::seconds(2)) {
                            uint8_t req[8] = {};
                            write_u32_be_(req, kMagicResyncReq);
                            write_u32_be_(req + 4, f);
                            sendto(sock, req, sizeof(req), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
                            lastResyncRequestSent = now;
                        }
                    }
                }
                continue;
            }

            if (magic == kMagicResyncReq) {
                if (n < 8) continue;
                if (localPlayerNum != 1) continue;
                // Joiner requested a state resync (likely due to desync detection).
                pendingResyncHost = true;
                continue;
            }

            if (magic == kMagicStateInfo) {
                if (n < 16) continue;
                if (isHost) continue;
                const uint32_t sz = read_u32_be_(buf + 4);
                const uint32_t crc = read_u32_be_(buf + 8);
                const uint16_t csz = read_u16_be_(buf + 12);
                const uint16_t ccnt = read_u16_be_(buf + 14);
                if (sz == 0 || csz == 0 || ccnt == 0) continue;

                if (!wantStateSync) {
                    wantStateSync = true;
                    selfStateReady = false;
                    joinAwaitingStateOffer = false;
                    stateSyncDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    stateSize = sz;
                    stateCrc = crc;
                    stateChunkSize = csz;
                    stateChunkCount = ccnt;
                    stateRxHaveCount = 0;
                    try {
                        stateRx.assign(stateSize, 0);
                        stateRxHave.assign(stateChunkCount, 0);
                    } catch (...) {
                        wantStateSync = false;
                        selfStateReady = true;
                    }
                }
                continue;
            }

            if (magic == kMagicStateChunk) {
                if (n < 12) continue;
                if (isHost) continue;
                if (!wantStateSync || selfStateReady) continue;
                const uint16_t idx = read_u16_be_(buf + 4);
                const uint16_t ccnt = read_u16_be_(buf + 6);
                const uint16_t psz = read_u16_be_(buf + 8);
                if (ccnt != stateChunkCount) continue;
                if (idx >= stateChunkCount) continue;
                if (psz == 0) continue;
                if (12 + psz > static_cast<uint16_t>(n)) continue;

                if (!stateRxHave.empty() && stateRxHave[idx] == 0) {
                    stateRxHave[idx] = 1;
                    stateRxHaveCount++;
                }

                const uint32_t off = static_cast<uint32_t>(idx) * static_cast<uint32_t>(stateChunkSize);
                if (off >= stateSize) continue;
                const uint32_t maxCopy = stateSize - off;
                const uint32_t copyN = (psz < maxCopy) ? psz : maxCopy;
                std::memcpy(stateRx.data() + off, buf + 12, copyN);

                if (stateRxHaveCount == stateChunkCount) {
                    const uint32_t got = crc32_(stateRx.data(), stateRx.size());
                    if (got == stateCrc) {
                        if (loadStateBytes_(stateRx.data(), stateRx.size())) {
                            selfStateReady = true;
                            uint8_t ack[12] = {};
                            write_u32_be_(ack, kMagicStateAck);
                            write_u32_be_(ack + 4, stateSize);
                            write_u32_be_(ack + 8, stateCrc);
                            sendto(sock, ack, sizeof(ack), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
                        }
                    }
                }
                continue;
            }

            if (magic == kMagicStateAck) {
                if (n < 12) continue;
                if (!isHost || !wantStateSync) continue;
                const uint32_t sz = read_u32_be_(buf + 4);
                const uint32_t crc = read_u32_be_(buf + 8);
                if (sz == stateSize && crc == stateCrc) {
                    peerStateReady = true;
                }
                continue;
            }

            if (magic == kMagicSaveRamInfo) {
                if (n < 16) continue;
                if (isHost) continue;
                const uint32_t sz = read_u32_be_(buf + 4);
                const uint32_t crc = read_u32_be_(buf + 8);
                const uint16_t csz = read_u16_be_(buf + 12);
                const uint16_t ccnt = read_u16_be_(buf + 14);
                if (sz == 0 || csz == 0 || ccnt == 0) continue;

                uint16_t flags = 0;
                if (n >= 18) flags = read_u16_be_(buf + 16);
                const bool gate = (flags & 1u) != 0;

                // Start (or restart) a save-ram transfer.
                wantSaveRamSync = true;
                saveRamGate = gate;
                selfSaveRamReady = !saveRamGate;
                saveRamSize = sz;
                saveRamCrc = crc;
                saveRamChunkSize = csz;
                saveRamChunkCount = ccnt;
                saveRamRxHaveCount = 0;
                try {
                    saveRamRx.assign(saveRamSize, 0);
                    saveRamRxHave.assign(saveRamChunkCount, 0);
                } catch (...) {
                    wantSaveRamSync = false;
                    saveRamGate = false;
                    selfSaveRamReady = true;
                }
                continue;
            }

            if (magic == kMagicSaveRamChunk) {
                if (n < 12) continue;
                if (isHost) continue;
                if (!wantSaveRamSync) continue;
                const uint16_t idx = read_u16_be_(buf + 4);
                const uint16_t ccnt = read_u16_be_(buf + 6);
                const uint16_t psz = read_u16_be_(buf + 8);
                if (ccnt != saveRamChunkCount) continue;
                if (idx >= saveRamChunkCount) continue;
                if (psz == 0) continue;
                if (12 + psz > static_cast<uint16_t>(n)) continue;

                if (!saveRamRxHave.empty() && saveRamRxHave[idx] == 0) {
                    saveRamRxHave[idx] = 1;
                    saveRamRxHaveCount++;
                }

                const uint32_t off = static_cast<uint32_t>(idx) * static_cast<uint32_t>(saveRamChunkSize);
                if (off >= saveRamSize) continue;
                const uint32_t maxCopy = saveRamSize - off;
                const uint32_t copyN = (psz < maxCopy) ? psz : maxCopy;
                std::memcpy(saveRamRx.data() + off, buf + 12, copyN);

                if (saveRamRxHaveCount == saveRamChunkCount) {
                    const uint32_t got = crc32_(saveRamRx.data(), saveRamRx.size());
                    if (got == saveRamCrc) {
                        // Apply to core memory and persist.
                        (void)applySaveRamBytes_(saveRamRx.data(), saveRamRx.size());
                        if (!g_saveRamPath.empty()) {
                            (void)writeFile_(g_saveRamPath.c_str(), saveRamRx.data(), saveRamRx.size());
                            g_saveRamLastCrc = saveRamCrc;
                        }

                        uint8_t ack[12] = {};
                        write_u32_be_(ack, kMagicSaveRamAck);
                        write_u32_be_(ack + 4, saveRamSize);
                        write_u32_be_(ack + 8, saveRamCrc);
                        sendto(sock, ack, sizeof(ack), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));

                        if (saveRamGate) {
                            selfSaveRamReady = true;
                            saveRamGate = false;
                        }

                        // Keep wantSaveRamSync=true so we can accept future updates; reset receive state.
                        saveRamRxHaveCount = 0;
                    }
                }
                continue;
            }

            if (magic == kMagicSaveRamAck) {
                if (n < 12) continue;
                if (!isHost || !wantSaveRamSync) continue;
                const uint32_t sz = read_u32_be_(buf + 4);
                const uint32_t crc = read_u32_be_(buf + 8);
                if (sz == saveRamSize && crc == saveRamCrc) {
                    if (saveRamGate) peerSaveRamReady = true;
                    // Stop sending until we queue another update.
                    wantSaveRamSync = false;
                    saveRamGate = false;
                }
                continue;
            }
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

    bool computeFastStateHash_(uint32_t& outHash) noexcept {
        outHash = 0;
        // Prefer a cheap deterministic region (system RAM) to avoid frequent full savestates.
        auto& core = snesonline::EmulatorEngine::instance().core();
        void* mem = core.memoryData(kRetroMemorySystemRam_);
        const std::size_t memSize = core.memorySize(kRetroMemorySystemRam_);
        if (!mem || memSize == 0) return false;
        outHash = crc32_(mem, memSize);
        return true;
    }

    void recordLocalHashForCompletedFrame_(uint32_t completedFrame) noexcept {
        uint32_t h = 0;
        if (!computeFastStateHash_(h)) return;
        if (h == 0) return;
        const uint32_t idx = completedFrame % kBufN;
        localHashTag[idx] = completedFrame;
        localHash[idx] = h;
    }

    void maybeSendLocalHashForCompletedFrame_(uint32_t completedFrame) noexcept {
        if (sock < 0) return;
        if (!hasPeer) return;
        if (discoverPeer && !hasPeer) return;
        if (remote.sin6_family != AF_INET6 || remote.sin6_port == 0) return;
        if (kHashIntervalFrames == 0) return;
        if ((completedFrame % kHashIntervalFrames) != 0) return;

        const uint32_t idx = completedFrame % kBufN;
        if (localHashTag[idx] != completedFrame) return;
        const uint32_t h = localHash[idx];
        if (h == 0) return;

        uint8_t pkt[12] = {};
        write_u32_be_(pkt, kMagicHash);
        write_u32_be_(pkt + 4, completedFrame);
        write_u32_be_(pkt + 8, h);
        sendto(sock, pkt, sizeof(pkt), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
    }

    void performResyncHostIfPending_() noexcept {
        if (localPlayerNum != 1) return;
        if (!pendingResyncHost) return;
        pendingResyncHost = false;

        // Avoid re-triggering too frequently.
        const auto now = std::chrono::steady_clock::now();
        if (lastResyncTriggered.time_since_epoch().count() != 0 && (now - lastResyncTriggered) < std::chrono::seconds(2)) {
            return;
        }

        // Don't stomp an in-flight transfer.
        if (wantStateSync && !peerStateReady) {
            return;
        }

        snesonline::SaveState st;
        if (!snesonline::EmulatorEngine::instance().saveState(st)) {
            return;
        }
        if (st.sizeBytes == 0 || !st.buffer.data()) {
            return;
        }

        std::vector<uint8_t> bytes;
        try {
            bytes.resize(st.sizeBytes);
        } catch (...) {
            return;
        }
        std::memcpy(bytes.data(), st.buffer.data(), st.sizeBytes);
        configureStateSyncHost(std::move(bytes));
        lastResyncTriggered = now;
    }

    void pumpSaveRamSyncSend() noexcept {
        if (sock < 0) return;
        if (!isHost || !wantSaveRamSync) return;
        if (!hasPeer) return;
        if (discoverPeer && !hasPeer) return;
        if (remote.sin6_family != AF_INET6 || remote.sin6_port == 0) return;

        // If this transfer is gating startup, don't spam forever once acked.
        if (saveRamGate && peerSaveRamReady) return;

        const auto now = std::chrono::steady_clock::now();

        if (lastSaveRamInfoSent.time_since_epoch().count() == 0 || now - lastSaveRamInfoSent >= std::chrono::milliseconds(250)) {
            uint8_t info[18] = {};
            write_u32_be_(info, kMagicSaveRamInfo);
            write_u32_be_(info + 4, saveRamSize);
            write_u32_be_(info + 8, saveRamCrc);
            write_u16_be_(info + 12, saveRamChunkSize);
            write_u16_be_(info + 14, saveRamChunkCount);
            write_u16_be_(info + 16, saveRamGate ? 1u : 0u);
            sendto(sock, info, sizeof(info), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
            lastSaveRamInfoSent = now;
        }

        const uint16_t burst = 6;
        for (uint16_t k = 0; k < burst; ++k) {
            const uint16_t idx = nextSaveRamChunkToSend;
            nextSaveRamChunkToSend = static_cast<uint16_t>((nextSaveRamChunkToSend + 1) % saveRamChunkCount);
            const uint32_t off = static_cast<uint32_t>(idx) * static_cast<uint32_t>(saveRamChunkSize);
            if (off >= saveRamSize) continue;
            const uint32_t remaining = saveRamSize - off;
            const uint16_t psz = static_cast<uint16_t>((remaining > saveRamChunkSize) ? saveRamChunkSize : remaining);

            uint8_t pkt[12 + 1024] = {};
            write_u32_be_(pkt, kMagicSaveRamChunk);
            write_u16_be_(pkt + 4, idx);
            write_u16_be_(pkt + 6, saveRamChunkCount);
            write_u16_be_(pkt + 8, psz);
            write_u16_be_(pkt + 10, 0);
            std::memcpy(pkt + 12, saveRamTx.data() + off, psz);
            sendto(sock, pkt, static_cast<int>(12 + psz), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
        }
    }

    void pumpStateSyncSend() noexcept {
        if (sock < 0) return;
        if (!isHost || !wantStateSync) return;
        if (!hasPeer || peerStateReady) return;
        if (discoverPeer && !hasPeer) return;
        if (remote.sin6_family != AF_INET6 || remote.sin6_port == 0) return;

        const auto now = std::chrono::steady_clock::now();

        // Periodically send state offer.
        if (lastInfoSent.time_since_epoch().count() == 0 || now - lastInfoSent >= std::chrono::milliseconds(250)) {
            uint8_t info[16] = {};
            write_u32_be_(info, kMagicStateInfo);
            write_u32_be_(info + 4, stateSize);
            write_u32_be_(info + 8, stateCrc);
            write_u16_be_(info + 12, stateChunkSize);
            write_u16_be_(info + 14, stateChunkCount);
            sendto(sock, info, sizeof(info), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
            lastInfoSent = now;
        }

        // Burst a few chunks per call.
        const uint16_t burst = 6;
        for (uint16_t k = 0; k < burst; ++k) {
            const uint16_t idx = nextChunkToSend;
            nextChunkToSend = static_cast<uint16_t>((nextChunkToSend + 1) % stateChunkCount);
            const uint32_t off = static_cast<uint32_t>(idx) * static_cast<uint32_t>(stateChunkSize);
            if (off >= stateSize) continue;
            const uint32_t remaining = stateSize - off;
            const uint16_t psz = static_cast<uint16_t>((remaining > stateChunkSize) ? stateChunkSize : remaining);

            uint8_t pkt[12 + 1024] = {};
            write_u32_be_(pkt, kMagicStateChunk);
            write_u16_be_(pkt + 4, idx);
            write_u16_be_(pkt + 6, stateChunkCount);
            write_u16_be_(pkt + 8, psz);
            write_u16_be_(pkt + 10, 0);
            std::memcpy(pkt + 12, stateTx.data() + off, psz);
            sendto(sock, pkt, static_cast<int>(12 + psz), 0, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
        }
    }

    bool readyToRun() noexcept {
        if (!hasPeer) return false;
        const auto now = std::chrono::steady_clock::now();
        if (!isHost && joinAwaitingStateOffer) {
            if (now < joinWaitStateOfferDeadline) {
                return false;
            }
            joinAwaitingStateOffer = false;
        }

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
                // State offer received but never successfully loaded. Fall back to starting from reset.
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
        if (isHost && saveRamGate) return peerSaveRamReady;
        if (!isHost && saveRamGate) return selfSaveRamReady;
        return true;
    }

    void sendLocal(uint16_t mask) noexcept {
        if (sock < 0) return;
        const uint32_t targetFrame = frame + kInputDelayFrames;
        const uint32_t idx = targetFrame % kBufN;
        sentFrameTag[idx] = targetFrame;
        sentMask[idx] = mask;

        // In host auto-discovery mode, wait for the first inbound packet so we learn the peer endpoint.
        // In normal client mode (remote host configured), we must send immediately to initiate handshake.
        if (discoverPeer && !hasPeer) return;
        if (remote.sin6_family != AF_INET6 || remote.sin6_port == 0) return;

        const uint32_t start = (targetFrame >= (kResendWindow - 1)) ? (targetFrame - (kResendWindow - 1)) : 0u;
        for (uint32_t f = start; f <= targetFrame; ++f) {
            const uint32_t i = f % kBufN;
            if (sentFrameTag[i] != f) continue;
            Packet p{};
            p.magic_be = htonl(kMagicInput);
            p.frame_be = htonl(f);
            p.mask_be = htons(sentMask[i]);
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

// 0=off, 1=connecting (no peer), 2=waiting (missing inputs), 3=ok
// 4=syncing state
std::atomic<int> g_netplayStatus{0};

// --- Video buffer (RGBA8888) ---
static constexpr int kMaxW = 512;
static constexpr int kMaxH = 512;
alignas(64) static uint32_t g_rgba[kMaxW * kMaxH];
static std::atomic<int> g_videoW{0};
static std::atomic<int> g_videoH{0};
static std::atomic<uint32_t> g_videoSeq{0};

static inline uint32_t toRGBA_fromXRGB8888(uint32_t xrgb) noexcept {
    // RETRO_PIXEL_FORMAT_XRGB8888 is X,R,G,B byte order.
    // When read as a uint32 on little-endian CPUs, the bytes land as:
    //   xrgb = (B<<24) | (G<<16) | (R<<8) | X
    // Convert to ARGB8888 int (0xAARRGGBB).
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
            for (unsigned x = 0; x < width; ++x) {
                dst[x] = toRGBA_fromXRGB8888(row[x]);
            }
        }
    } else if (fmt == snesonline::LibretroCore::PixelFormat::RGB565) {
        const auto* src = static_cast<const uint8_t*>(data);
        for (unsigned y = 0; y < height; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(src + y * pitchBytes);
            uint32_t* dst = &g_rgba[y * kMaxW];
            for (unsigned x = 0; x < width; ++x) {
                dst[x] = toRGBA_fromRGB565(row[x]);
            }
        }
    } else {
        // XRGB1555
        const auto* src = static_cast<const uint8_t*>(data);
        for (unsigned y = 0; y < height; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(src + y * pitchBytes);
            uint32_t* dst = &g_rgba[y * kMaxW];
            for (unsigned x = 0; x < width; ++x) {
                dst[x] = toRGBA_from0RGB1555(row[x]);
            }
        }
    }

    g_videoW.store(static_cast<int>(width), std::memory_order_relaxed);
    g_videoH.store(static_cast<int>(height), std::memory_order_relaxed);
    g_videoSeq.fetch_add(1, std::memory_order_relaxed);
}

// --- Audio ring buffer (stereo S16) ---
static constexpr uint32_t kAudioCapacityFrames = 48000; // ~1s @48k
// Keep buffered audio bounded to avoid perceived "audio lag".
// If we get ahead (e.g., emulator catch-up frames), drop oldest samples.
// Drop gradually to avoid audible "rubber-band" jumps.
// Default: no latency clamp (prioritize stable audio over minimum latency).
// The ring buffer itself is ~1s, so this still won't grow unbounded.
static std::atomic<uint32_t> g_audioMaxBufferedFrames{0};
static std::atomic<uint32_t> g_audioMaxDropPerCallFrames{0};
alignas(64) static int16_t g_audio[kAudioCapacityFrames * 2];
static std::atomic<uint32_t> g_audioW{0};
static std::atomic<uint32_t> g_audioR{0};
static std::atomic<uint64_t> g_audioDroppedTotalFrames{0};
static std::atomic<uint64_t> g_audioUnderflowTotalFrames{0};

// Loop timing stats (diagnostics)
static std::atomic<uint64_t> g_loopFramesTotal{0};
static std::atomic<uint64_t> g_loopCatchupEventsTotal{0};
static std::atomic<uint64_t> g_loopCatchupFramesTotal{0};

static void configureAudioLatencyCaps(double sampleRateHz) noexcept {
    int sr = static_cast<int>(sampleRateHz + 0.5);
    if (sr < 8000 || sr > 192000) sr = 48000;

    // Target ~50ms of buffered audio max, with a small per-callback drop cap.
    // This keeps latency tight while avoiding large discontinuities.
    const int targetMs = 50;
    uint32_t maxBuf = static_cast<uint32_t>((static_cast<uint64_t>(sr) * targetMs) / 1000);
    if (maxBuf < 1024) maxBuf = 1024;
    if (maxBuf > (kAudioCapacityFrames / 2)) maxBuf = (kAudioCapacityFrames / 2);

    // Cap drops to ~5ms of audio per callback.
    uint32_t maxDrop = static_cast<uint32_t>((static_cast<uint64_t>(sr) * 5) / 1000);
    if (maxDrop < 128) maxDrop = 128;
    if (maxDrop > 1024) maxDrop = 1024;

    g_audioMaxBufferedFrames.store(maxBuf, std::memory_order_relaxed);
    g_audioMaxDropPerCallFrames.store(maxDrop, std::memory_order_relaxed);
}

static void setAudioLatencyCapsMs(int maxBufferedMs, int maxDropMs) noexcept {
    // Convert ms -> frames using the core's current sample rate.
    const double hz = snesonline::EmulatorEngine::instance().core().sampleRateHz();
    int sr = static_cast<int>(hz + 0.5);
    if (sr < 8000 || sr > 192000) sr = 48000;

    uint32_t maxBufFrames = 0;
    if (maxBufferedMs > 0) {
        maxBufFrames = static_cast<uint32_t>((static_cast<uint64_t>(sr) * static_cast<uint64_t>(maxBufferedMs)) / 1000ULL);
        if (maxBufFrames < 256) maxBufFrames = 256;
        if (maxBufFrames > (kAudioCapacityFrames / 2)) maxBufFrames = (kAudioCapacityFrames / 2);
    }

    uint32_t maxDropFrames = 0;
    if (maxDropMs > 0) {
        maxDropFrames = static_cast<uint32_t>((static_cast<uint64_t>(sr) * static_cast<uint64_t>(maxDropMs)) / 1000ULL);
        if (maxDropFrames < 64) maxDropFrames = 64;
        if (maxDropFrames > 4096) maxDropFrames = 4096;
    }

    g_audioMaxBufferedFrames.store(maxBufFrames, std::memory_order_relaxed);
    g_audioMaxDropPerCallFrames.store(maxDropFrames, std::memory_order_relaxed);

    // Flush any stale buffered samples to prevent a "burst" after preset changes.
    const uint32_t w = g_audioW.load(std::memory_order_acquire);
    g_audioR.store(w, std::memory_order_release);
}

static std::size_t audioSink(void* /*ctx*/, const int16_t* stereoFrames, std::size_t frameCount) noexcept {
    if (!stereoFrames || frameCount == 0) return frameCount;
    uint32_t w = g_audioW.load(std::memory_order_relaxed);
    uint32_t r = g_audioR.load(std::memory_order_acquire);
    uint32_t used = w - r;
    if (used > kAudioCapacityFrames) used = kAudioCapacityFrames;
    uint32_t freeFrames = kAudioCapacityFrames - used;

    uint32_t frames = static_cast<uint32_t>(frameCount);
    if (frames > freeFrames) {
        // Drop oldest.
        const uint32_t drop = frames - freeFrames;
        g_audioR.store(r + drop, std::memory_order_release);
        g_audioDroppedTotalFrames.fetch_add(drop, std::memory_order_relaxed);
    }

    w = g_audioW.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t idx = (w + i) % kAudioCapacityFrames;
        g_audio[idx * 2 + 0] = stereoFrames[i * 2 + 0];
        g_audio[idx * 2 + 1] = stereoFrames[i * 2 + 1];
    }
    const uint32_t newW = w + frames;
    g_audioW.store(newW, std::memory_order_release);

    // Clamp buffered audio to avoid latency drifting upward over time.
    const uint32_t maxBuffered = g_audioMaxBufferedFrames.load(std::memory_order_relaxed);
    const uint32_t maxDropPerCall = g_audioMaxDropPerCallFrames.load(std::memory_order_relaxed);
    if (maxBuffered > 0) {
        const uint32_t curR = g_audioR.load(std::memory_order_acquire);
        uint32_t usedNow = newW - curR;
        if (usedNow > kAudioCapacityFrames) usedNow = kAudioCapacityFrames;
        if (usedNow > maxBuffered) {
            uint32_t drop = usedNow - maxBuffered;
            // If far beyond the target, hard-drop to the target immediately.
            // This avoids a long period of tiny drops (audible repeated glitches).
            const uint32_t hardDropThreshold = maxBuffered * 2;
            if (usedNow <= hardDropThreshold && maxDropPerCall > 0 && drop > maxDropPerCall) {
                drop = maxDropPerCall;
            }
            g_audioR.store(curR + drop, std::memory_order_release);
            g_audioDroppedTotalFrames.fetch_add(drop, std::memory_order_relaxed);
        }
    }
    return frameCount;
}

inline uint16_t androidKeyToSnesBit(int keyCode) noexcept {
    // Common Android keycodes (values match Android KeyEvent constants).
    // We intentionally avoid including android/keycodes.h here.
    switch (keyCode) {
        case 96:  return snesonline::SNES_A;      // KEYCODE_BUTTON_A
        case 97:  return snesonline::SNES_B;      // KEYCODE_BUTTON_B
        case 99:  return snesonline::SNES_X;      // KEYCODE_BUTTON_X
        case 100: return snesonline::SNES_Y;      // KEYCODE_BUTTON_Y
        case 102: return snesonline::SNES_L;      // KEYCODE_BUTTON_L1
        case 103: return snesonline::SNES_R;      // KEYCODE_BUTTON_R1
        case 104: return snesonline::SNES_L;      // KEYCODE_BUTTON_L2
        case 105: return snesonline::SNES_R;      // KEYCODE_BUTTON_R2
        case 108: return snesonline::SNES_START;  // KEYCODE_BUTTON_START
        case 109: return snesonline::SNES_SELECT; // KEYCODE_BUTTON_SELECT

        // --- BlueStacks / emulator-friendly keyboard fallbacks ---
        // D-pad via WASD
        case 51: return snesonline::SNES_UP;      // KEYCODE_W
        case 47: return snesonline::SNES_DOWN;    // KEYCODE_S
        case 29: return snesonline::SNES_LEFT;    // KEYCODE_A
        case 32: return snesonline::SNES_RIGHT;   // KEYCODE_D

        // D-pad via IJKL
        case 37: return snesonline::SNES_UP;      // KEYCODE_I
        case 38: return snesonline::SNES_DOWN;    // KEYCODE_J
        case 39: return snesonline::SNES_LEFT;    // KEYCODE_K
        case 40: return snesonline::SNES_RIGHT;   // KEYCODE_L

        // Face buttons via ZXCV
        case 54: return snesonline::SNES_B;       // KEYCODE_Z
        case 52: return snesonline::SNES_A;       // KEYCODE_X
        case 31: return snesonline::SNES_Y;       // KEYCODE_C
        case 50: return snesonline::SNES_X;       // KEYCODE_V

        // Face buttons via UIOP-ish (common alt bindings)
        case 48: return snesonline::SNES_Y;       // KEYCODE_U
        case 34: return snesonline::SNES_A;       // KEYCODE_G
        case 36: return snesonline::SNES_B;       // KEYCODE_H

        // Start / Select
        case 66: return snesonline::SNES_START;   // KEYCODE_ENTER
        case 62: return snesonline::SNES_START;   // KEYCODE_SPACE
        case 67: return snesonline::SNES_SELECT;  // KEYCODE_DEL (Backspace)
        case 111: return snesonline::SNES_SELECT; // KEYCODE_ESCAPE

        // L / R
        case 45: return snesonline::SNES_L;       // KEYCODE_Q
        case 33: return snesonline::SNES_R;       // KEYCODE_E
        case 59: return snesonline::SNES_L;       // KEYCODE_SHIFT_LEFT
        case 60: return snesonline::SNES_R;       // KEYCODE_SHIFT_RIGHT

        case 19: return snesonline::SNES_UP;      // KEYCODE_DPAD_UP
        case 20: return snesonline::SNES_DOWN;    // KEYCODE_DPAD_DOWN
        case 21: return snesonline::SNES_LEFT;    // KEYCODE_DPAD_LEFT
        case 22: return snesonline::SNES_RIGHT;   // KEYCODE_DPAD_RIGHT
        default: return 0;
    }
}

inline void setBit(uint16_t bit, bool down) noexcept {
    if (bit == 0) return;
    uint16_t cur = g_inputMask.load(std::memory_order_relaxed);
    if (down) cur |= bit;
    else cur &= static_cast<uint16_t>(~bit);
    g_inputMask.store(cur, std::memory_order_relaxed);
}

void loop60fps() {
    using clock = std::chrono::steady_clock;
    constexpr auto frame = std::chrono::microseconds(16667);

    // Best-effort: prioritize emulation/audio production.
    // (May be ignored by the OS depending on device/vendor restrictions.)
    setpriority(PRIO_PROCESS, 0, -10);

    auto next = clock::now();
    while (g_running.load(std::memory_order_relaxed)) {
        const uint16_t localMask = g_inputMask.load(std::memory_order_relaxed);
        const bool paused = g_paused.load(std::memory_order_relaxed);

        // Fixed-timestep loop with bounded catch-up.
        // If this is too small and we ever miss our schedule (GC, I/O, CPU contention),
        // we will under-produce audio and Java will start zero-padding.
        constexpr int kMaxCatchUpFrames = 60;
        int steps = 0;
        auto now = clock::now();
        while (now >= next && steps < kMaxCatchUpFrames) {
            next += frame;

            // Persist in-game saves (SRAM) periodically (independent of savestates).
            maybeFlushSaveRam_(false);

            if (g_netplayEnabled.load(std::memory_order_relaxed) && g_netplay) {
                g_netplay->pumpRecv();
                // When paused we intentionally avoid sending new input frames (so the peer stalls too),
                // but we still send keepalives so the connection stays up.
                if (paused) {
                    g_netplay->sendKeepAlive();
                } else {
                    g_netplay->sendLocal(localMask);
                }

                g_netplay->pumpStateSyncSend();
                g_netplay->pumpSaveRamSyncSend();

                // If host detected a desync (or received a resync request), start a state sync.
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
                    // Keep loop alive for networking/state sync, but don't advance frames.
                    g_netplayStatus.store(3, std::memory_order_relaxed);
                    break;
                }

                uint16_t localForFrame = 0;
                uint16_t remoteForFrame = 0;
                if (!g_netplay->tryGetInputsForCurrentFrame(localForFrame, remoteForFrame)) {
                    g_netplayStatus.store(2, std::memory_order_relaxed);
                    // Can't advance this frame yet; try again on the next iteration.
                    break;
                }

                g_netplayStatus.store(3, std::memory_order_relaxed);

                const bool localIsP1 = (g_netplay->localPlayerNum == 1);
                snesonline::EmulatorEngine::instance().setInputMask(0, localIsP1 ? localForFrame : remoteForFrame);
                snesonline::EmulatorEngine::instance().setInputMask(1, localIsP1 ? remoteForFrame : localForFrame);
                snesonline::EmulatorEngine::instance().advanceFrame();
                g_netplay->frame++;

                // Record + periodically exchange a light-weight checksum for desync detection.
                const uint32_t completedFrame = g_netplay->frame - 1;
                g_netplay->recordLocalHashForCompletedFrame_(completedFrame);
                g_netplay->maybeSendLocalHashForCompletedFrame_(completedFrame);
            } else {
                g_netplayStatus.store(0, std::memory_order_relaxed);
                if (paused) {
                    break;
                }
                snesonline::EmulatorEngine::instance().setLocalInputMask(localMask);
                snesonline::EmulatorEngine::instance().advanceFrame();
            }

            steps++;
            now = clock::now();

            if (paused) {
                // Don't try to catch up accumulated time while paused.
                break;
            }
        }

        // Do NOT resync `next` forward here.
        // Resyncing effectively *skips emulated frames*, which directly causes audio underflows.
        // If we're behind, we want to keep advancing frames until we catch up.

        if (steps > 0) {
            g_loopFramesTotal.fetch_add(static_cast<uint64_t>(steps), std::memory_order_relaxed);
            if (steps > 1) {
                g_loopCatchupEventsTotal.fetch_add(1, std::memory_order_relaxed);
                g_loopCatchupFramesTotal.fetch_add(static_cast<uint64_t>(steps - 1), std::memory_order_relaxed);
            }
        }

        std::this_thread::sleep_until(next);
    }
}

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_snesonline_NativeBridge_nativeInitialize(JNIEnv* env, jclass /*cls*/, jstring corePath, jstring romPath, jstring statePath, jstring savePath,
                                                 jboolean enableNetplay, jstring remoteHost,
                                                 jint remotePort, jint localPort, jint localPlayerNum,
                                                 jstring roomServerUrl, jstring roomCode, jstring sharedSecret) {
    if (!corePath || !romPath) return JNI_FALSE;

    const char* core = env->GetStringUTFChars(corePath, nullptr);
    const char* rom = env->GetStringUTFChars(romPath, nullptr);

    const char* state = nullptr;
    if (statePath) {
        state = env->GetStringUTFChars(statePath, nullptr);
    }

    const char* save = nullptr;
    if (savePath) {
        save = env->GetStringUTFChars(savePath, nullptr);
    }

    const char* remote = nullptr;
    if (remoteHost) {
        remote = env->GetStringUTFChars(remoteHost, nullptr);
    }

    const char* roomUrl = nullptr;
    if (roomServerUrl) {
        roomUrl = env->GetStringUTFChars(roomServerUrl, nullptr);
    }
    const char* room = nullptr;
    if (roomCode) {
        room = env->GetStringUTFChars(roomCode, nullptr);
    }

    const char* secret = nullptr;
    if (sharedSecret) {
        secret = env->GetStringUTFChars(sharedSecret, nullptr);
    }

    auto& eng = snesonline::EmulatorEngine::instance();
    const bool ok = eng.initialize(core, rom);
    bool netplayOk = true;
    if (ok) {
        eng.core().setVideoSink(nullptr, &videoSink);
        eng.core().setAudioSink(nullptr, &audioSink);
        configureAudioLatencyCaps(eng.core().sampleRateHz());

        const bool wantNetplay = (enableNetplay == JNI_TRUE);
        const uint8_t pnum = static_cast<uint8_t>((localPlayerNum == 2) ? 2 : 1);

        // Natural saves (SRAM):
        // - Offline: auto-load last per-ROM save (default path).
        // - Netplay: host does NOT auto-load; must be explicitly selected via savePath.
        // - Netplay joiner never loads local SRAM (may receive host sync).
        const std::string defaultSavePath = makeSaveRamPathFromRom_(rom);
        g_saveRamPath = (save && save[0]) ? std::string(save) : defaultSavePath;
        g_saveRamLoadedFromFile = false;
        g_saveRamLoadedExplicitly = false;

        if (!wantNetplay) {
            (void)loadSaveRamFromFile_(g_saveRamPath);
        } else if (pnum == 1) {
            if (save && save[0]) {
                (void)loadSaveRamFromFile_(g_saveRamPath);
                g_saveRamLoadedExplicitly = g_saveRamLoadedFromFile;
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

        g_netplayEnabled.store(false, std::memory_order_relaxed);
        g_netplay.reset();

        // Best-effort: load a state before starting.
        // Netplay rule: only Player 1 (host) state is considered.
        if (state && state[0]) {
            const bool wantNetplay = (enableNetplay == JNI_TRUE);
            const uint8_t pnum = static_cast<uint8_t>((localPlayerNum == 2) ? 2 : 1);
            const bool canUse = (!wantNetplay) || (pnum == 1);
            if (canUse) {
                std::vector<uint8_t> bytes;
                if (readFile_(state, bytes)) {
                    (void)loadStateBytes_(bytes.data(), bytes.size());
                }
            }
        }

        if (wantNetplay) {
            const uint16_t rp = static_cast<uint16_t>((remotePort >= 1 && remotePort <= 65535) ? remotePort : 7000);
            const uint16_t lp = static_cast<uint16_t>((localPort >= 1 && localPort <= 65535) ? localPort : 7000);

            auto np = std::make_unique<UdpNetplay>();
            const char* host = (remote && remote[0]) ? remote : "";
            if (np->start(host, rp, lp, pnum, roomUrl ? roomUrl : "", room ? room : "", secret ? secret : "")) {
                // Host: stage the same state for the peer to load.
                if (pnum == 1 && state && state[0]) {
                    std::vector<uint8_t> bytes;
                    if (readFile_(state, bytes)) {
                        np->configureStateSyncHost(std::move(bytes));
                    }
                }

                // Host: if SRAM was explicitly loaded, require peer to receive it before starting.
                if (pnum == 1 && g_saveRamLoadedExplicitly) {
                    std::vector<uint8_t> bytes;
                    if (getSaveRamBytes_(bytes)) {
                        (void)np->queueSaveRamSync(std::move(bytes), true);
                    }
                }

                g_netplay = std::move(np);
                g_netplayEnabled.store(true, std::memory_order_relaxed);
            } else {
                // Do not silently fall back to offline play if the user asked for netplay.
                netplayOk = false;
            }
        }
    }

    env->ReleaseStringUTFChars(corePath, core);
    env->ReleaseStringUTFChars(romPath, rom);

    if (statePath && state) {
        env->ReleaseStringUTFChars(statePath, state);
    }

    if (savePath && save) {
        env->ReleaseStringUTFChars(savePath, save);
    }

    if (remoteHost && remote) {
        env->ReleaseStringUTFChars(remoteHost, remote);
    }

    if (roomServerUrl && roomUrl) {
        env->ReleaseStringUTFChars(roomServerUrl, roomUrl);
    }
    if (roomCode && room) {
        env->ReleaseStringUTFChars(roomCode, room);
    }

    if (sharedSecret && secret) {
        env->ReleaseStringUTFChars(sharedSecret, secret);
    }

    return (ok && netplayOk) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_snesonline_NativeBridge_nativeSaveStateToFile(JNIEnv* env, jclass /*cls*/, jstring statePath) {
    if (!statePath) return JNI_FALSE;
    const char* path = env->GetStringUTFChars(statePath, nullptr);
    if (!path || !path[0]) {
        if (path) env->ReleaseStringUTFChars(statePath, path);
        return JNI_FALSE;
    }

    snesonline::SaveState st;
    const bool ok = snesonline::EmulatorEngine::instance().saveState(st);
    bool wrote = false;
    if (ok && st.sizeBytes > 0 && st.buffer.data()) {
        wrote = writeFile_(path, st.buffer.data(), st.sizeBytes);
    }

    env->ReleaseStringUTFChars(statePath, path);
    return wrote ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_snesonline_NativeBridge_nativeLoadStateFromFile(JNIEnv* env, jclass /*cls*/, jstring statePath) {
    if (!statePath) return JNI_FALSE;
    const char* path = env->GetStringUTFChars(statePath, nullptr);
    if (!path || !path[0]) {
        if (path) env->ReleaseStringUTFChars(statePath, path);
        return JNI_FALSE;
    }

    // Netplay rule: only host can load the canonical state.
    if (g_netplayEnabled.load(std::memory_order_relaxed) && g_netplay && g_netplay->localPlayerNum != 1) {
        env->ReleaseStringUTFChars(statePath, path);
        return JNI_FALSE;
    }

    std::vector<uint8_t> bytes;
    const bool okRead = readFile_(path, bytes);
    bool okLoad = false;
    if (okRead && !bytes.empty()) {
        okLoad = loadStateBytes_(bytes.data(), bytes.size());
        if (okLoad) {
            // If host in netplay, immediately stage and transfer the same state to the joiner.
            if (g_netplayEnabled.load(std::memory_order_relaxed) && g_netplay && g_netplay->localPlayerNum == 1) {
                g_netplay->configureStateSyncHost(std::move(bytes));
            }
        }
    }

    env->ReleaseStringUTFChars(statePath, path);
    return okLoad ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_snesonline_NativeBridge_nativeSetPaused(JNIEnv* /*env*/, jclass /*cls*/, jboolean paused) {
    g_paused.store(paused == JNI_TRUE, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_snesonline_NativeBridge_nativeShutdown(JNIEnv* /*env*/, jclass /*cls*/) {
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

extern "C" JNIEXPORT jint JNICALL
Java_com_snesonline_NativeBridge_nativeGetNetplayStatus(JNIEnv* /*env*/, jclass /*cls*/) {
    return static_cast<jint>(g_netplayStatus.load(std::memory_order_relaxed));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_snesonline_NativeBridge_nativeStunPublicUdpPort(JNIEnv* /*env*/, jclass /*cls*/, jint localPort) {
    const uint16_t lp = static_cast<uint16_t>((localPort >= 1 && localPort <= 65535) ? localPort : 0);
    if (lp == 0) return 0;

    snesonline::StunMappedAddress mapped;
    if (!snesonline::stunDiscoverMappedAddressDefault(lp, mapped)) return 0;
    if (mapped.port < 1 || mapped.port > 65535) return 0;
    return static_cast<jint>(mapped.port);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_snesonline_NativeBridge_nativeStunMappedAddress(JNIEnv* env, jclass /*cls*/, jint localPort) {
    const uint16_t lp = static_cast<uint16_t>((localPort >= 1 && localPort <= 65535) ? localPort : 0);
    if (lp == 0) return env->NewStringUTF("");

    snesonline::StunMappedAddress mapped;
    if (!snesonline::stunDiscoverMappedAddressDefault(lp, mapped)) return env->NewStringUTF("");
    if (mapped.ip.empty() || mapped.port < 1 || mapped.port > 65535) return env->NewStringUTF("");

    std::string s;
    if (mapped.ip.find(':') != std::string::npos) {
        s = "[" + mapped.ip + "]:" + std::to_string(static_cast<unsigned>(mapped.port));
    } else {
        s = mapped.ip + ":" + std::to_string(static_cast<unsigned>(mapped.port));
    }
    return env->NewStringUTF(s.c_str());
}

// Called by Activity/GL thread on MotionEvent.
extern "C" JNIEXPORT void JNICALL
Java_com_snesonline_NativeBridge_nativeOnAxis(JNIEnv* /*env*/, jclass /*cls*/, jfloat axisX, jfloat axisY) {
    uint16_t cur = g_inputMask.load(std::memory_order_relaxed);

    // Clear dpad bits and re-apply based on stick.
    cur &= static_cast<uint16_t>(~(snesonline::SNES_UP | snesonline::SNES_DOWN | snesonline::SNES_LEFT | snesonline::SNES_RIGHT));

    const snesonline::Stick2f stick{axisX, axisY};
    const uint16_t dpad = snesonline::sanitizeDpad(snesonline::mapAndroidAxesToDpad(stick));
    cur |= dpad;

    g_inputMask.store(cur, std::memory_order_relaxed);
}

// action: 0=down, 1=up (match KeyEvent.ACTION_DOWN/UP)
extern "C" JNIEXPORT void JNICALL
Java_com_snesonline_NativeBridge_nativeOnKey(JNIEnv* /*env*/, jclass /*cls*/, jint keyCode, jint action) {
    const uint16_t bit = androidKeyToSnesBit(static_cast<int>(keyCode));
    const bool down = (action == 0);
    setBit(bit, down);
}

extern "C" JNIEXPORT void JNICALL
Java_com_snesonline_NativeBridge_nativeStartLoop(JNIEnv* /*env*/, jclass /*cls*/) {
    if (g_running.exchange(true, std::memory_order_relaxed)) return;
    g_loopThread = std::thread(loop60fps);
}

extern "C" JNIEXPORT void JNICALL
Java_com_snesonline_NativeBridge_nativeStopLoop(JNIEnv* /*env*/, jclass /*cls*/) {
    g_running.store(false, std::memory_order_relaxed);
    if (g_loopThread.joinable()) g_loopThread.join();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_snesonline_NativeBridge_nativeGetVideoWidth(JNIEnv* /*env*/, jclass /*cls*/) {
    return static_cast<jint>(g_videoW.load(std::memory_order_relaxed));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_snesonline_NativeBridge_nativeGetVideoHeight(JNIEnv* /*env*/, jclass /*cls*/) {
    return static_cast<jint>(g_videoH.load(std::memory_order_relaxed));
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_snesonline_NativeBridge_nativeGetVideoBufferRGBA(JNIEnv* env, jclass /*cls*/) {
    // Whole buffer; GameActivity uses width/height.
    return env->NewDirectByteBuffer(g_rgba, static_cast<jlong>(sizeof(g_rgba)));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_snesonline_NativeBridge_nativePopAudio(JNIEnv* env, jclass /*cls*/, jshortArray dstInterleavedStereo, jint framesWanted) {
    if (!dstInterleavedStereo || framesWanted <= 0) return 0;
    const jsize nShorts = env->GetArrayLength(dstInterleavedStereo);
    if (nShorts < framesWanted * 2) return 0;

    jboolean isCopy = JNI_FALSE;
    jshort* out = env->GetShortArrayElements(dstInterleavedStereo, &isCopy);
    if (!out) return 0;

    uint32_t r = g_audioR.load(std::memory_order_relaxed);
    uint32_t w = g_audioW.load(std::memory_order_acquire);
    uint32_t avail = w - r;
    if (avail > kAudioCapacityFrames) avail = kAudioCapacityFrames;
    uint32_t frames = static_cast<uint32_t>(framesWanted);
    if (frames > avail) frames = avail;
    if (static_cast<uint32_t>(framesWanted) > frames) {
        g_audioUnderflowTotalFrames.fetch_add(static_cast<uint64_t>(static_cast<uint32_t>(framesWanted) - frames), std::memory_order_relaxed);
    }

    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t idx = (r + i) % kAudioCapacityFrames;
        out[i * 2 + 0] = g_audio[idx * 2 + 0];
        out[i * 2 + 1] = g_audio[idx * 2 + 1];
    }

    g_audioR.store(r + frames, std::memory_order_release);

    env->ReleaseShortArrayElements(dstInterleavedStereo, out, 0);
    return static_cast<jint>(frames);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_snesonline_NativeBridge_nativeGetAudioSampleRateHz(JNIEnv* /*env*/, jclass /*cls*/) {
    const double hz = snesonline::EmulatorEngine::instance().core().sampleRateHz();
    int out = static_cast<int>(hz + 0.5);
    // IMPORTANT:
    // Some cores don't report a valid sample rate until after the first frame.
    // Returning 0 lets Java wait briefly and then configure AudioTrack correctly,
    // avoiding large systematic underflows from a sample-rate mismatch.
    if (out < 8000 || out > 192000) return 0;
    return static_cast<jint>(out);
}

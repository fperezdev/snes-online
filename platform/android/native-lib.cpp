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

#include "snesonline/EmulatorEngine.h"
#include "snesonline/InputBits.h"
#include "snesonline/InputMapping.h"
#include "snesonline/StunClient.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::atomic<uint16_t> g_inputMask{0};
std::atomic<bool> g_running{false};
std::thread g_loopThread;

struct UdpNetplay {
    int sock = -1;
    sockaddr_in remote{};
    uint16_t localPort = 7000;
    uint16_t remotePort = 7000;
    uint8_t localPlayerNum = 1;
    uint32_t frame = 0;

    bool discoverPeer = false;

    // Tiny receive buffer keyed by frame % N.
    static constexpr uint32_t kBufN = 256;
    // Delay inputs slightly to absorb jitter (both peers must use same value).
    static constexpr uint32_t kInputDelayFrames = 5;
    // Re-send a small window of recent frames to survive UDP loss.
    static constexpr uint32_t kResendWindow = 16;
    std::array<uint16_t, kBufN> remoteMask{};
    std::array<uint32_t, kBufN> remoteFrameTag{};

    std::array<uint16_t, kBufN> sentMask{};
    std::array<uint32_t, kBufN> sentFrameTag{};

    bool hasPeer = false;
    std::chrono::steady_clock::time_point lastRecv{};

    struct Packet {
        uint32_t frame_be;
        uint16_t mask_be;
        uint16_t reserved_be;
    };

    static bool setNonBlocking(int fd) noexcept {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) return false;
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    static bool resolveIpv4(const char* host, sockaddr_in& out, uint16_t port) noexcept {
        if (!host || !host[0]) return false;

        // Fast path: numeric IPv4.
        in_addr addr{};
        if (inet_pton(AF_INET, host, &addr) == 1) {
            out = {};
            out.sin_family = AF_INET;
            out.sin_addr = addr;
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
            const auto* sin = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
            out = *sin;
            out.sin_port = htons(port);
            ok = true;
            break;
        }

        freeaddrinfo(res);
        return ok;
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

        // host:port
        std::string host = s;
        uint16_t port = 0;
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

        sockaddr_in server{};
        if (!resolveIpv4(host.c_str(), server, port)) return false;

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
                sockaddr_in from{};
                socklen_t fromLen = sizeof(from);
                const int n = static_cast<int>(recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
                if (n <= 0) break;
                if (from.sin_addr.s_addr != server.sin_addr.s_addr) continue;
                if (n < 10) continue;
                if (std::memcmp(buf, "SNO_PEER1", 9) != 0) continue;

                char ip[64] = {};
                int p = 0;
                // Ensure NUL-terminated for sscanf.
                buf[(n >= static_cast<int>(sizeof(buf))) ? (static_cast<int>(sizeof(buf)) - 1) : n] = 0;
                if (std::sscanf(buf, "SNO_PEER1 %63s %d", ip, &p) != 2) continue;
                if (p < 1 || p > 65535) continue;

                sockaddr_in peer{};
                if (!resolveIpv4(ip, peer, static_cast<uint16_t>(p))) continue;

                remote = peer;
                remotePort = static_cast<uint16_t>(p);
                discoverPeer = false;
                // Allow sending immediately.
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        return false;
    }

    bool start(const char* remoteHost, uint16_t rPort, uint16_t lPort, uint8_t lPlayer, const char* roomServerUrl, const char* roomCode) noexcept {
        stop();

        localPort = (lPort != 0) ? lPort : 7000;
        remotePort = (rPort != 0) ? rPort : 7000;
        localPlayerNum = (lPlayer == 2) ? 2 : 1;

        remoteFrameTag.fill(0xFFFFFFFFu);
        remoteMask.fill(0);
        sentFrameTag.fill(0xFFFFFFFFu);
        sentMask.fill(0);
        frame = 0;

        // Prime our local input history for the first few frames so the sim can start immediately.
        for (uint32_t f = 0; f < kInputDelayFrames; ++f) {
            const uint32_t idx = f % kBufN;
            sentFrameTag[idx] = f;
            sentMask[idx] = 0;
        }

        hasPeer = false;
        lastRecv = {};

        discoverPeer = false;

        const bool hasRemoteHost = (remoteHost && remoteHost[0]);
        if (!hasRemoteHost) {
            // Host convenience: allow Player 1 to leave remoteHost empty and auto-discover from the first packet.
            if (localPlayerNum != 1) {
                return false;
            }
            discoverPeer = true;
            remote = {};
            remote.sin_family = AF_INET;
            remote.sin_port = 0;
        } else {
            if (!resolveIpv4(remoteHost, remote, remotePort)) {
                return false;
            }
        }

        sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            sock = -1;
            return false;
        }

        // Reuse address helps quick restarts.
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        // Larger buffers reduce drops on jittery Wi-Fi.
        int bufBytes = 1 << 20;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufBytes, sizeof(bufBytes));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufBytes, sizeof(bufBytes));

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(localPort);
        if (bind(sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            stop();
            return false;
        }

        if (!setNonBlocking(sock)) {
            stop();
            return false;
        }

        // Room flow does not use server-assisted peer rendezvous here.

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
            Packet p{};
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            const int n = static_cast<int>(recvfrom(sock, &p, sizeof(p), 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
            if (n < 0) {
                break;
            }
            if (n != static_cast<int>(sizeof(p))) {
                continue;
            }

            // Learn/refresh peer endpoint from observed packets (NATs may rewrite source ports).
            if (discoverPeer) {
                if (!hasPeer) {
                    // Lock onto the first peer we observe.
                    remote = from;
                    remotePort = static_cast<uint16_t>(ntohs(from.sin_port));
                } else if (remote.sin_addr.s_addr == from.sin_addr.s_addr) {
                    remote.sin_port = from.sin_port;
                    remotePort = static_cast<uint16_t>(ntohs(from.sin_port));
                }
            } else {
                // If user configured a host/IP, keep IP pinned but let the port float if NAT changes it.
                if (remote.sin_addr.s_addr == from.sin_addr.s_addr) {
                    remote.sin_port = from.sin_port;
                    remotePort = static_cast<uint16_t>(ntohs(from.sin_port));
                }
            }

            const uint32_t f = ntohl(p.frame_be);
            const uint16_t m = ntohs(p.mask_be);
            const uint32_t idx = f % kBufN;
            remoteFrameTag[idx] = f;
            remoteMask[idx] = m;
            hasPeer = true;
            lastRecv = std::chrono::steady_clock::now();
        }
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
        if (remote.sin_family != AF_INET || remote.sin_port == 0) return;

        const uint32_t start = (targetFrame >= (kResendWindow - 1)) ? (targetFrame - (kResendWindow - 1)) : 0u;
        for (uint32_t f = start; f <= targetFrame; ++f) {
            const uint32_t i = f % kBufN;
            if (sentFrameTag[i] != f) continue;
            Packet p{};
            p.frame_be = htonl(f);
            p.mask_be = htons(sentMask[i]);
            p.reserved_be = 0;
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

        // Fixed-timestep loop with bounded catch-up.
        // If this is too small and we ever miss our schedule (GC, I/O, CPU contention),
        // we will under-produce audio and Java will start zero-padding.
        constexpr int kMaxCatchUpFrames = 60;
        int steps = 0;
        auto now = clock::now();
        while (now >= next && steps < kMaxCatchUpFrames) {
            next += frame;

            if (g_netplayEnabled.load(std::memory_order_relaxed) && g_netplay) {
                g_netplay->pumpRecv();
                g_netplay->sendLocal(localMask);

                if (!g_netplay->hasPeer) {
                    g_netplayStatus.store(1, std::memory_order_relaxed);
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
            } else {
                g_netplayStatus.store(0, std::memory_order_relaxed);
                snesonline::EmulatorEngine::instance().setLocalInputMask(localMask);
                snesonline::EmulatorEngine::instance().advanceFrame();
            }

            steps++;
            now = clock::now();
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
Java_com_snesonline_NativeBridge_nativeInitialize(JNIEnv* env, jclass /*cls*/, jstring corePath, jstring romPath,
                                                 jboolean enableNetplay, jstring remoteHost,
                                                 jint remotePort, jint localPort, jint localPlayerNum,
                                                 jstring roomServerUrl, jstring roomCode) {
    if (!corePath || !romPath) return JNI_FALSE;

    const char* core = env->GetStringUTFChars(corePath, nullptr);
    const char* rom = env->GetStringUTFChars(romPath, nullptr);

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

    auto& eng = snesonline::EmulatorEngine::instance();
    const bool ok = eng.initialize(core, rom);
    bool netplayOk = true;
    if (ok) {
        eng.core().setVideoSink(nullptr, &videoSink);
        eng.core().setAudioSink(nullptr, &audioSink);
        configureAudioLatencyCaps(eng.core().sampleRateHz());

        g_netplayEnabled.store(false, std::memory_order_relaxed);
        g_netplay.reset();

        const bool wantNetplay = (enableNetplay == JNI_TRUE);
        if (wantNetplay) {
            const uint16_t rp = static_cast<uint16_t>((remotePort >= 1 && remotePort <= 65535) ? remotePort : 7000);
            const uint16_t lp = static_cast<uint16_t>((localPort >= 1 && localPort <= 65535) ? localPort : 7000);
            const uint8_t pnum = static_cast<uint8_t>((localPlayerNum == 2) ? 2 : 1);

            auto np = std::make_unique<UdpNetplay>();
            const char* host = (remote && remote[0]) ? remote : "";
            if (np->start(host, rp, lp, pnum, roomUrl ? roomUrl : "", room ? room : "")) {
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

    if (remoteHost && remote) {
        env->ReleaseStringUTFChars(remoteHost, remote);
    }

    if (roomServerUrl && roomUrl) {
        env->ReleaseStringUTFChars(roomServerUrl, roomUrl);
    }
    if (roomCode && room) {
        env->ReleaseStringUTFChars(roomCode, room);
    }

    return (ok && netplayOk) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_snesonline_NativeBridge_nativeShutdown(JNIEnv* /*env*/, jclass /*cls*/) {
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

    const std::string s = mapped.ip + ":" + std::to_string(static_cast<unsigned>(mapped.port));
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

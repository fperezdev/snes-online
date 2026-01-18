#import "SNESOnlineIOSNative.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

#include "snesonline/EmulatorEngine.h"
#include "snesonline/InputBits.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
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

    static constexpr uint32_t kBufN = 256;
    static constexpr uint32_t kInputDelayFrames = 5;
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

    bool start(const char* remoteHost, uint16_t rPort, uint16_t lPort, uint8_t lPlayer) noexcept {
        stop();

        localPort = (lPort != 0) ? lPort : 7000;
        remotePort = (rPort != 0) ? rPort : 7000;
        localPlayerNum = (lPlayer == 2) ? 2 : 1;

        remoteFrameTag.fill(0xFFFFFFFFu);
        remoteMask.fill(0);
        sentFrameTag.fill(0xFFFFFFFFu);
        sentMask.fill(0);
        frame = 0;

        for (uint32_t f = 0; f < kInputDelayFrames; ++f) {
            const uint32_t idx = f % kBufN;
            sentFrameTag[idx] = f;
            sentMask[idx] = 0;
        }

        hasPeer = false;
        lastRecv = {};

        discoverPeer = false;
        if (remoteHost && remoteHost[0]) {
            if (!resolveIpv4(remoteHost, remote, remotePort)) {
                return false;
            }
        } else {
            discoverPeer = (localPlayerNum == 1);
            remote = {};
            remote.sin_family = AF_INET;
            remote.sin_port = htons(remotePort);
            remote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }

        sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) return false;

        int buf = 1 << 20;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(localPort);
        if (bind(sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            stop();
            return false;
        }

        setNonBlocking(sock);
        return true;
    }

    void stop() noexcept {
        if (sock >= 0) {
            close(sock);
            sock = -1;
        }
        hasPeer = false;
    }

    void pumpRecv() noexcept {
        if (sock < 0) return;

        for (;;) {
            Packet p{};
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            const ssize_t n = recvfrom(sock, &p, sizeof(p), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n < 0) break;
            if (static_cast<size_t>(n) < sizeof(p)) continue;

            const uint32_t f = ntohl(p.frame_be);
            const uint16_t m = ntohs(p.mask_be);

            if (!hasPeer) {
                hasPeer = true;
                lastRecv = std::chrono::steady_clock::now();
                if (discoverPeer) {
                    remote = from;
                    remotePort = ntohs(from.sin_port);
                }
            }

            lastRecv = std::chrono::steady_clock::now();

            const uint32_t idx = f % kBufN;
            remoteFrameTag[idx] = f;
            remoteMask[idx] = m;
        }

        if (hasPeer && (std::chrono::steady_clock::now() - lastRecv) > std::chrono::seconds(5)) {
            hasPeer = false;
        }
    }

    void sendLocal(uint16_t localMask) noexcept {
        if (sock < 0) return;

        const uint32_t sendFrame = frame + kInputDelayFrames;
        const uint32_t idx = sendFrame % kBufN;
        sentFrameTag[idx] = sendFrame;
        sentMask[idx] = localMask;

        const uint32_t start = (sendFrame > kResendWindow) ? (sendFrame - kResendWindow) : 0;
        for (uint32_t f = start; f <= sendFrame; ++f) {
            const uint32_t fi = f % kBufN;
            if (sentFrameTag[fi] != f) continue;

            Packet p{};
            p.frame_be = htonl(f);
            p.mask_be = htons(sentMask[fi]);
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

    auto next = clock::now();
    while (g_running.load(std::memory_order_relaxed)) {
        const uint16_t localMask = g_inputMask.load(std::memory_order_relaxed);

        constexpr int kMaxCatchUpFrames = 4;
        int steps = 0;
        auto now = clock::now();
        while (now >= next && steps < kMaxCatchUpFrames) {
            next += frameDur;

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
                    break;
                }

                g_netplayStatus.store(3, std::memory_order_relaxed);

                const uint16_t p1 = (g_netplay->localPlayerNum == 1) ? localForFrame : remoteForFrame;
                const uint16_t p2 = (g_netplay->localPlayerNum == 1) ? remoteForFrame : localForFrame;

                snesonline::EmulatorEngine::instance().setInputMasks(p1, p2);
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

        std::this_thread::sleep_until(next);
    }
}

} // namespace

int snesonline_ios_get_netplay_status(void) {
    return g_netplayStatus.load(std::memory_order_relaxed);
}

bool snesonline_ios_initialize(const char* corePath,
                              const char* romPath,
                              bool enableNetplay,
                              const char* remoteHost,
                              int remotePort,
                              int localPort,
                              int localPlayerNum) {
    g_running.store(false, std::memory_order_relaxed);
    if (g_loopThread.joinable()) g_loopThread.join();

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

    bool netplayOk = true;
    if (enableNetplay) {
        auto np = std::make_unique<UdpNetplay>();
        netplayOk = np->start(remoteHost ? remoteHost : "", (uint16_t)remotePort, (uint16_t)localPort, (uint8_t)localPlayerNum);
        if (netplayOk) {
            g_netplay = std::move(np);
            g_netplayEnabled.store(true, std::memory_order_relaxed);
            g_netplayStatus.store(1, std::memory_order_relaxed);
        }
    }

    return enableNetplay ? netplayOk : true;
}

void snesonline_ios_shutdown(void) {
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

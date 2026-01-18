#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace snesonline {

// Minimal Libretro host by dynamic symbol loading.
// This avoids pulling in libretro headers and keeps the core boundary explicit.
class LibretroCore {
public:
    LibretroCore() noexcept;
    ~LibretroCore() noexcept;

    LibretroCore(const LibretroCore&) = delete;
    LibretroCore& operator=(const LibretroCore&) = delete;

    bool load(const char* corePath) noexcept;
    void unload() noexcept;

    bool loadGame(const char* romPath) noexcept;
    void unloadGame() noexcept;

    void runFrame() noexcept;

    // Optional host sinks (video/audio). If unset, frames/samples are dropped.
    using VideoRefreshFn = void (*)(void* ctx, const void* data, unsigned width, unsigned height, std::size_t pitchBytes) noexcept;
    using AudioSampleBatchFn = std::size_t (*)(void* ctx, const int16_t* stereoFrames, std::size_t frameCount) noexcept;

    void setVideoSink(void* ctx, VideoRefreshFn fn) noexcept;
    void setAudioSink(void* ctx, AudioSampleBatchFn fn) noexcept;

    // Runtime info (populated after core is loaded; AV info after game is loaded).
    enum class PixelFormat : uint8_t {
        XRGB1555 = 0,
        XRGB8888 = 1,
        RGB565 = 2,
    };

    PixelFormat pixelFormat() const noexcept { return pixelFormat_; }
    double framesPerSecond() const noexcept { return fps_; }
    double sampleRateHz() const noexcept { return sampleRateHz_; }

    std::size_t serializeSize() const noexcept;
    bool serialize(void* dst, std::size_t sizeBytes) const noexcept;
    bool unserialize(const void* src, std::size_t sizeBytes) noexcept;

    // Per-frame input feeding (SNES mask per port).
    // Port 0 = Player 1, Port 1 = Player 2.
    void setInputMask(unsigned port, uint16_t mask) noexcept;
    void setInputMasks(uint16_t port0Mask, uint16_t port1Mask) noexcept;

    bool isLoaded() const noexcept { return handle_ != nullptr; }

private:
    void* resolve_(const char* name) noexcept;

    void* handle_ = nullptr;

    // Common libretro entrypoints.
    void (*retro_init_)() = nullptr;
    void (*retro_deinit_)() = nullptr;
    unsigned (*retro_api_version_)() = nullptr;
    void (*retro_run_)() = nullptr;

    size_t (*retro_serialize_size_)() = nullptr;
    bool (*retro_serialize_)(void*, size_t) = nullptr;
    bool (*retro_unserialize_)(const void*, size_t) = nullptr;

    bool (*retro_load_game_)(const void* /*retro_game_info*/ ) = nullptr;
    void (*retro_unload_game_)() = nullptr;

    void (*retro_set_input_poll_)(void (*)(void)) = nullptr;
    void (*retro_set_input_state_)(int16_t (*)(unsigned, unsigned, unsigned, unsigned)) = nullptr;

    bool (*retro_get_system_av_info_)(void* /*retro_system_av_info*/ ) = nullptr;
    void (*retro_set_environment_)(bool (*)(unsigned, void*)) = nullptr;
    void (*retro_set_video_refresh_)(void (*)(const void*, unsigned, unsigned, size_t)) = nullptr;
    void (*retro_set_audio_sample_)(void (*)(int16_t, int16_t)) = nullptr;
    void (*retro_set_audio_sample_batch_)(size_t (*)(const int16_t*, size_t)) = nullptr;

private:
    static void inputPoll_() noexcept;
    static int16_t inputState_(unsigned port, unsigned device, unsigned index, unsigned id) noexcept;

    static bool environment_(unsigned cmd, void* data) noexcept;
    static void videoRefresh_(const void* data, unsigned width, unsigned height, size_t pitch) noexcept;
    static void audioSample_(int16_t left, int16_t right) noexcept;
    static size_t audioSampleBatch_(const int16_t* data, size_t frames) noexcept;

    static std::atomic<uint16_t> inputMasks_[2]; // global for callback simplicity

    static void* videoCtx_;
    static VideoRefreshFn videoFn_;

    static void* audioCtx_;
    static AudioSampleBatchFn audioFn_;

    static std::atomic<int> pixelFormatRaw_; // libretro RETRO_PIXEL_FORMAT_*

    PixelFormat pixelFormat_ = PixelFormat::XRGB8888;
    double fps_ = 60.0;
    double sampleRateHz_ = 48000.0;
};

} // namespace snesonline

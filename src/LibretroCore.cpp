#include "snesonline/LibretroCore.h"

#include "snesonline/InputBits.h"

#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace snesonline {

std::atomic<uint16_t> LibretroCore::inputMasks_[2] = {0, 0};
void* LibretroCore::videoCtx_ = nullptr;
LibretroCore::VideoRefreshFn LibretroCore::videoFn_ = nullptr;
void* LibretroCore::audioCtx_ = nullptr;
LibretroCore::AudioSampleBatchFn LibretroCore::audioFn_ = nullptr;
// Initialize to RETRO_PIXEL_FORMAT_XRGB8888 (1) without depending on constants declared below.
std::atomic<int> LibretroCore::pixelFormatRaw_{1};

// Minimal libretro command/format values used by this host.
static constexpr unsigned RETRO_ENVIRONMENT_SET_PIXEL_FORMAT = 10;
static constexpr unsigned RETRO_ENVIRONMENT_GET_LOG_INTERFACE = 27;
static constexpr unsigned RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY = 9;
static constexpr unsigned RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY = 31;

static constexpr int RETRO_PIXEL_FORMAT_0RGB1555 = 0;
static constexpr int RETRO_PIXEL_FORMAT_XRGB8888 = 1;
static constexpr int RETRO_PIXEL_FORMAT_RGB565 = 2;

struct RetroLogCallback {
    void (*log)(int level, const char* fmt, ...) = nullptr;
};

LibretroCore::LibretroCore() noexcept = default;

LibretroCore::~LibretroCore() noexcept {
    unload();
}

void* LibretroCore::resolve_(const char* name) noexcept {
    if (!handle_) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

bool LibretroCore::load(const char* corePath) noexcept {
    unload();

#if defined(_WIN32)
    handle_ = static_cast<void*>(LoadLibraryA(corePath));
#else
    handle_ = dlopen(corePath, RTLD_NOW);
#endif
    if (!handle_) return false;

    retro_init_ = reinterpret_cast<void (*)()>(resolve_("retro_init"));
    retro_deinit_ = reinterpret_cast<void (*)()>(resolve_("retro_deinit"));
    retro_api_version_ = reinterpret_cast<unsigned (*)()>(resolve_("retro_api_version"));
    retro_run_ = reinterpret_cast<void (*)()>(resolve_("retro_run"));

    retro_serialize_size_ = reinterpret_cast<size_t (*)()>(resolve_("retro_serialize_size"));
    retro_serialize_ = reinterpret_cast<bool (*)(void*, size_t)>(resolve_("retro_serialize"));
    retro_unserialize_ = reinterpret_cast<bool (*)(const void*, size_t)>(resolve_("retro_unserialize"));

    retro_load_game_ = reinterpret_cast<bool (*)(const void*)>(resolve_("retro_load_game"));
    retro_unload_game_ = reinterpret_cast<void (*)()>(resolve_("retro_unload_game"));

    retro_set_input_poll_ = reinterpret_cast<void (*)(void (*)(void))>(resolve_("retro_set_input_poll"));
    retro_set_input_state_ = reinterpret_cast<void (*)(int16_t (*)(unsigned, unsigned, unsigned, unsigned))>(resolve_("retro_set_input_state"));

    retro_get_system_av_info_ = reinterpret_cast<bool (*)(void*)>(resolve_("retro_get_system_av_info"));
    retro_set_environment_ = reinterpret_cast<void (*)(bool (*)(unsigned, void*))>(resolve_("retro_set_environment"));
    retro_set_video_refresh_ = reinterpret_cast<void (*)(void (*)(const void*, unsigned, unsigned, size_t))>(resolve_("retro_set_video_refresh"));
    retro_set_audio_sample_ = reinterpret_cast<void (*)(void (*)(int16_t, int16_t))>(resolve_("retro_set_audio_sample"));
    retro_set_audio_sample_batch_ = reinterpret_cast<void (*)(size_t (*)(const int16_t*, size_t))>(resolve_("retro_set_audio_sample_batch"));

    if (!retro_init_ || !retro_deinit_ || !retro_run_ || !retro_serialize_size_ || !retro_serialize_ || !retro_unserialize_ || !retro_load_game_) {
        unload();
        return false;
    }

    if (retro_set_environment_) retro_set_environment_(&LibretroCore::environment_);
    if (retro_set_video_refresh_) retro_set_video_refresh_(&LibretroCore::videoRefresh_);
    if (retro_set_audio_sample_) retro_set_audio_sample_(&LibretroCore::audioSample_);
    if (retro_set_audio_sample_batch_) retro_set_audio_sample_batch_(&LibretroCore::audioSampleBatch_);
    if (retro_set_input_poll_) retro_set_input_poll_(&LibretroCore::inputPoll_);
    if (retro_set_input_state_) retro_set_input_state_(&LibretroCore::inputState_);

    retro_init_();
    (void)retro_api_version_;

    return true;
}

void LibretroCore::unload() noexcept {
    if (!handle_) return;

    unloadGame();

    if (retro_deinit_) retro_deinit_();

#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif

    handle_ = nullptr;

    retro_init_ = nullptr;
    retro_deinit_ = nullptr;
    retro_api_version_ = nullptr;
    retro_run_ = nullptr;

    retro_serialize_size_ = nullptr;
    retro_serialize_ = nullptr;
    retro_unserialize_ = nullptr;

    retro_load_game_ = nullptr;
    retro_unload_game_ = nullptr;

    retro_set_input_poll_ = nullptr;
    retro_set_input_state_ = nullptr;

    retro_get_system_av_info_ = nullptr;
    retro_set_environment_ = nullptr;
    retro_set_video_refresh_ = nullptr;
    retro_set_audio_sample_ = nullptr;
    retro_set_audio_sample_batch_ = nullptr;

    inputMasks_[0].store(0, std::memory_order_relaxed);
    inputMasks_[1].store(0, std::memory_order_relaxed);

    videoCtx_ = nullptr;
    videoFn_ = nullptr;
    audioCtx_ = nullptr;
    audioFn_ = nullptr;

    pixelFormatRaw_.store(1 /* RETRO_PIXEL_FORMAT_XRGB8888 */, std::memory_order_relaxed);

    pixelFormat_ = PixelFormat::XRGB8888;
    fps_ = 60.0;
    sampleRateHz_ = 48000.0;
}

bool LibretroCore::loadGame(const char* romPath) noexcept {
    if (!handle_ || !retro_load_game_) return false;

    // Minimal retro_game_info layout (path/data/size/meta). We only set path.
    struct RetroGameInfo {
        const char* path;
        const void* data;
        size_t size;
        const char* meta;
    } info;

    std::memset(&info, 0, sizeof(info));
    info.path = romPath;

    const bool ok = retro_load_game_(&info);
    if (!ok) return false;

    // Pull AV info if available to configure host timing/audio.
    // Minimal retro_system_av_info layout.
    struct RetroGameGeometry {
        unsigned base_width;
        unsigned base_height;
        unsigned max_width;
        unsigned max_height;
        float aspect_ratio;
    };
    struct RetroSystemTiming {
        double fps;
        double sample_rate;
    };
    struct RetroSystemAvInfo {
        RetroGameGeometry geometry;
        RetroSystemTiming timing;
    };

    if (retro_get_system_av_info_) {
        RetroSystemAvInfo av{};
        if (retro_get_system_av_info_(&av)) {
            if (av.timing.fps > 1.0) fps_ = av.timing.fps;
            if (av.timing.sample_rate > 1000.0) sampleRateHz_ = av.timing.sample_rate;
        }
    }

    // Map negotiated pixel format into the public enum.
    switch (pixelFormatRaw_.load(std::memory_order_relaxed)) {
        case RETRO_PIXEL_FORMAT_RGB565:
            pixelFormat_ = PixelFormat::RGB565;
            break;
        case RETRO_PIXEL_FORMAT_0RGB1555:
            pixelFormat_ = PixelFormat::XRGB1555;
            break;
        case RETRO_PIXEL_FORMAT_XRGB8888:
        default:
            pixelFormat_ = PixelFormat::XRGB8888;
            break;
    }

    return true;
}

void LibretroCore::unloadGame() noexcept {
    if (retro_unload_game_) {
        retro_unload_game_();
    }
}

void LibretroCore::runFrame() noexcept {
    if (retro_run_) retro_run_();
}

std::size_t LibretroCore::serializeSize() const noexcept {
    if (!retro_serialize_size_) return 0;
    return static_cast<std::size_t>(retro_serialize_size_());
}

bool LibretroCore::serialize(void* dst, std::size_t sizeBytes) const noexcept {
    if (!retro_serialize_) return false;
    return retro_serialize_(dst, static_cast<size_t>(sizeBytes));
}

bool LibretroCore::unserialize(const void* src, std::size_t sizeBytes) noexcept {
    if (!retro_unserialize_) return false;
    return retro_unserialize_(src, static_cast<size_t>(sizeBytes));
}

void LibretroCore::setInputMask(unsigned port, uint16_t mask) noexcept {
    if (port >= 2) return;
    inputMasks_[port].store(mask, std::memory_order_relaxed);
}

void LibretroCore::setInputMasks(uint16_t port0Mask, uint16_t port1Mask) noexcept {
    inputMasks_[0].store(port0Mask, std::memory_order_relaxed);
    inputMasks_[1].store(port1Mask, std::memory_order_relaxed);
}

void LibretroCore::setVideoSink(void* ctx, VideoRefreshFn fn) noexcept {
    videoCtx_ = ctx;
    videoFn_ = fn;
}

void LibretroCore::setAudioSink(void* ctx, AudioSampleBatchFn fn) noexcept {
    audioCtx_ = ctx;
    audioFn_ = fn;
}

void LibretroCore::inputPoll_() noexcept {
    // No-op: inputMask_ is already set by the host.
}

int16_t LibretroCore::inputState_(unsigned port, unsigned device, unsigned index, unsigned id) noexcept {
    (void)index;

    if (port >= 2) return 0;

    // Device/id values match libretro: device=RETRO_DEVICE_JOYPAD, id=RETRO_DEVICE_ID_JOYPAD_*
    // We map a subset (common SNES ids). If the core asks for unknown ids, return 0.
    // Known ordering in libretro joypad: B,Y,SELECT,START,UP,DOWN,LEFT,RIGHT,A,X,L,R
    if (device != 1 /* RETRO_DEVICE_JOYPAD */) return 0;

    const uint16_t inputMask = inputMasks_[port].load(std::memory_order_relaxed);

    switch (id) {
        case 0: return (inputMask & SNES_B) ? 1 : 0;
        case 1: return (inputMask & SNES_Y) ? 1 : 0;
        case 2: return (inputMask & SNES_SELECT) ? 1 : 0;
        case 3: return (inputMask & SNES_START) ? 1 : 0;
        case 4: return (inputMask & SNES_UP) ? 1 : 0;
        case 5: return (inputMask & SNES_DOWN) ? 1 : 0;
        case 6: return (inputMask & SNES_LEFT) ? 1 : 0;
        case 7: return (inputMask & SNES_RIGHT) ? 1 : 0;
        case 8: return (inputMask & SNES_A) ? 1 : 0;
        case 9: return (inputMask & SNES_X) ? 1 : 0;
        case 10: return (inputMask & SNES_L) ? 1 : 0;
        case 11: return (inputMask & SNES_R) ? 1 : 0;
        default: return 0;
    }
}

bool LibretroCore::environment_(unsigned cmd, void* data) noexcept {
    // Keep this minimal; many cores require pixel format to be accepted.
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            if (!data) return false;
            const int fmt = *static_cast<const int*>(data);
            switch (fmt) {
                case RETRO_PIXEL_FORMAT_0RGB1555:
                    pixelFormatRaw_.store(RETRO_PIXEL_FORMAT_0RGB1555, std::memory_order_relaxed);
                    return true;
                case RETRO_PIXEL_FORMAT_XRGB8888:
                    pixelFormatRaw_.store(RETRO_PIXEL_FORMAT_XRGB8888, std::memory_order_relaxed);
                    return true;
                case RETRO_PIXEL_FORMAT_RGB565:
                    pixelFormatRaw_.store(RETRO_PIXEL_FORMAT_RGB565, std::memory_order_relaxed);
                    return true;
                default:
                    return false;
            }
        }

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            // Optional; returning false is acceptable.
            if (!data) return false;
            auto* cb = static_cast<RetroLogCallback*>(data);
            cb->log = nullptr;
            return false;
        }

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            if (!data) return false;
            // Provide a safe default; many cores accept this.
            *static_cast<const char**>(data) = ".";
            return true;
        }

        default:
            return false;
    }
}

void LibretroCore::videoRefresh_(const void* data, unsigned width, unsigned height, size_t pitch) noexcept {
    if (!videoFn_) return;
    videoFn_(videoCtx_, data, width, height, static_cast<std::size_t>(pitch));
}

void LibretroCore::audioSample_(int16_t left, int16_t right) noexcept {
    if (!audioFn_) return;
    const int16_t stereo[2] = {left, right};
    audioFn_(audioCtx_, stereo, 1);
}

size_t LibretroCore::audioSampleBatch_(const int16_t* data, size_t frames) noexcept {
    if (!audioFn_) return frames;
    return static_cast<size_t>(audioFn_(audioCtx_, data, static_cast<std::size_t>(frames)));
}

} // namespace snesonline

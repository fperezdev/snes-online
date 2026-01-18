#include "snesonline/EmulatorEngine.h"

#include <cstdint>
#include <cstring>

namespace snesonline {

EmulatorEngine& EmulatorEngine::instance() noexcept {
    static EmulatorEngine inst;
    return inst;
}

EmulatorEngine::EmulatorEngine() noexcept = default;
EmulatorEngine::~EmulatorEngine() noexcept { shutdown(); }

bool EmulatorEngine::initialize(const char* corePath, const char* romPath) noexcept {
    if (!core_.load(corePath)) return false;
    if (!core_.loadGame(romPath)) return false;
    return true;
}

void EmulatorEngine::shutdown() noexcept {
    core_.unload();
}

void EmulatorEngine::setLocalInputMask(uint16_t mask) noexcept {
    inputMasks_[0].store(mask, std::memory_order_relaxed);
}

void EmulatorEngine::setRemoteInputMask(uint16_t mask) noexcept {
    inputMasks_[1].store(mask, std::memory_order_relaxed);
}

void EmulatorEngine::setInputMask(unsigned port, uint16_t mask) noexcept {
    if (port >= 2) return;
    inputMasks_[port].store(mask, std::memory_order_relaxed);
}

void EmulatorEngine::advanceFrame() noexcept {
    // No allocations, no std::string in hot path.
    core_.setInputMasks(
        inputMasks_[0].load(std::memory_order_relaxed),
        inputMasks_[1].load(std::memory_order_relaxed));
    core_.runFrame();
}

uint32_t EmulatorEngine::checksum32_(const void* data, std::size_t sizeBytes) noexcept {
    // Fast-ish FNV-1a 32-bit checksum. Deterministic and cheap.
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < sizeBytes; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

bool EmulatorEngine::saveState(SaveState& out) noexcept {
    const std::size_t sz = core_.serializeSize();
    if (sz == 0) return false;

    if (out.buffer.size() < sz) {
        if (!out.buffer.allocate(sz, 64)) return false;
    }

    if (!core_.serialize(out.buffer.data(), sz)) return false;

    out.sizeBytes = sz;
    out.checksum = checksum32_(out.buffer.data(), out.sizeBytes);
    return true;
}

bool EmulatorEngine::loadState(const SaveState& in) noexcept {
    if (in.sizeBytes == 0 || !in.buffer.data()) return false;
    return core_.unserialize(in.buffer.data(), in.sizeBytes);
}

} // namespace snesonline

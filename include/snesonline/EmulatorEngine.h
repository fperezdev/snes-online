#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snesonline/AlignedBuffer.h"
#include "snesonline/LibretroCore.h"

namespace snesonline {

// Save-state blob suitable for GGPO snapshotting.
struct SaveState {
    AlignedBuffer buffer;
    std::size_t sizeBytes = 0;
    uint32_t checksum = 0;
};

class EmulatorEngine {
public:
    static EmulatorEngine& instance() noexcept;

    EmulatorEngine(const EmulatorEngine&) = delete;
    EmulatorEngine& operator=(const EmulatorEngine&) = delete;

    // Initialization (call once from platform layer).
    bool initialize(const char* corePath, const char* romPath) noexcept;
    void shutdown() noexcept;

    // Called by platform input code.
    void setLocalInputMask(uint16_t mask) noexcept;
    void setRemoteInputMask(uint16_t mask) noexcept;
    void setInputMask(unsigned port, uint16_t mask) noexcept;

    uint16_t localInputMask() const noexcept { return inputMasks_[0].load(std::memory_order_relaxed); }
    uint16_t remoteInputMask() const noexcept { return inputMasks_[1].load(std::memory_order_relaxed); }

    // Core logic required by GGPO callbacks.
    void advanceFrame() noexcept;
    bool saveState(SaveState& out) noexcept;
    bool loadState(const SaveState& in) noexcept;

    // Exposed for netplay integration.
    LibretroCore& core() noexcept { return core_; }

private:
    EmulatorEngine() noexcept;
    ~EmulatorEngine() noexcept;

    uint32_t checksum32_(const void* data, std::size_t sizeBytes) noexcept;

private:
    LibretroCore core_;
    std::atomic<uint16_t> inputMasks_[2] = {0, 0};
};

} // namespace snesonline

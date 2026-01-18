#pragma once

#include <cstddef>
#include <cstdint>

namespace snesonline {

// Simple aligned buffer for fast snapshotting (serialize blobs).
// Uses 64B alignment by default for cache-line friendly access.
class AlignedBuffer {
public:
    AlignedBuffer() noexcept;
    ~AlignedBuffer() noexcept;

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    AlignedBuffer(AlignedBuffer&& other) noexcept;
    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept;

    bool allocate(std::size_t sizeBytes, std::size_t alignment = 64) noexcept;
    void reset() noexcept;

    void* data() noexcept { return data_; }
    const void* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    void* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t alignment_ = 0;
};

} // namespace snesonline

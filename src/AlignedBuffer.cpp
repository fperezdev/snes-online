#include "snesonline/AlignedBuffer.h"

#include <cstdlib>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace snesonline {

AlignedBuffer::AlignedBuffer() noexcept = default;

AlignedBuffer::~AlignedBuffer() noexcept {
    reset();
}

AlignedBuffer::AlignedBuffer(AlignedBuffer&& other) noexcept {
    data_ = other.data_;
    size_ = other.size_;
    alignment_ = other.alignment_;

    other.data_ = nullptr;
    other.size_ = 0;
    other.alignment_ = 0;
}

AlignedBuffer& AlignedBuffer::operator=(AlignedBuffer&& other) noexcept {
    if (this == &other) return *this;
    reset();

    data_ = other.data_;
    size_ = other.size_;
    alignment_ = other.alignment_;

    other.data_ = nullptr;
    other.size_ = 0;
    other.alignment_ = 0;

    return *this;
}

bool AlignedBuffer::allocate(std::size_t sizeBytes, std::size_t alignment) noexcept {
    reset();

    if (sizeBytes == 0) return true;

#if defined(_WIN32)
    void* ptr = _aligned_malloc(sizeBytes, alignment);
    if (!ptr) return false;
    data_ = ptr;
    size_ = sizeBytes;
    alignment_ = alignment;
    return true;
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, sizeBytes) != 0) {
        return false;
    }
    data_ = ptr;
    size_ = sizeBytes;
    alignment_ = alignment;
    return true;
#endif
}

void AlignedBuffer::reset() noexcept {
    if (!data_) return;

#if defined(_WIN32)
    _aligned_free(data_);
#else
    std::free(data_);
#endif

    data_ = nullptr;
    size_ = 0;
    alignment_ = 0;
}

} // namespace snesonline

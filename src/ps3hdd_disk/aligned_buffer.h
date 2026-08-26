#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>

#if defined(_WIN32)
#  include <malloc.h>
#endif

namespace ps3hdd::disk {

class aligned_buffer {
public:
    aligned_buffer() = default;

    aligned_buffer(std::size_t size, std::size_t alignment) { reset(size, alignment); }

    aligned_buffer(const aligned_buffer&) = delete;
    aligned_buffer& operator=(const aligned_buffer&) = delete;

    aligned_buffer(aligned_buffer&& o) noexcept
        : data_(o.data_), size_(o.size_) { o.data_ = nullptr; o.size_ = 0; }
    aligned_buffer& operator=(aligned_buffer&& o) noexcept {
        if (this != &o) { free_(); data_ = o.data_; size_ = o.size_; o.data_ = nullptr; o.size_ = 0; }
        return *this;
    }

    ~aligned_buffer() { free_(); }

    void reset(std::size_t size, std::size_t alignment) {
        if (data_ && size_ >= size) return;
        free_();
        const std::size_t rounded = (size + alignment - 1) & ~(alignment - 1);
#if defined(_WIN32)
        data_ = static_cast<std::byte*>(_aligned_malloc(rounded, alignment));
#else
        data_ = static_cast<std::byte*>(std::aligned_alloc(alignment, rounded));
#endif
        if (!data_) throw std::bad_alloc();
        size_ = rounded;
    }

    std::byte* data() noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    std::span<std::byte> span(std::size_t n) noexcept { return {data_, n}; }

private:
    void free_() noexcept {
        if (!data_) return;
#if defined(_WIN32)
        _aligned_free(data_);
#else
        std::free(data_);
#endif
        data_ = nullptr;
        size_ = 0;
    }

    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace ps3hdd::disk
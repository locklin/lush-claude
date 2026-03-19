#ifndef CONTEXT_BUFFER_H
#define CONTEXT_BUFFER_H

#include <cstddef>
#include <cstring>
#include <vector>

namespace vmm {

// Fixed-capacity circular buffer for tracking context symbols.
// Oldest element is at index 0, newest at index size()-1.
template<typename T>
class ContextBuffer {
public:
    explicit ContextBuffer(size_t capacity)
        : buf_(capacity), cap_(capacity), size_(0), head_(0) {}

    void push(T val) {
        buf_[head_] = val;
        head_ = (head_ + 1) % cap_;
        if (size_ < cap_) ++size_;
    }

    void clear() { size_ = 0; head_ = 0; }

    size_t size() const { return size_; }
    size_t capacity() const { return cap_; }

    // i=0 is oldest, i=size()-1 is newest
    T operator[](size_t i) const {
        size_t start = (head_ + cap_ - size_) % cap_;
        return buf_[(start + i) % cap_];
    }

    // Copy the last n elements (most recent) into out[0..n-1]
    // where out[0] is the oldest of the n, out[n-1] is the newest.
    void get_last(T* out, size_t n) const {
        if (n > size_) n = size_;
        size_t start = (head_ + cap_ - n) % cap_;
        for (size_t i = 0; i < n; ++i)
            out[i] = buf_[(start + i) % cap_];
    }

    // Get pointer to internal array and offset info for direct access.
    // Returns the underlying buffer; use operator[] for safe access.
    const T* data() const { return buf_.data(); }

private:
    std::vector<T> buf_;
    size_t cap_;
    size_t size_;
    size_t head_; // next write position
};

} // namespace vmm

#endif // CONTEXT_BUFFER_H

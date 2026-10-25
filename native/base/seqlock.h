#pragma once

// Single-writer seqlock for trivially copyable models (§3: game thread publishes, render thread reads).
// The writer copies a finished staging value in (seq odd while copying); a reader copies out and retries
// while it raced the writer. Neither side blocks; a reader that keeps racing keeps its previous copy.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace elmp {

template <class T>
class Seqlock {
    static_assert(std::is_trivially_copyable_v<T>);

public:
    void store(const T& v) {
        const auto s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&value_, &v, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }

    // false: nothing published yet, or the writer kept racing (out unchanged in meaning: keep the old copy)
    bool load(T& out, int attempts = 16) const {
        for (int i = 0; i < attempts; ++i) {
            const auto s0 = seq_.load(std::memory_order_acquire);
            if (s0 == 0) return false;
            if (s0 & 1u) continue;
            std::memcpy(&scratch_, &value_, sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s0) {
                std::memcpy(&out, &scratch_, sizeof(T));
                return true;
            }
        }
        return false;
    }

    std::uint32_t version() const { return seq_.load(std::memory_order_acquire); }

private:
    std::atomic<std::uint32_t> seq_{0};
    T value_{};
    mutable T scratch_{}; // reader-side (single reader: the render thread)
};

// Bounded single-producer / single-consumer ring of trivially copyable items.
template <class T, std::size_t N>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T> && (N & (N - 1)) == 0);

public:
    bool push(const T& v) {
        const auto h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= N) return false;
        items_[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = items_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

private:
    std::atomic<std::size_t> head_{0}, tail_{0};
    T items_[N]{};
};

} // namespace elmp

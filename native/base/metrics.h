#pragma once

// Counters and timing histograms (NFR-5, §0.3 budgets). Callers keep the returned references; lookup by
// name happens once. Counters are atomic; histograms are written by one thread (the game thread).

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace elmp::metrics {

struct Summary {
    std::uint64_t n = 0;
    double mean = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
};

class Histogram {
public:
    explicit Histogram(std::size_t capacity = 8192) : ring_(capacity) {}
    void add(float v) noexcept {
        ring_[static_cast<std::size_t>(total_ % ring_.size())] = v;
        ++total_;
    }
    Summary summary() const; // over the retained window (last `capacity` samples)
    std::uint64_t total() const noexcept { return total_; }
    void clear() noexcept { total_ = 0; }

private:
    std::vector<float> ring_;
    std::uint64_t total_ = 0;
};

using Counter = std::atomic<std::uint64_t>;

Counter& counter(std::string_view name);
Histogram& histogram(std::string_view name, std::size_t capacity = 8192);

struct Snapshot {
    std::vector<std::pair<std::string, std::uint64_t>> counters;
    std::vector<std::pair<std::string, Summary>> histograms;
};
Snapshot snapshot(std::string_view prefix = {});
void reset_histograms(std::string_view prefix = {});

Summary summarize(std::vector<float> samples);

} // namespace elmp::metrics

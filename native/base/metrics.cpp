#include "base/metrics.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>

namespace elmp::metrics {

namespace {

struct Store {
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<Counter>, std::less<>> counters;
    std::map<std::string, std::unique_ptr<Histogram>, std::less<>> histograms;
};

Store& store() {
    static Store s;
    return s;
}

double pct(const std::vector<float>& sorted, double p) {
    if (sorted.empty()) return 0;
    const auto idx = static_cast<std::size_t>(std::min<double>(static_cast<double>(sorted.size() - 1), p * static_cast<double>(sorted.size() - 1) + 0.5));
    return sorted[idx];
}

} // namespace

Summary summarize(std::vector<float> v) {
    Summary s;
    s.n = v.size();
    if (v.empty()) return s;
    std::ranges::sort(v);
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    s.p50 = pct(v, 0.50);
    s.p95 = pct(v, 0.95);
    s.p99 = pct(v, 0.99);
    s.max = v.back();
    return s;
}

Summary Histogram::summary() const {
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(total_, ring_.size()));
    return summarize(std::vector<float>(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(n)));
}

Counter& counter(std::string_view name) {
    auto& s = store();
    std::scoped_lock lock(s.mutex);
    auto it = s.counters.find(name);
    if (it == s.counters.end()) it = s.counters.emplace(std::string(name), std::make_unique<Counter>(0)).first;
    return *it->second;
}

Histogram& histogram(std::string_view name, std::size_t capacity) {
    auto& s = store();
    std::scoped_lock lock(s.mutex);
    auto it = s.histograms.find(name);
    if (it == s.histograms.end()) it = s.histograms.emplace(std::string(name), std::make_unique<Histogram>(capacity)).first;
    return *it->second;
}

Snapshot snapshot(std::string_view prefix) {
    auto& s = store();
    std::scoped_lock lock(s.mutex);
    Snapshot out;
    for (const auto& [k, c] : s.counters)
        if (k.starts_with(prefix)) out.counters.emplace_back(k, c->load());
    for (const auto& [k, h] : s.histograms)
        if (k.starts_with(prefix)) out.histograms.emplace_back(k, h->summary());
    return out;
}

void reset_histograms(std::string_view prefix) {
    auto& s = store();
    std::scoped_lock lock(s.mutex);
    for (auto& [k, h] : s.histograms)
        if (k.starts_with(prefix)) h->clear();
}

} // namespace elmp::metrics

#include "engine/binary_checks.h"

#include "engine/reflect.h"

#include "base/log.h"
#include "base/preflight.h"

#include <format>

namespace elmp::binary_checks {

namespace {

std::vector<Status> g_results;
bool g_ran = false;

Status check_offset(const binary::Offset& o) {
    Status s{.name = o.name, .value = o.value};
    const auto& c = o.check;
    const auto size = o.size > 0 ? o.size : 0;
    switch (c.kind) {
    case binary::OffsetCheck::Kind::NativeOffset: {
        // The parser matched the value to the instruction displacement; preflight matched the entire
        // containing prefix to this EXE. No native layout is inferred from unrelated reflected fields.
        s.ok = preflight::address(c.function) != nullptr;
        s.detail = std::format("verified {} prefix displacement byte {} width {}", c.function, c.byte, c.width);
        return s;
    }
    case binary::OffsetCheck::Kind::Equals: {
        auto* p = reflect::find_property(reflect::find_class(log::wide(c.cls)), log::wide(c.property));
        if (!p) return s.detail = "reflected " + c.cls + "." + c.property + " not found", s;
        s.ok = p->GetOffset_Internal() == o.value && (!size || p->GetSize() == size);
        s.detail = std::format("reflected {} at {} (size {})", c.property, p->GetOffset_Internal(), p->GetSize());
        return s;
    }
    case binary::OffsetCheck::Kind::Within: {
        auto* cls = reflect::find_class(log::wide(c.cls));
        if (!cls) return s.detail = c.cls + " not found", s;
        auto* after = reflect::find_property(cls, log::wide(c.after));
        if (!after) return s.detail = "reflected " + c.after + " not found", s;
        const std::int64_t lo = after->GetOffset_Internal() + after->GetSize();
        std::int64_t hi;
        if (c.before == "<end>") {
            hi = reflect::properties_size(cls);
        } else {
            auto* before = reflect::find_property(cls, log::wide(c.before));
            if (!before) return s.detail = "reflected " + c.before + " not found", s;
            hi = before->GetOffset_Internal();
        }
        s.ok = o.value >= lo && o.value + std::max<std::int64_t>(size, 1) <= hi;
        s.detail = std::format("gap [{}, {}) after {}", lo, hi, c.after);
        return s;
    }
    case binary::OffsetCheck::Kind::StructSize: {
        auto* st = static_cast<ue::UStruct*>(reflect::find_object(log::wide(c.cls)));
        if (!st) return s.detail = c.cls + " not found", s;
        const auto n = reflect::properties_size(st);
        s.ok = n == o.value;
        s.detail = std::format("struct size {}", n);
        return s;
    }
    }
    return s;
}

} // namespace

void run() {
    if (g_ran) return;
    g_ran = true;
    g_results.clear();
    for (const auto& [_, o] : preflight::current().manifest.offsets) {
        auto s = check_offset(o);
        if (!s.ok) log::error("binary offset {}={} failed its check: {}", s.name, s.value, s.detail);
        g_results.push_back(std::move(s));
    }
}

bool ran() { return g_ran; }
const std::vector<Status>& results() { return g_results; }

std::optional<std::int64_t> offset(std::string_view name) {
    for (const auto& s : g_results)
        if (s.name == name) return s.ok ? std::optional<std::int64_t>(s.value) : std::nullopt;
    return std::nullopt;
}

} // namespace elmp::binary_checks

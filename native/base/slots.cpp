#include "base/slots.h"

#include <format>

namespace elmp::slots {

const char* describe(Result r) noexcept {
    switch (r) {
    case Result::Ok: return "ok";
    case Result::ReadOnly: return "slot 1 is P1 (read-only)";
    case Result::OutOfRange: return "slot outside the caller's range";
    case Result::HeldByOther: return "slot is leased by another owner";
    case Result::NotDeclared: return "caller declared no slot range";
    }
    return "?";
}

std::string Table::declare(const Owner& owner, std::int64_t first, std::int64_t last) {
    if (owner.kind != OwnerKind::ThirdParty) return "only third-party mods declare ranges";
    if (first > last) return "empty range";
    if (first <= kTestLast) return std::format("range must start above {}", kTestLast);
    std::scoped_lock lock(mutex_);
    for (const auto& r : ranges_) {
        if (r.owner == owner.name) continue;
        if (first <= r.last && r.first <= last) return std::format("range {}-{} overlaps '{}' ({}-{})", first, last, r.owner, r.first, r.last);
    }
    std::erase_if(ranges_, [&](const Range& r) { return r.owner == owner.name; });
    ranges_.push_back({first, last, owner.name});
    return {};
}

Result Table::allowed(const Owner& owner, std::int64_t slot) const {
    if (slot == kP1) return Result::ReadOnly;
    if (slot < kP1) return Result::OutOfRange;
    switch (owner.kind) {
    case OwnerKind::Elmp:
        if (slot <= kInternalLast) return Result::Ok;
        return (dev_ && slot <= kTestLast) ? Result::Ok : Result::OutOfRange;
    case OwnerKind::DevTool:
        return (dev_ && slot >= kTestFirst && slot <= kTestLast) ? Result::Ok : Result::OutOfRange;
    case OwnerKind::ThirdParty: {
        bool declared = false;
        for (const auto& r : ranges_) {
            if (r.owner != owner.name) continue;
            declared = true;
            if (slot >= r.first && slot <= r.last) return Result::Ok;
        }
        return declared ? Result::OutOfRange : Result::NotDeclared;
    }
    }
    return Result::OutOfRange;
}

Result Table::acquire(const Owner& owner, std::int64_t slot) {
    std::scoped_lock lock(mutex_);
    if (const auto r = allowed(owner, slot); r != Result::Ok) return r;
    auto [it, inserted] = leases_.try_emplace(slot, owner.name);
    return (inserted || it->second == owner.name) ? Result::Ok : Result::HeldByOther;
}

bool Table::release(const Owner& owner, std::int64_t slot) {
    std::scoped_lock lock(mutex_);
    auto it = leases_.find(slot);
    if (it == leases_.end() || it->second != owner.name) return false;
    leases_.erase(it);
    return true;
}

void Table::release_all(const std::string& owner) {
    std::scoped_lock lock(mutex_);
    std::erase_if(leases_, [&](const auto& kv) { return kv.second == owner; });
}

std::optional<std::string> Table::owner_of(std::int64_t slot) const {
    std::scoped_lock lock(mutex_);
    auto it = leases_.find(slot);
    return it == leases_.end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::vector<std::pair<std::int64_t, std::string>> Table::leases() const {
    std::scoped_lock lock(mutex_);
    return {leases_.begin(), leases_.end()};
}

} // namespace elmp::slots

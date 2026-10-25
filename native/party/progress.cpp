#include "party/progress.h"

#include "core/player.h"

#include <set>

namespace elmp::party::progress {

namespace {
std::set<std::int64_t> g_joined;
}

std::optional<core::profile::Profile> shared_profile(std::string* err) {
    auto p = core::profile::read(core::controller(1), err);
    if (!p) return std::nullopt;
    p->spirits.reset();
    p->summon_set.reset();
    if (p->passives) p->passives->equipped.reset();
    return p;
}

void joined(std::int64_t slot) { g_joined.insert(slot); }
void forget(std::int64_t slot) { g_joined.erase(slot); }
void forget_all() { g_joined.clear(); }

} // namespace elmp::party::progress

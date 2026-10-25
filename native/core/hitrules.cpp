#include "core/hitrules.h"

#include "core/player.h"

#include "engine/lifecycle.h"

#include <format>
#include <map>

namespace elmp::core::hitrules {

namespace {

std::map<std::int64_t, Team> g_teams;
Rule g_rule;
std::uint64_t g_generation = 1;
bool g_opposed = false;

void changed() {
    g_opposed = false;
    for (const auto& [slot, t] : g_teams) g_opposed |= t == Team::Enemy; // P1 is friendly
    ++g_generation;
}

} // namespace

const char* team_name(Team t) noexcept { return t == Team::Enemy ? "enemy" : "friendly"; }

std::string bind(const slots::Owner& who, std::int64_t slot, Team team) {
    if (slot == slots::kP1) return "P1 is always friendly";
    const auto owner = leases().owner_of(slot);
    if (!owner || *owner != who.name) return std::format("slot {} is not leased by {}", slot, who.name);
    if (auto it = g_teams.find(slot); it != g_teams.end() && it->second == team) return {};
    g_teams[slot] = team;
    changed();
    return {};
}

void unbind(std::int64_t slot) {
    if (g_teams.erase(slot)) changed();
}

void clear() {
    if (g_teams.empty()) return;
    g_teams.clear();
    changed();
}

std::vector<Binding> bindings() {
    std::vector<Binding> out{{slots::kP1, Team::Friendly}};
    for (const auto& [slot, t] : g_teams) out.push_back({slot, t});
    return out;
}

bool opposed() { return g_opposed; }
std::uint64_t generation() { return g_generation; }
Rule rule() { return g_rule; }

void set_rule(const Rule& r) {
    if (r.damage == g_rule.damage && r.knockback == g_rule.knockback) return;
    g_rule = r;
    ++g_generation;
}

void install() { lifecycle::on_teardown(clear); }

} // namespace elmp::core::hitrules

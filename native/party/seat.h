#pragma once

// Local seats (C2, F1.1): seat = ELMP slot 2..100 + input source + team. Join order (C2): spawn behind P1
// → baseline = shared progress mirrored from P1 + the seat's own equipment → full restore → only then the
// seat enters the input router (devices) and gets a free pad if it has none.
// Leave: unequip + remove the pawn (controller stays pooled), release the lease, drop from the router.
// World teardown: seats are dropped with the world. Automatic re-join per save is tied to the save
// identity (C12) and arrives with save_scope in P1e; until then nothing is carried into another world
// (fail closed: a party never leaks from save A into save B).

#include "core/player.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace elmp::party {

enum class Source : std::uint8_t { Devices, Script };
enum class Team : std::uint8_t { Friendly, Enemy };
const char* source_name(Source s) noexcept;
const char* team_name(Team t) noexcept;

struct Seat {
    std::int64_t slot = 0;
    Source source = Source::Devices;
    Team team = Team::Friendly;
};

struct JoinOptions {
    std::int64_t slot = 0; // 0 = first free 2..100
    Source source = Source::Devices;
    Team team = Team::Friendly;
    int pad = -1; // assign this XInput pad on join
};

std::string join(const JoinOptions& opt, std::int64_t* slot_out);
std::string leave(std::int64_t slot);
std::string set_team(std::int64_t slot, Team team); // pushed to core/hitrules (P1 is always friendly)
const Seat* seat(std::int64_t slot);
std::vector<Seat> seats();
std::size_t count();

std::optional<core::Vec3> behind_p1(int n); // placement shared by join / respawn / recall / transit (v1 70 UU × n, cycle 4)

void install();

} // namespace elmp::party

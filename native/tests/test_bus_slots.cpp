// Event bus cursor / loss accounting and slot lease ranges (FR-SLOT-1).

#include "base/events.h"
#include "base/slots.h"
#include "check.h"

#include <vector>

using namespace elmp;

namespace {

void bus_cursor_and_loss() {
    events::Bus bus;
    std::vector<events::Event> out(16);
    std::uint64_t cursor = 0, lost = 0;
    CHECK(bus.since(cursor, out, &lost) == 0 && lost == 0);
    for (int i = 0; i < 5; ++i) bus.push(events::Type::Damaged, 2, i);
    CHECK(bus.since(cursor, out, &lost) == 5 && lost == 0 && cursor == 5 && out[4].a == 4);
    CHECK(bus.since(cursor, out, &lost) == 0);
    // Wrap far past the reader: oldest retained is reported as loss, reading resumes at the oldest.
    for (int i = 0; i < 1100; ++i) bus.push(events::Type::Death, 3, i);
    std::uint64_t total = 0, n;
    std::uint64_t lost_first = 0;
    bool first = true;
    while ((n = bus.since(cursor, out, &lost)) > 0) {
        if (first) lost_first = lost, first = false;
        total += n;
    }
    CHECK(lost_first == 1100 - events::Bus::kCapacity);
    CHECK(total == events::Bus::kCapacity && cursor == bus.last_seq());
    // Partial reads keep order.
    std::uint64_t c2 = bus.last_seq() - 3;
    std::vector<events::Event> two(2);
    CHECK(bus.since(c2, two) == 2 && two[0].seq + 1 == two[1].seq && c2 == bus.last_seq() - 1);
}

void lease_ranges() {
    slots::Table dev(true), ship(false);
    const slots::Owner elmp{"ELMP", slots::OwnerKind::Elmp}, tool{"UEBridge", slots::OwnerKind::DevTool};
    const slots::Owner follower{"LilyFollower", slots::OwnerKind::ThirdParty}, enemy{"LilyEnemy", slots::OwnerKind::ThirdParty};
    CHECK(dev.acquire(elmp, 1) == slots::Result::ReadOnly);
    CHECK(dev.acquire(elmp, 2) == slots::Result::Ok && dev.acquire(elmp, 2) == slots::Result::Ok);
    CHECK(dev.acquire(tool, 2) == slots::Result::OutOfRange);
    CHECK(dev.acquire(tool, 101) == slots::Result::Ok && ship.acquire(tool, 101) == slots::Result::OutOfRange);
    CHECK(ship.acquire(elmp, 101) == slots::Result::OutOfRange);
    CHECK(dev.acquire(follower, 1001) == slots::Result::NotDeclared);
    CHECK(dev.declare(follower, 1001, 1999).empty());
    CHECK(!dev.declare(enemy, 1500, 2500).empty()); // overlap → later loader refused
    CHECK(!dev.declare(enemy, 900, 999).empty());   // below third-party space
    CHECK(dev.declare(enemy, 2001, 2999).empty());
    CHECK(dev.acquire(follower, 1001) == slots::Result::Ok && dev.acquire(enemy, 1001) == slots::Result::OutOfRange);
    CHECK(dev.acquire(follower, 2001) == slots::Result::OutOfRange);
    CHECK(!dev.release(enemy, 1001) && dev.release(follower, 1001));
    dev.acquire(follower, 1002);
    dev.release_all("LilyFollower");
    CHECK(!dev.owner_of(1002) && dev.owner_of(2) == "ELMP");
}

} // namespace

TEST_MAIN({
    bus_cursor_and_loss();
    lease_ranges();
})

// Adapters enemy and lily. Their halves live with the role state they share:
// enemy — host: spawn-point enemies keyed by the SourceSpawnPoint name (position / facing / HP / dead flag /
// Spine pose); client: takeover recipe (freeze, AI gate, marks), buffered interpolated position, facing,
// HP property write, pose with track-time alignment, death = presentation (hide after the row goes).
// lily — host: P1 + every seat; client: own seat → local P1 prediction with snap-back, others → pooled
// copies (marked, IsPlayer false), HUD vitals from the host seat.

#include "coop/client.h"
#include "coop/entity.h"
#include "coop/host.h"

namespace elmp::coop::entities {

namespace {

class Enemy final : public IEntityAdapter {
public:
    const char* name() const override { return "enemy"; }
    void write_host(HostCtx& c) override { host::write_enemies(c); }
    void apply_client(const ClientCtx& c) override { client::present_enemies(c); }
};

class Lily final : public IEntityAdapter {
public:
    const char* name() const override { return "lily"; }
    void write_host(HostCtx& c) override { host::write_lilies(c); }
    void apply_client(const ClientCtx& c) override { client::present_lily_rows(c); }
};

// spirit (P6) — host: visible (summoned) spirits whose Owner is a seat's controller (P1 or a seat), keyed
// seat << 8 | index, class / position / facing / Spine pose; client: presentation spirit actors (spawned per
// key and class, frozen like enemies, marked, no collision) for the other seats; hidden when the row goes.
class Spirit final : public IEntityAdapter {
public:
    const char* name() const override { return "spirit"; }
    void write_host(HostCtx& c) override { host::write_spirits(c); }
    void apply_client(const ClientCtx& c) override { client::present_spirits(c); }
    void reset() override { host::reset_spirits(), client::reset_spirits(); }
};

} // namespace

IEntityAdapter& spirit() {
    static Spirit a;
    return a;
}

IEntityAdapter& enemy() {
    static Enemy a;
    return a;
}

IEntityAdapter& lily() {
    static Lily a;
    return a;
}

} // namespace elmp::coop::entities

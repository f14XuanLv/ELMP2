// Adapters enemy and lily. Their halves live with the role state they share:
// enemy — host: spawn-point enemies keyed by the SourceSpawnPoint name (position / facing / HP / dead flag /
// Spine pose); client (ADR 0006): takeover stops the decision source only (AI gate, marks), the vanilla
// execution layer runs the host's commands; interpolated position + velocity, facing, HP property write,
// death = presentation (frozen corpse with the host pose, hidden after the row goes).
// lily — host: P1 + every seat (+ live input); client: own seat → local P1 prediction with snap-back,
// others → pooled copies on the vanilla pipeline (host input via virtual pad, host commands), HUD vitals.

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

} // namespace

IEntityAdapter& enemy() {
    static Enemy a;
    return a;
}

IEntityAdapter& lily() {
    static Lily a;
    return a;
}

} // namespace elmp::coop::entities

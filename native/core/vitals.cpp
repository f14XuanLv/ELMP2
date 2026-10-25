#include "core/vitals.h"

#include "core/player.h"
#include "core/schema.h"

#include "engine/reflect.h"

namespace elmp::core::vitals {

namespace {

int call_int(const reflect::FuncRef& f, ue::UObject* self) {
    reflect::Frame fr(f);
    return fr.invoke(self) ? fr.ret<std::int32_t>() : 0;
}

} // namespace

Values read(std::int64_t slot) {
    Values v;
    const auto* s = schema::script();
    auto* pc = controller(slot);
    if (!s || !pc) return v;
    if (auto* p = pawn(slot)) {
        if (auto* hp = s->zc_hp.object(p)) {
            v.valid = true;
            v.hp = s->hp_curr.read<std::int32_t>(hp);
            v.max_hp = s->hp_max.read<std::int32_t>(hp);
            reflect::Frame alive(s->hp_alive);
            v.alive = alive.invoke(hp) && alive.ret_bool();
        }
        if (auto* mp = s->zc_mp.object(p)) {
            v.mp = s->mp_curr.read<std::int32_t>(mp);
            v.max_mp = s->mp_max.read<std::int32_t>(mp);
        }
    }
    if (auto* heal = s->zpc_heal.object(pc)) {
        v.heals = call_int(s->heal_remaining, heal);
        v.max_heals = call_int(s->heal_max_count, heal);
    }
    return v;
}

std::string restore(std::int64_t slot) {
    const auto* s = schema::script();
    if (!s) return "core schema unresolved";
    auto* pc = controller(slot);
    if (slot <= 1 || !is_elmp_controller(pc)) return "refusing to modify a controller not created by ELMP (P1 is read-only)";
    auto* p = pawn(slot);
    if (!p) return "player has no pawn";
    if (!read(slot).alive) return "player is dead; respawn instead";
    reflect::Frame fr(s->zpc_fully_restore);
    fr.arg(0, p);
    if (!fr.invoke(pc)) return "FullyRestore failed";
    if (auto* mp = s->zc_mp.object(p)) {
        reflect::Frame unlocked(s->zpc_aptitude_unlocked);
        unlocked.arg(0, kAptitudeSpecialAttack);
        if (unlocked.invoke(pc) && unlocked.ret_bool()) {
            const auto missing = s->mp_max.read<std::int32_t>(mp) - s->mp_curr.read<std::int32_t>(mp);
            if (missing > 0) reflect::Frame(s->mp_add).arg(0, missing).invoke(mp);
        } else {
            reflect::Frame(s->mp_reset).invoke(mp);
        }
    }
    return {};
}

} // namespace elmp::core::vitals

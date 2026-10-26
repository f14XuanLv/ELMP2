#include "coop/entities/clearable.h"

#include "core/schema.h"

#include "engine/reflect.h"

#include <format>

namespace elmp::coop::entities {

namespace {

ue::UObject* find_local(const std::vector<registry::Kind>& kinds, const std::string& name) {
    for (auto kind : kinds)
        for (const auto& e : registry::bucket(kind))
            if (auto* o = e.handle.get(); o && ue::utf8_name(o) == name) return o;
    return nullptr;
}

} // namespace

void hide_actor(ue::UObject* a) {
    static reflect::FuncRef hide = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"SetActorHiddenInGame", 1);
    static reflect::FuncRef coll = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"SetActorEnableCollision", 1);
    if (hide.ok()) {
        reflect::Frame h(hide);
        h.arg_bool(0, true);
        h.invoke(a);
    }
    if (coll.ok()) {
        reflect::Frame c(coll);
        c.arg_bool(0, false);
        c.invoke(a);
    }
}

void LevelObjects::write_host(HostCtx& c) {
    if (c.now_us >= next_sample_us_) {
        next_sample_us_ = c.now_us + 500'000;
        const auto* s = core::schema::script();
        static ObjHandle clear_cls = ObjHandle::of(reflect::find_object(L"/Script/Zenith.ClearableComponent"));
        static reflect::FuncRef is_cleared =
            reflect::Resolver("ClearableComponent", reflect::find_class(L"/Script/Zenith.ClearableComponent")).func(L"IsCleared", 0);
        if (s)
            for (auto kind : kinds_)
                for (const auto& e : registry::bucket(kind)) {
                    auto* a = e.handle.get();
                    if (!a) continue;
                    auto& w = host_[ue::utf8_name(a)];
                    if (w.actor != e.handle) {
                        w.actor = e.handle;
                        reflect::Frame f(s->actor_component_by_class);
                        f.arg(0, clear_cls.get());
                        w.clearable = f.invoke(a) ? ObjHandle::of(f.ret<ue::UObject*>()) : ObjHandle{};
                    }
                    w.flags &= ~2;
                    if (auto* cc = w.clearable.get(); cc && is_cleared.ok()) {
                        reflect::Frame f(is_cleared);
                        if (f.invoke(cc)) w.flags = f.ret_bool() ? (w.flags | 1) : (w.flags & ~1);
                    }
                    w.flags |= host_extra(kind, a);
                }
        for (auto& [n, w] : host_)
            if (!w.actor.get()) w.flags |= 2; // destroyed on the host (item consumed, breakable removed)
    }
    for (const auto& [n, w] : host_)
        if (w.flags)
            if (auto* row = c.frame.w.add(c.name(n))) row->flags = w.flags;
}

void LevelObjects::apply_client(const ClientCtx& c) {
    for (const auto& row : c.frame.w.view()) {
        const auto* name = c.text(row.key);
        if (!name) continue;
        auto it = done_.find(*name);
        const std::uint8_t done = it == done_.end() ? 0 : it->second;
        if ((done & row.flags) == row.flags) continue;
        auto* a = find_local(kinds_, *name);
        if (!a) continue; // not ours (another adapter's kind) or not streamed in locally yet
        done_[*name] = done | present(a, row.flags, done);
        ++presented_;
    }
}

void LevelObjects::reset() {
    host_.clear();
    done_.clear();
    next_sample_us_ = 0;
}

std::string LevelObjects::describe() const {
    std::size_t flagged = 0;
    for (const auto& [n, w] : host_) flagged += w.flags != 0;
    return std::format("{}: hostRows={} presented={}\n", name(), flagged, presented_);
}

} // namespace elmp::coop::entities

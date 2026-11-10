// Typed codec boundaries (§13 CTest class 1): writer overflow latches, reader never reads past the end,
// varint limits, string caps, truncation of every prefix of a valid message fails cleanly.
#include "net/codec.h"
#include "net/protocol.h"
#include "net/snapshot.h"

#include <memory>

#include "tests/check.h"

#include <array>
#include <vector>
#include <limits>

using namespace elmp::net;

TEST_MAIN({
    { // Autonomous command frames: bounded nested context, command edges, and original turn definition.
        using namespace elmp::net::proto;
        SpiritFrame a,b;auto& c=a.context;c.seat=2;c.map=3;c.epoch=4;c.view=5;c.generation=6;c.seq=7;c.facing=1;c.dt=c.step=c.world_dt=1.f/60;
        a.role=8;a.episode=9;a.cls=10;a.summon=11;a.edges.push_back({12,13,0,8,9,0});a.edges.push_back({12,13,1,0,0,6});
        a.layer=3;a.animation={14,15,1,.2f,0,0,.1f};
        a.has_follow=true;a.follow_position={1,-200,500};a.follow_rotation={0,90,0};a.follow_velocity={0,-300,20};a.decision_only=true;c.dt=0;
        a.follow_scale={1,2,3};a.follow_history={1,-200,3,.5f,1,2,0,-300,20,0,-301,19};a.soft_ending=true;
        a.actor_input={1,0,1,0,0,1,0};a.movement_input={0,2,0};a.actor_direction=4;a.actor_flags=a.actor_raw=1<<3;a.forced_walk=2;
        std::array<std::uint8_t,1024> wire{};auto n=encode(a,wire);CHECK(n&&decode(std::span(wire.data(),n),b)&&a==b);
        for(std::size_t cut=0;cut<n;++cut)CHECK(!decode(std::span(wire.data(),cut),b));
        CHECK(!decode(std::span(wire.data(),n+1),b));CHECK(!encode(a,std::span(wire.data(),n-1)));
        auto bad=a;bad.edges.resize(17);CHECK(!encode(bad,wire));bad=a;bad.edges[0].sequence=0;CHECK(!encode(bad,wire));
        bad=a;bad.actor_scale=std::numeric_limits<float>::quiet_NaN();CHECK(!encode(bad,wire));bad=a;bad.animation.time=std::numeric_limits<float>::infinity();CHECK(!encode(bad,wire));
        bad=a;bad.follow_history[4]=std::numeric_limits<float>::quiet_NaN();CHECK(!encode(bad,wire));
        bad=a;bad.episode=0;CHECK(!encode(bad,wire));bad=a;bad.edges[0].reason=7;CHECK(!encode(bad,wire));
        bad=a;bad.actor_input[0]=std::numeric_limits<float>::quiet_NaN();CHECK(!encode(bad,wire));
        bad=a;bad.movement_input[1]=std::numeric_limits<float>::infinity();CHECK(!encode(bad,wire));
        bad=a;bad.actor_raw=1;CHECK(!encode(bad,wire));bad=a;bad.actor_direction=9;CHECK(!encode(bad,wire));bad=a;bad.forced_walk=4;CHECK(!encode(bad,wire));
        n=encode(a,wire);wire[n-1]=4;CHECK(!decode(std::span(wire.data(),n),b));
        n=encode(a,wire);Writer input_patch(std::span(wire).subspan(n-34));input_patch.f32(std::numeric_limits<float>::infinity());CHECK(!decode(std::span(wire.data(),n),b));
    }
    { // Reliable original input history: bounded states, complete frames, no NaN or trailing bytes.
        using namespace elmp::net::proto;
        LilyFrame a, b;
        a.seat = 1; a.map = 2; a.seq = 1; a.generation = 10; a.dt = a.step = 1.0f / 60;
        a.epoch=2;a.view=1;a.t_ms=100;a.world_dt=a.dt;a.facing=2;
        a.persistent_class=7;a.persistent_token=900;a.persistent_input=1;a.persistent_direction=3;
        a.actor_clock_count=kLilyClocks;for(unsigned i=0;i<kLilyClocks;++i)a.actor_clocks[i]={static_cast<std::uint16_t>(i+1),0.1f};
        a.mode = 1; a.state_count = a.command_state_count = static_cast<std::uint8_t>(kLilyStates);
        a.flags = a.raw = 0x3FE; a.input[0] = -1; a.velocity[1] = 300;
        for (std::size_t i = 0; i < kLilyStates; ++i) a.states[i] = a.command_states[i] = static_cast<std::uint16_t>(i + 1);
        a.commands.resize(kLilyCommands, LilyCommand{27, 1, 3, 2, 1, 0});
        for (auto& edge : a.commands) edge.climb_target = std::array<float, 3>{0, -1600, 400};
        a.commands.back().end = 3;
        a.commands.back().climb_target.reset();
        std::array<std::uint8_t, kLilyFrameBytes + 1> wire{};
        a.commands.push_back(a.commands.front()); CHECK(encode(a, wire) == 0); a.commands.pop_back();
        const auto n = encode(a, wire);
        CHECK(n > 0 && decode(std::span(wire.data(), n), b) && a == b);
        for (std::size_t cut = 0; cut < n; ++cut) CHECK(!decode(std::span(wire.data(), cut), b));
        CHECK(!decode(std::span(wire.data(), n + 1), b));
        a.input[0] = std::numeric_limits<float>::quiet_NaN(); CHECK(encode(a, wire) == 0);
        a.input[0] = 0; a.seq = 0; CHECK(encode(a, wire) == 0);
        a.seq = 1; a.step = 0; CHECK(encode(a, wire) == 0);
        a.step = std::numeric_limits<float>::infinity(); CHECK(encode(a, wire) == 0);
        a.step = 1.0f / 60; a.dt = 0; // a genuine zero-delta HitStop is distinct from no received frame
        CHECK(encode(a, wire) > 0);
        a.step = 2; a.dt = 0.4f; // a streaming hitch is legitimate, unlike a non-finite/corrupt clock
        const auto long_frame = encode(a, wire);
        CHECK(long_frame > 0 && decode(std::span(wire.data(), long_frame), b) && a == b);
        a.step = 60.01f; CHECK(encode(a, wire) == 0);
        a.step = 1.0f / 60;
        a.actor_clock_count=kLilyClocks+1;CHECK(encode(a,wire)==0);a.actor_clock_count=kLilyClocks;
        a.actor_clocks[0].scale=std::numeric_limits<float>::quiet_NaN();CHECK(encode(a,wire)==0);a.actor_clocks[0].scale=0.1f;
        a.facing=3;CHECK(encode(a,wire)==0);a.facing=2;
        a.persistent_direction=9;CHECK(encode(a,wire)==0);a.persistent_direction=3;
        a.persistent_class=0;CHECK(encode(a,wire)==0);a.persistent_class=7;
        a.command_state_count = 49; CHECK(encode(a, wire) == 0);
        a.command_state_count = 48; a.command_states[0]=0; CHECK(encode(a, wire) == 0);
        a.command_states[0]=1; a.state_count = 49; CHECK(encode(a, wire) == 0);
    }
    {
        using namespace elmp::net::proto;
        LilyCauses a,b;a.epoch=2;a.map=3;a.view=4;a.fence_ms=100;
        Cause root;root.seat=1;root.map=3;root.generation=10;root.input=99;root.root=42;root.ordinal=1;root.cls=5;root.invoker=6;root.t_ms=90;
        a.facts.push_back(root);
        a.facts[0].domain=1;a.facts[0].role=37;a.facts[0].actor_rotation={0,-90,0};
        auto q=root;q.kind=CauseKind::Query;q.cls=7;q.query=1;q.path.depth=1;q.path.entries[0]=0;q.path.occurrences[0]=1;q.ref_kind=1;q.ref_id=8;q.ref_generation=123;q.component=9;q.range=2000;q.flags=3;q.channel=3;
        a.facts.push_back(q);q.kind=CauseKind::Target;a.facts.push_back(q);q.kind=CauseKind::End;q.reason=2;a.facts.push_back(q);
        std::vector<std::uint8_t> wire(128*1024);const auto n=encode(a,wire);
        CHECK(n>0&&decode(std::span(wire.data(),n),b)&&a.facts==b.facts&&a.view==b.view&&a.fence_ms==b.fence_ms);
        for(std::size_t cut=0;cut<n;++cut)CHECK(!decode(std::span(wire.data(),cut),b));
        CHECK(!decode(std::span(wire.data(),n+1),b));
        auto bad=a;bad.facts[1].path.occurrences[0]=0;CHECK(encode(bad,wire)==0);
        bad=a;bad.facts[1].range=std::numeric_limits<float>::quiet_NaN();CHECK(encode(bad,wire)==0);
        bad=a;bad.facts[1].ref_generation=0;CHECK(encode(bad,wire)==0);
        bad=a;bad.facts[2].component=0;CHECK(encode(bad,wire)==0);
        bad=a;bad.facts[0].root=0;CHECK(encode(bad,wire)==0);
        bad=a;bad.facts[0].ordinal=0;CHECK(encode(bad,wire)==0);
        bad=a;bad.facts[1].actor_scale=std::numeric_limits<float>::infinity();CHECK(encode(bad,wire)==0);
        a.facts.assign(kCauses,root);CHECK(encode(a,wire)>0);a.facts.push_back(root);CHECK(encode(a,wire)==0);
    }
    std::array<std::uint8_t, 64> buf{};
    Writer w(buf.data(), buf.size());
    w.u8(7);
    w.u16(0xBEEF);
    w.u32(0xDEADBEEF);
    w.u64(0x0123456789ABCDEFull);
    w.f32(1.5f);
    w.var(0);
    w.var(300);
    w.var(0xFFFFFFFFu);
    w.str("hi");
    CHECK(w.ok());
    const auto full = w.size();
    {
        Reader r(w.view());
        CHECK(r.u8() == 7);
        CHECK(r.u16() == 0xBEEF);
        CHECK(r.u32() == 0xDEADBEEF);
        CHECK(r.u64() == 0x0123456789ABCDEFull);
        CHECK(r.f32() == 1.5f);
        CHECK(r.var() == 0);
        CHECK(r.var() == 300);
        CHECK(r.var() == 0xFFFFFFFFu);
        CHECK(r.str(16) == "hi");
        CHECK(r.done());
    }
    for (std::size_t n = 0; n < full; ++n) { // every truncation fails without reading past n
        Reader r(std::span<const std::uint8_t>(buf.data(), n));
        r.u8(), r.u16(), r.u32(), r.u64(), r.f32(), r.var(), r.var(), r.var(), r.str(16);
        CHECK(!r.ok());
    }
    { // overflow latches and does not write past the capacity
        std::array<std::uint8_t, 8> small{};
        Writer s(small.data(), 4);
        s.u32(1);
        CHECK(s.ok());
        s.u8(9);
        CHECK(!s.ok());
        CHECK(small[4] == 0);
        CHECK(s.reserve(1) == nullptr);
    }
    { // varint longer than 5 bytes / > 32 bits rejected
        const std::uint8_t six[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
        Reader r(six);
        r.var();
        CHECK(!r.ok());
        const std::uint8_t big[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F};
        Reader r2(big);
        r2.var();
        CHECK(!r2.ok());
    }
    { // string longer than the cap rejected; huge declared length rejected
        std::array<std::uint8_t, 32> b{};
        Writer s(b.data(), b.size());
        s.str("0123456789");
        Reader r(s.view());
        CHECK(r.str(4).empty());
        CHECK(!r.ok());
        const std::uint8_t lie[] = {0xFF, 0xFF, 0x03, 'a'};
        Reader r2(lie);
        r2.str(1 << 20);
        CHECK(!r2.ok());
    }
    { // snapshot: full + delta round trip, removal, unchanged sections absent, malformed rejected
        using namespace elmp::net::snap;
        auto a = std::make_unique<Frame>(), b = std::make_unique<Frame>(), got = std::make_unique<Frame>(), got2 = std::make_unique<Frame>();
        a->seq = 1, a->t_ms = 100, a->map = 3, a->me = 2;
        for (std::uint16_t k = 1; k <= 128; ++k) {
            auto* e = a->e.add(k);
            e->x = k * 1.5f, e->hp = k;
        }
        for (std::uint16_t k = 1; k <= 300; ++k) a->w.add(k)->flags = 1;
        auto* lily=a->l.add(1);lily->hp=400;
        lily->events.layers[0]={12,7,2,0.5f,0,0,0};
        lily->events.layers[3]={13,8,1,8,0,0,.1f};
        std::vector<std::uint8_t> buf(64 * 1024);
        const auto n_full = encode(*a, nullptr, 0, buf);
        CHECK(n_full > 0);
        CHECK(decode(std::span(buf.data(), n_full), nullptr, *got));
        CHECK(got->e.n == 128 && got->e.find(77)->x == 77 * 1.5f && got->w.n == 300 && got->l.find(1)->hp == 400 && got->me == 2);
        CHECK(got->l.find(1)->events==lily->events);
        *b = *a;
        b->seq = 2;
        b->e.rows[5].hp = 999;                      // one field changed
        b->e.rows[9] = b->e.rows[--b->e.n];          // one row removed (order changes)
        const auto n_delta = encode(*b, a.get(), 1, buf);
        CHECK(n_delta > 0 && n_delta < 64);          // l / w absent, tiny delta
        CHECK(decode(std::span(buf.data(), n_delta), got.get(), *got2));
        CHECK(got2->seq == 2 && got2->e.n == 127 && got2->e.find(6)->hp == 999 && !got2->e.find(10) && got2->w.n == 300);
        CHECK(!decode(std::span(buf.data(), n_delta), nullptr, *got2)); // delta without its base
        for (std::size_t n = 0; n < n_delta; ++n) CHECK(!decode(std::span(buf.data(), n), got.get(), *got2));
        std::vector<std::uint8_t> bad(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n_full));
        bad[15] = 0xFF; // first section tag / count corrupted
        CHECK(!decode(bad, nullptr, *got2));
        std::vector<std::uint8_t> small(100);
        CHECK(encode(*a, nullptr, 0, small) == 0); // too small → 0, no overflow
        *b=*a;b->seq=3;b->l.rows[0].events={};
        auto event_size=encode(*b,a.get(),1,buf);CHECK(event_size&&decode(std::span(buf.data(),event_size),a.get(),*got)&&got->l.rows[0].events==LilyEvents{});
        for(float invalid:{-1.0f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
            *b=*a;b->l.rows[0].events.layers[0].time=invalid;
            event_size=encode(*b,nullptr,0,buf);CHECK(event_size&&!decode(std::span(buf.data(),event_size),nullptr,*got));
        }
    }
    { // Real track indices (damage=80), same-name restart generations, explicit empty/removal, and full capacity.
        using namespace snap;
        auto a = std::make_unique<Frame>(), b = std::make_unique<Frame>(), out = std::make_unique<Frame>();
        Pose pose; pose.flags = 1; pose.count = 8;
        for (std::size_t i = 0; i < pose.count; ++i) {
            auto& t = pose.tracks[i];
            t.index = static_cast<std::uint8_t>(i == 7 ? 80 : i);
            t.flags = 1; t.anim = 7; t.generation = static_cast<std::uint32_t>(i + 1);
            t.t = 0.75f; t.mix_time = 0.1f; t.mix_duration = 0.2f;
            t.animation_start = 0.2f; t.animation_end = 2; t.track_end = std::numeric_limits<float>::max();
        }
        Visual visual; visual.valid = 31; visual.pivot.fill(1); visual.renderer.fill(2); visual.color.fill(0.5f); visual.dissolve = 0.375f;visual.root_scale={-1,1};
        const auto fill = [&](auto& section, std::uint16_t cap) {
            for (std::uint16_t k = 1; k <= cap; ++k) { auto* row = section.add(k); row->pose = pose; row->visual = visual; }
        };
        a->seq = 1;
        for (std::uint16_t i = 1; i <= 8; ++i) a->l.add(i)->hp = 400;
        fill(a->e, 128); fill(a->ed, 64);
        CHECK(!a->e.add(129));
        for (std::uint16_t i = 1; i <= 512; ++i) a->w.add(i)->flags = 1;
        for (std::uint16_t i = 1; i <= 64; ++i) a->pk.add(i)->x = 2;
        std::vector<std::uint8_t> bytes(128 * 1024);
        auto n = encode(*a, nullptr, 0, bytes);
        CHECK(n > 64 * 1024 && n < bytes.size());
        CHECK(decode(std::span(bytes.data(), n), nullptr, *out));
        CHECK(out->e.rows[0].pose == pose && out->ed.rows[63].visual.color == visual.color && out->e.rows[0].visual.root_scale==visual.root_scale);
        const auto maximum_full = n;
        std::fprintf(stderr, "maximum native-Lily/enemy snapshot: %zu bytes (limit %zu)\\n", n, bytes.size());
        *b = *a; b->seq = 2;
        const auto replace_keys = [](auto& section) {
            for (auto& row : std::span(section.rows.data(), section.n)) row.key += 1000;
        };
        replace_keys(b->l); replace_keys(b->e); replace_keys(b->ed); replace_keys(b->w); replace_keys(b->pk);
        n = encode(*b, a.get(), 1, bytes);
        CHECK(n > maximum_full && n < bytes.size() && decode(std::span(bytes.data(), n), a.get(), *out));
        CHECK(out->l.n == 8 && out->e.n == 128 && out->ed.n == 64 && out->w.n == 512 && out->pk.n == 64);
        CHECK(!out->e.find(1) && out->ed.find(1064)->pose == pose && out->w.find(1512));
        *b = *a; b->seq = 3; b->e.rows[0].key = 60000;
        n = encode(*b, a.get(), 1, bytes);
        CHECK(n > 0 && decode(std::span(bytes.data(), n), a.get(), *out));
        CHECK(out->e.n == 128 && !out->e.find(1) && out->e.find(60000)->pose == pose);
        *b = *a; b->seq = 4; b->ed.clear();
        n = encode(*b, a.get(), 1, bytes);
        CHECK(n > 0 && n < 512 && decode(std::span(bytes.data(), n), a.get(), *out));
        CHECK(out->ed.n == 0 && out->l.n == 8 && out->e.n == 128);
        *b = *a; b->seq = 5; b->e.rows[0].visual.valid = 0;
        n = encode(*b, a.get(), 1, bytes);
        CHECK(n > 0 && decode(std::span(bytes.data(), n), a.get(), *out));
        CHECK(out->e.find(1)->visual.valid == 0 && out->e.find(1)->visual.dissolve == 0);
        *b = *a; b->seq = 2;
        auto& changed = b->e.rows[0].pose;
        ++changed.tracks[7].generation; // same animation name, new playback instance
        changed.tracks[7].flags = 2; changed.tracks[7].anim = 0; // real <empty> transition, not track absence
        n = encode(*b, a.get(), 1, bytes);
        CHECK(n > 0 && n < 512 && decode(std::span(bytes.data(), n), a.get(), *out));
        CHECK(out->e.rows[0].pose == changed);
        // Pause is an existing flags bit: it survives a delta without changing the encoded pose size.
        const auto unpaused_size = n;
        changed.flags = 5;
        n = encode(*b, a.get(), 1, bytes);
        CHECK(n == unpaused_size && decode(std::span(bytes.data(), n), a.get(), *out) && out->e.rows[0].pose == changed);
        changed.flags = 7; // paused + overflow still carries a valid bounded active set
        n = encode(*b, a.get(), 1, bytes);
        CHECK(n == unpaused_size && decode(std::span(bytes.data(), n), a.get(), *out) && out->e.rows[0].pose.flags == 7);
        changed = {}; changed.flags = 1; // authoritative complete empty set clears all previous tracks
        n = encode(*b, a.get(), 1, bytes);
        CHECK(decode(std::span(bytes.data(), n), a.get(), *out) && out->e.rows[0].pose.count == 0 && out->e.rows[0].pose.flags == 1);
        // Malformed active sets, floats, flags, and Visual masks are rejected by the real decoder.
        for (int bad = 0; bad < 8; ++bad) {
            b->e.rows[0].pose = pose; b->e.rows[0].visual = visual;
            if (bad == 0) b->e.rows[0].pose.count = 9;
            if (bad == 1) b->e.rows[0].pose.tracks[1].index = 0;
            if (bad == 2) b->e.rows[0].pose.tracks[0].t = std::numeric_limits<float>::infinity();
            if (bad == 3) b->e.rows[0].visual.valid = 32;
            if (bad == 4) b->e.rows[0].visual.color[0] = std::numeric_limits<float>::quiet_NaN();
            if (bad == 5) b->e.rows[0].visual.dissolve = std::numeric_limits<float>::infinity();
            if (bad == 6) b->e.rows[0].pose.flags = 9; // unknown state flag
            if (bad == 7) { b->e.rows[0].pose = {}; b->e.rows[0].pose.flags = 4; } // pause without a valid pose
            n = encode(*b, a.get(), 1, bytes);
            CHECK(n && !decode(std::span(bytes.data(), n), a.get(), *out));
        }
    }
    { // typed messages: round trip, value ranges, truncation
        using namespace elmp::net::proto;
        std::array<std::uint8_t, 256> mb{};
        Input in;
        in.x = 100, in.y = -100, in.raw = 1 << 3, in.presses[3] = 65535, in.snap_ack = 9;
        auto n = encode(in, mb);
        Input in2;
        CHECK(n && decode(std::span(mb.data(), n), in2) && in2.presses[3] == 65535 && in2.snap_ack == 9);
        mb[2] = 101; // x out of range
        CHECK(!decode(std::span(mb.data(), n), in2));
        Act act;
        act.ability = 5, act.xf = {0, 0, 0, 1, 1, 2, 3, 1, 1, 1};
        n = encode(act, mb);
        Act act2;
        CHECK(n && decode(std::span(mb.data(), n), act2) && act2.xf[5] == 2);
        for (std::size_t k = 0; k < n; ++k) CHECK(!decode(std::span(mb.data(), k), act2));
        act.xf[3] = 0; // degenerate quaternion
        n = encode(act, mb);
        CHECK(!decode(std::span(mb.data(), n), act2));
        act.xf[3] = 1, act.kind = 6; // unknown owner kind (Companion is 5)
        n = encode(act, mb);
        CHECK(!decode(std::span(mb.data(), n), act2));
        act.kind = static_cast<std::uint8_t>(RefKind::Companion); act.net_id = 60000;
        n = encode(act, mb);
        CHECK(n && decode(std::span(mb.data(), n), act2) && act2.kind == 5 && act2.net_id == 60000);
        act.max_execution_time = -2.0f; act.damage_rate = -0.25f;
        n = encode(act, mb);
        CHECK(n && decode(std::span(mb.data(), n), act2) && act2.max_execution_time == -2.0f && act2.damage_rate == -0.25f);
        act.damage_rate = std::numeric_limits<float>::infinity();
        n = encode(act, mb); CHECK(n && !decode(std::span(mb.data(), n), act2));
        act.damage_rate = 1; act.max_execution_time = std::numeric_limits<float>::quiet_NaN();
        n = encode(act, mb); CHECK(n && !decode(std::span(mb.data(), n), act2));
        Fx fx;
        fx.kind = 5, fx.net_id = 60000, fx.seq = 77, fx.ps = 9, fx.custom_bone = 3, fx.flags = 3, fx.sort = -2, fx.pool = 1;
        n = encode(fx, mb);
        Fx fx2;
        CHECK(n && decode(std::span(mb.data(), n), fx2) && fx2.kind == 5 && fx2.net_id == 60000 && fx2.seq == 77 && fx2.sort == -2 && fx2.pool == 1);
        for (std::size_t k = 0; k < n; ++k) CHECK(!decode(std::span(mb.data(), k), fx2));
        fx.flags |= 16 | 32; // initial state and proven feedback are independent wire bits
        n = encode(fx, mb);
        CHECK(n && decode(std::span(mb.data(), n), fx2) && fx2.flags == 51);
        fx.flags |= 64;
        n = encode(fx, mb); CHECK(n && !decode(std::span(mb.data(), n), fx2));
        fx.flags = 3;
        fx.ps = 0; // no particle system
        n = encode(fx, mb);
        CHECK(!decode(std::span(mb.data(), n), fx2));
        Fx emitter;
        emitter.kind = 5; emitter.net_id = 60000; emitter.ps = 9; emitter.flags = 9; emitter.pool = 1;
        emitter.attach_kind = static_cast<std::uint8_t>(FxAttach::Component);
        emitter.attach_component = 17; emitter.attach_socket = 18; emitter.emitter_flags = 3;
        emitter.xf = {15, 30, 5, 1, 7, 13, 19, 0.5f, 2, 3}; // Euler is not a quaternion
        for (int rule = 0; rule <= 3; ++rule) {
            emitter.sort = rule;
            n = encode(emitter, mb);
            CHECK(n && decode(std::span(mb.data(), n), fx2) && fx2.xf == emitter.xf && fx2.sort == rule);
            CHECK(fx2.attach_kind == 2 && fx2.attach_component == 17 && fx2.attach_socket == 18 && fx2.emitter_flags == 3);
            for (std::size_t k = 0; k < n; ++k) CHECK(!decode(std::span(mb.data(), k), fx2));
            mb[n] = 0; CHECK(!decode(std::span(mb.data(), n + 1), fx2));
        }
        for (int bad = 0; bad < 12; ++bad) {
            auto malformed = emitter;
            if (bad == 0) malformed.attach_kind = 0;
            if (bad == 1) malformed.attach_kind = 5;
            if (bad == 2) malformed.attach_component = 0;
            if (bad == 3) malformed.attach_kind = 1; // explicit root cannot carry a component path
            if (bad == 4) malformed.emitter_flags = 4;
            if (bad == 5) malformed.sort = 4;
            if (bad == 6) malformed.custom_bone = 19;
            if (bad == 7) malformed.xf[3] = 0;
            if (bad == 8) malformed.xf[1] = std::numeric_limits<float>::quiet_NaN();
            if (bad == 9) malformed.xf[7] = std::numeric_limits<float>::infinity();
            if (bad == 10) malformed.flags = 8; // attached bit required
            if (bad == 11) malformed.flags = 11; // follow rotation is meaningful only for bone identity
            n = encode(malformed, mb); CHECK(n && !decode(std::span(mb.data(), n), fx2));
        }
        emitter.attach_kind = static_cast<std::uint8_t>(FxAttach::SpineBone);
        emitter.bone = 60; emitter.flags |= 2;
        n = encode(emitter, mb); CHECK(n && decode(std::span(mb.data(), n), fx2) && fx2.bone == 60);
        emitter.flags = 63; // feedback + initial state preserve the existing emitter/bone/baseline semantics
        n = encode(emitter, mb); CHECK(n && decode(std::span(mb.data(), n), fx2) && fx2.flags == 63 && fx2.bone == 60);
        emitter.bone = 255;
        n = encode(emitter, mb); CHECK(n && !decode(std::span(mb.data(), n), fx2));
        emitter.bone = 0; emitter.flags = 9; emitter.attach_component = 0;
        for (auto kind : {FxAttach::Root, FxAttach::CompanionFx}) {
            emitter.attach_kind = static_cast<std::uint8_t>(kind);
            n = encode(emitter, mb); CHECK(n && decode(std::span(mb.data(), n), fx2));
        }
        fx.ps = 9; fx.xf[0] = 15; // ordinary BaseFX must still have a unit quaternion
        n = encode(fx, mb); CHECK(n && !decode(std::span(mb.data(), n), fx2));
        NameTable attachment_names;
        CHECK(attachment_names.intern(std::string(kMaxName, 'x')) != 0);
        CHECK(attachment_names.intern(std::string(kMaxName + 1, 'x')) == 0);
        ActStop as{5, 60000, 12, 2};
        n = encode(as, mb);
        ActStop as2;
        CHECK(n && decode(std::span(mb.data(), n), as2) && as2.seq == 12 && as2.kind == 5 && as2.net_id == 60000);
        Snd sd{5, 60000, 8, 1};
        n = encode(sd, mb);
        Snd sd2;
        CHECK(n && decode(std::span(mb.data(), n), sd2) && sd2.kind == 5 && sd2.net_id == 60000 && sd2.event == 8 && !decode(std::span(mb.data(), n - 1), sd2));
        NameDef nd{7, "map_church_08"};
        n = encode(nd, mb);
        NameDef nd2;
        CHECK(decode(std::span(mb.data(), n), nd2) && nd2.text == "map_church_08" && type_of(std::span(mb.data(), n)) == Msg::NameDef);
        Welcome wel;
        CHECK(!decode(std::span(mb.data(), n), wel)); // wrong type
        NameTable t;
        bool fresh = false;
        CHECK(t.intern("a", &fresh) == 1 && fresh && t.intern("a", &fresh) == 1 && !fresh && *t.text(1) == "a" && !t.text(2));
    }
    { // Proven feedback audio carries the original FTransform, including the 2D forwarding entry.
        using namespace elmp::net::proto;
        std::array<std::uint8_t, 64> wire{};
        Snd a{5, 60000, 8, 0}, b;
        a.t_ms=1234;a.causal_root=42;
        a.xf = {0, 0, 0.6f, 0.8f, -1e6f, 1e6f, 123.5f, 0.5f, 2, -1};
        for (std::uint8_t flags : {0, 2, 3}) for (std::uint8_t is2d : {0, 1}) {
            a.flags = flags; a.is2d = is2d;
            const auto n = encode(a, wire);
            CHECK(n == ((flags & 2) ? 60 : 20));
            CHECK(decode(std::span(wire.data(), n), b) && b.flags == flags && b.is2d == is2d && b.event == a.event);
            CHECK(b.t_ms==a.t_ms&&b.causal_root==a.causal_root);
            CHECK(b.xf == ((flags & 2) ? a.xf : Snd{}.xf));
            for (std::size_t cut = 0; cut < n; ++cut) CHECK(!decode(std::span(wire.data(), cut), b));
            wire[n] = 0; CHECK(!decode(std::span(wire.data(), n + 1), b));
            CHECK(encode(a, std::span(wire.data(), n - 1)) == 0);
        }
        a.flags = 3; a.is2d = 1;
        for (std::uint8_t flags : {1, 4, 7, 128, 255}) {
            auto malformed = a; malformed.flags = flags;
            CHECK(encode(malformed, wire) == 0);
            auto n = encode(a, wire); wire[7] = flags;
            CHECK(!decode(std::span(wire.data(), n), b));
            // flags=1 is also invalid at the short wire length (including is2d=1).
            CHECK(!decode(std::span(wire.data(), 8), b));
        }
        for (int bad = 0; bad < 5; ++bad) {
            auto malformed = a;
            if (bad == 0) malformed.kind = 0;
            if (bad == 1) malformed.kind = kRefKindMax + 1;
            if (bad == 2) malformed.event = 0;
            if (bad == 3) malformed.is2d = 2;
            if (bad == 4) malformed.is2d = 255;
            CHECK(encode(malformed, wire) == 0);
            const auto n = encode(a, wire);
            if (bad <= 1) wire[1] = malformed.kind;
            if (bad == 2) wire[4] = wire[5] = 0;
            if (bad >= 3) wire[6] = malformed.is2d;
            CHECK(!decode(std::span(wire.data(), n), b));
        }
        for (std::size_t field = 0; field < a.xf.size(); ++field) {
            for (const auto value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1e6f - 1, 1e6f + 1}) {
                auto malformed = a; malformed.xf[field] = value;
                CHECK(encode(malformed, wire) == 0);
                const auto n = encode(a, wire);
                Writer patch(std::span(wire).subspan(8 + 4 * field)); patch.f32(value);
                CHECK(!decode(std::span(wire.data(), n), b));
            }
        }
        for (const auto qw : {0.0f, 2.0f}) {
            auto malformed = a; malformed.xf[2] = 0; malformed.xf[3] = qw;
            CHECK(encode(malformed, wire) == 0);
            const auto n = encode(a, wire);
            Writer patch(std::span(wire).subspan(16)); patch.f32(0); patch.f32(qw);
            CHECK(!decode(std::span(wire.data(), n), b));
        }
        // Reusing a receive struct for legacy owner-relative audio must clear all prior exact fields.
        const auto exact_n = encode(a, wire);
        CHECK(decode(std::span(wire.data(), exact_n), b) && b.xf == a.xf);
        a.flags = 0;
        const auto short_n = encode(a, wire);
        CHECK(short_n == 20 && decode(std::span(wire.data(), short_n), b) && b.flags == 0 && b.xf == Snd{}.xf);
    }
    { // A child stop names a full native path under an immutable root, including repeated entries.
        using namespace elmp::net::proto;
        std::array<std::uint8_t, 128> wire{};
        ChildStop a, b;
        a.kind = static_cast<std::uint8_t>(RefKind::Companion);
        a.net_id = 65535; a.seq = std::numeric_limits<std::uint32_t>::max(); a.ability = 65535; a.reason = 3;
        a.path.depth = static_cast<std::uint8_t>(kAbilityPathDepth);
        for (std::size_t i = 0; i < kAbilityPathDepth; ++i) {
            a.path.entries[i] = static_cast<std::uint16_t>(i ? 65535 - i : 0); // entry zero is valid
            a.path.occurrences[i] = std::numeric_limits<std::uint32_t>::max() - static_cast<std::uint32_t>(i);
        }
        const auto n = encode(a, wire);
        CHECK(n == 60 && decode(std::span(wire.data(), n), b) && a == b);
        CHECK(type_of(std::span(wire.data(), n)) == Msg::ChildStop);
        for (std::size_t cut = 0; cut < n; ++cut) CHECK(!decode(std::span(wire.data(), cut), b));
        wire[n] = 0; CHECK(!decode(std::span(wire.data(), n + 1), b));
        CHECK(encode(a, std::span(wire.data(), n - 1)) == 0);
        encode(a, wire);
        wire[10] = 9; // reject an attacker-controlled path count before reading bounded arrays
        CHECK(!decode(std::span(wire.data(), n), b));
        encode(a, wire);
        wire[13] = wire[14] = wire[15] = wire[16] = 0; // occurrence zero never identifies an activation
        CHECK(!decode(std::span(wire.data(), n), b));
        encode(a, wire); wire[n - 1] = 4;
        CHECK(!decode(std::span(wire.data(), n), b));
        for (int bad = 0; bad < 9; ++bad) {
            auto malformed = a;
            if (bad == 0) malformed.kind = 0;
            if (bad == 1) malformed.kind = kRefKindMax + 1;
            if (bad == 2) malformed.net_id = 0;
            if (bad == 3) malformed.seq = 0;
            if (bad == 4) malformed.ability = 0;
            if (bad == 5) malformed.path.depth = 0;
            if (bad == 6) malformed.path.depth = 9;
            if (bad == 7) malformed.path.occurrences[7] = 0;
            if (bad == 8) malformed.reason = 4;
            CHECK(!valid_child_stop(malformed) && encode(malformed, wire) == 0);
        }
        a.path = {}; a.path.depth = 1; a.path.entries[0] = 7; a.path.occurrences[0] = 1;
        auto short_n = encode(a, wire);
        CHECK(short_n == 18 && decode(std::span(wire.data(), short_n), b) && b == a);
        ++a.path.occurrences[0]; // same root/class/entry, a different original activation
        short_n = encode(a, wire);
        CHECK(decode(std::span(wire.data(), short_n), b) && b == a && b.path.occurrences[0] == 2);
    }
    { // Host-confirmed reaction: complete original data/context and bounded asset references.
        using namespace elmp::net::proto;
        std::array<std::uint8_t, 512> wire{};
        Reaction a, b;
        a.seat = 2; a.epoch = 3; a.map = 4; a.seq = 1001; a.t_ms = 120;
        a.flags = 15; a.ground = {500, 0, 500}; a.air = {200, 0, 400}; a.swim = {30, 0, 20};
        a.stun = 0.25f; a.move = 0.3f; a.strength = 100; a.curve = 9;
        a.direction = {0, -1, 0}; a.position = {10, 20, 30}; a.velocity = {0, 12, -70};
        a.launch_factor = {1, 1, 1}; a.effective_launch = {0, -500, 500}; a.movement_factor = 1;
        a.movement_mode = 3; a.custom_mode = 0; a.context = 3; a.knockback_type = 1;
        a.state_count = static_cast<std::uint8_t>(kReactionStates);
        for (std::size_t i = 0; i < kReactionStates; ++i) a.states[i] = static_cast<std::uint16_t>(i + 10);
        const auto n = encode(a, wire);
        CHECK(n && decode(std::span(wire.data(), n), b) && a == b && type_of(std::span(wire.data(), n)) == Msg::Reaction);
        for (std::size_t cut = 0; cut < n; ++cut) CHECK(!decode(std::span(wire.data(), cut), b));
        wire[n] = 0; CHECK(!decode(std::span(wire.data(), n + 1), b));
        wire[15] = 17; // shared AdditionalStates count is checked before either payload's array read
        CHECK(!decode(std::span(wire.data(), n), b));
        for (int bad = 0; bad < 13; ++bad) {
            auto malformed = a;
            if (bad == 0) malformed.seat = 0;
            if (bad == 1) malformed.epoch = 0;
            if (bad == 2) malformed.map = 0;
            if (bad == 3) malformed.seq = 0;
            if (bad == 4) malformed.flags = 16;
            if (bad == 5) malformed.state_count = 17;
            if (bad == 6) malformed.states[0] = 0;
            if (bad == 7) malformed.knockback_type = 2;
            if (bad == 8) malformed.context = 2;
            if (bad == 9) malformed.direction[0] = std::numeric_limits<float>::quiet_NaN();
            if (bad == 10) malformed.movement_mode = 7;
            if (bad == 11) malformed.velocity[0] = std::numeric_limits<float>::infinity();
            if (bad == 12) malformed.kind = 3;
            CHECK(!valid_reaction(malformed) && encode(malformed, wire) == 0);
        }
        auto unused = a;
        unused.duration = std::numeric_limits<float>::quiet_NaN(); unused.factor = std::numeric_limits<float>::infinity();
        CHECK(encode(unused, wire) == n && decode(std::span(wire.data(), n), b) && b == a); // HitStop fields are not Knockback data
    }
    { // HitStop has its own payload/validation, sharing only route/order and state assets with Knockback.
        using namespace elmp::net::proto;
        std::array<std::uint8_t, 128> wire{};
        Reaction a, b;
        a.kind = 1; a.seat = 8; a.epoch = 7; a.map = 5; a.seq = 1002; a.t_ms = 121;
        a.duration = 0.2f; a.factor = 0.75f;
        a.state_count = static_cast<std::uint8_t>(kReactionStates);
        for (std::size_t i = 0; i < kReactionStates; ++i) a.states[i] = static_cast<std::uint16_t>(65535 - i);
        const auto n = encode(a, wire);
        CHECK(n == 56 && decode(std::span(wire.data(), n), b) && a == b);
        for (std::size_t cut = 0; cut < n; ++cut) CHECK(!decode(std::span(wire.data(), cut), b));
        wire[n] = 0; CHECK(!decode(std::span(wire.data(), n + 1), b));
        CHECK(encode(a, std::span(wire.data(), n - 1)) == 0);
        encode(a, wire); wire[1] = 3;
        CHECK(!decode(std::span(wire.data(), n), b));
        encode(a, wire); wire[15] = 17;
        CHECK(!decode(std::span(wire.data(), n), b));
        encode(a, wire); wire[16] = wire[17] = 0;
        CHECK(!decode(std::span(wire.data(), n), b));
        encode(a, wire);
        Writer nonfinite(std::span(wire.data() + 48, 8));
        nonfinite.f32(std::numeric_limits<float>::quiet_NaN()); nonfinite.f32(1);
        CHECK(!decode(std::span(wire.data(), n), b));
        for (int bad = 0; bad < 14; ++bad) {
            auto malformed = a;
            if (bad == 0) malformed.duration = 0;
            if (bad == 1) malformed.duration = -1;
            if (bad == 2) malformed.factor = 0;
            if (bad == 3) malformed.factor = -1;
            if (bad == 4) malformed.duration = std::numeric_limits<float>::quiet_NaN();
            if (bad == 5) malformed.duration = std::numeric_limits<float>::infinity();
            if (bad == 6) malformed.factor = std::numeric_limits<float>::quiet_NaN();
            if (bad == 7) malformed.factor = std::numeric_limits<float>::infinity();
            if (bad == 8) malformed.duration = 1e6f + 1;
            if (bad == 9) malformed.factor = 1e6f + 1;
            if (bad == 10) { malformed.duration = 1e6f; malformed.factor = 2; } // effective value exceeds the wire bound
            if (bad == 11) malformed.duration = malformed.factor = 1e-30f; // positive inputs underflow to a no-op
            if (bad == 12) malformed.state_count = 17;
            if (bad == 13) malformed.states[15] = 0;
            CHECK(!valid_reaction(malformed) && encode(malformed, wire) == 0);
        }
        auto unused = a;
        unused.flags = unused.movement_mode = unused.knockback_type = unused.context = 255;
        unused.curve = 65535; unused.position[0] = std::numeric_limits<float>::quiet_NaN();
        unused.stun = std::numeric_limits<float>::infinity();
        CHECK(valid_reaction(unused) && encode(unused, wire) == n);
        CHECK(decode(std::span(wire.data(), n), b) && b == a); // no Knockback context leaks into HitStop
        a.state_count = 0; a.states = {}; a.duration = 1e6f; a.factor = 1;
        CHECK(encode(a, wire) == 24 && decode(std::span(wire.data(), 24), b) && b == a);
    }
    { // Pure DamageColor carries no Knockback/HitStop payload or additional states.
        using namespace elmp::net::proto;
        std::array<std::uint8_t, 32> wire{};
        Reaction a, b;
        a.kind = 2; a.seat = 2; a.epoch = 3; a.map = 4; a.seq = 1003; a.t_ms = 122;
        const auto n = encode(a, wire);
        CHECK(n == 16 && decode(std::span(wire.data(), n), b) && a == b);
        for (std::size_t cut = 0; cut < n; ++cut) CHECK(!decode(std::span(wire.data(), cut), b));
        wire[n] = 0; CHECK(!decode(std::span(wire.data(), n + 1), b));
        wire[15] = 1; CHECK(!decode(std::span(wire.data(), n), b));
        a.state_count = 1; a.states[0] = 1;
        CHECK(!valid_reaction(a) && encode(a, wire) == 0);
    }
})

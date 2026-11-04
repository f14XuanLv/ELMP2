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
        a->l.add(1)->hp = 400;
        std::vector<std::uint8_t> buf(64 * 1024);
        const auto n_full = encode(*a, nullptr, 0, buf);
        CHECK(n_full > 0);
        CHECK(decode(std::span(buf.data(), n_full), nullptr, *got));
        CHECK(got->e.n == 128 && got->e.find(77)->x == 77 * 1.5f && got->w.n == 300 && got->l.find(1)->hp == 400 && got->me == 2);
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
        Visual visual; visual.valid = 15; visual.pivot.fill(1); visual.renderer.fill(2); visual.color.fill(0.5f); visual.dissolve = 0.375f;
        const auto fill = [&](auto& section, std::uint16_t cap) {
            for (std::uint16_t k = 1; k <= cap; ++k) { auto* row = section.add(k); row->pose = pose; row->visual = visual; }
        };
        a->seq = 1;
        fill(a->l, 8); fill(a->e, 128); fill(a->ed, 64); fill(a->sp, 32); fill(a->cp, 48);
        for (std::uint16_t i = 0; i < a->cp.n; ++i) {
            auto& row = a->cp.rows[i];
            row.seat = static_cast<std::uint8_t>(i % 8 + 1); row.cls = 600 + i;
            row.x = i * 1.5f; row.y = -i * 2.0f; row.z = 80 + i;
            row.pitch = i * 0.25f; row.yaw = 90 + i; row.roll = -i * 0.5f;
            row.facing = -1; row.flags = 2;
        }
        CHECK(!a->cp.add(49)); // the declared companion capacity cannot be exceeded
        for (std::uint16_t i = 1; i <= 512; ++i) a->w.add(i)->flags = 1;
        for (std::uint16_t i = 1; i <= 64; ++i) a->pk.add(i)->x = 2;
        std::vector<std::uint8_t> bytes(128 * 1024);
        auto n = encode(*a, nullptr, 0, bytes);
        CHECK(n > 64 * 1024 && n < bytes.size());
        CHECK(decode(std::span(bytes.data(), n), nullptr, *out));
        CHECK(out->e.rows[0].pose == pose && out->sp.rows[31].visual.color == visual.color);
        CHECK(out->cp.n == 48 && out->cp.find(48)->seat == 8 && out->cp.find(48)->cls == 647 && out->cp.find(48)->flags == 2);
        CHECK(out->cp.find(48)->x == 70.5f && out->cp.find(48)->y == -94 && out->cp.find(48)->z == 127 && out->cp.find(48)->facing == -1);
        CHECK(out->cp.find(48)->pitch == 11.75f && out->cp.find(48)->yaw == 137 && out->cp.find(48)->roll == -23.5f);
        CHECK(out->cp.find(48)->pose == pose && out->cp.find(48)->visual.dissolve == visual.dissolve);
        std::fprintf(stderr, "maximum pose/visual/48-companion snapshot: %zu bytes (limit %zu)\n", n, bytes.size());
        const auto maximum_full = n;
        CHECK(maximum_full == 128649 + 48 * 12); // protocol 6 adds the complete Actor rotation to each cp
        *b = *a; b->seq = 2;
        const auto replace_keys = [](auto& section) {
            for (auto& row : std::span(section.rows.data(), section.n)) row.key += 1000;
        };
        replace_keys(b->l); replace_keys(b->e); replace_keys(b->ed); replace_keys(b->w);
        replace_keys(b->pk); replace_keys(b->sp); replace_keys(b->cp);
        n = encode(*b, a.get(), 1, bytes); // all rows change identity: full payload plus every removed key
        CHECK(n > maximum_full && n < bytes.size() && decode(std::span(bytes.data(), n), a.get(), *out));
        CHECK(n == 130363 + 48 * 12 && bytes.size() - n == 133);
        CHECK(out->l.n == 8 && out->e.n == 128 && out->ed.n == 64 && out->w.n == 512 && out->pk.n == 64 && out->sp.n == 32 && out->cp.n == 48);
        CHECK(!out->cp.find(1) && out->cp.find(1048)->pose == pose && out->w.find(1512) && out->e.find(1128));
        std::fprintf(stderr, "maximum replacement delta: %zu bytes (limit %zu)\n", n, bytes.size());
        *b = *a; b->seq = 2;
        auto& cp = b->cp.rows[0];
        cp.seat = 8; cp.cls = 1234; cp.x = -15; cp.facing = 1; cp.flags = 0xFF;
        cp.pitch = 7; cp.yaw = -23; cp.roll = 9;
        ++cp.pose.tracks[0].generation;
        cp.visual.dissolve = 0.875f;
        b->cp.rows[1] = b->cp.rows[--b->cp.n]; // removal can reorder rows independently of their identities
        n = encode(*b, a.get(), 1, bytes);
        CHECK(n > 0 && n < 1024 && decode(std::span(bytes.data(), n), a.get(), *out));
        CHECK(out->cp.n == 47 && !out->cp.find(2) && out->cp.find(1)->seat == 8 && out->cp.find(1)->cls == 1234);
        CHECK(out->cp.find(1)->x == -15 && out->cp.find(1)->facing == 1 && out->cp.find(1)->flags == 0xFF);
        CHECK(out->cp.find(1)->pitch == 7 && out->cp.find(1)->yaw == -23 && out->cp.find(1)->roll == 9);
        CHECK(out->cp.find(1)->pose == cp.pose && out->cp.find(1)->visual.dissolve == 0.875f && out->sp.n == 32);
        for (std::size_t cut = 0; cut < n; ++cut) CHECK(!decode(std::span(bytes.data(), cut), a.get(), *out));
        *b = *a; b->seq = 3;
        b->cp.rows[0].key = 60000; // replacing a key in a full section must remove the old key before insertion
        n = encode(*b, a.get(), 1, bytes);
        CHECK(n > 0 && decode(std::span(bytes.data(), n), a.get(), *out));
        CHECK(out->cp.n == 48 && !out->cp.find(1) && out->cp.find(60000) && out->cp.find(60000)->pose == pose);
        *b = *a; b->seq = 4; b->cp.clear();
        n = encode(*b, a.get(), 1, bytes);
        CHECK(n > 0 && n < 160 && decode(std::span(bytes.data(), n), a.get(), *out));
        CHECK(out->cp.n == 0 && out->l.n == 8 && out->e.n == 128);
        *b = *a; b->seq = 5; b->cp.rows[0].visual.valid = 0;
        n = encode(*b, a.get(), 1, bytes);
        CHECK(n > 0 && decode(std::span(bytes.data(), n), a.get(), *out));
        CHECK(out->cp.find(1)->visual.valid == 0 && out->cp.find(1)->visual.dissolve == 0);
        for (int bad = 0; bad < 3; ++bad) {
            *b = *a; b->seq = 6;
            if (bad == 0) b->cp.rows[0].pitch = std::numeric_limits<float>::quiet_NaN();
            if (bad == 1) b->cp.rows[0].yaw = std::numeric_limits<float>::infinity();
            if (bad == 2) b->cp.rows[0].roll = -std::numeric_limits<float>::infinity();
            n = encode(*b, a.get(), 1, bytes);
            CHECK(n && !decode(std::span(bytes.data(), n), a.get(), *out));
        }
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
            if (bad == 3) b->e.rows[0].visual.valid = 16;
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
        Fx fx;
        fx.kind = 5, fx.net_id = 60000, fx.seq = 77, fx.ps = 9, fx.custom_bone = 3, fx.flags = 3, fx.sort = -2, fx.pool = 1;
        n = encode(fx, mb);
        Fx fx2;
        CHECK(n && decode(std::span(mb.data(), n), fx2) && fx2.kind == 5 && fx2.net_id == 60000 && fx2.seq == 77 && fx2.sort == -2 && fx2.pool == 1);
        for (std::size_t k = 0; k < n; ++k) CHECK(!decode(std::span(mb.data(), k), fx2));
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
})

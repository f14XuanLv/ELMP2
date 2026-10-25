#include "base/binary_manifest.h"

#include <algorithm>
#include <cstdlib>

namespace elmp::binary {

std::optional<std::vector<std::uint8_t>> parse_hex(std::string_view hex) {
    if (hex.empty() || hex.size() % 2) return std::nullopt;
    const auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

bool prefix_matches(std::span<const std::uint8_t> code, std::span<const std::uint8_t> prefix) noexcept {
    return !prefix.empty() && code.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), code.begin());
}

namespace {

std::string str_field(const json::Value& v, std::string_view key) {
    const auto* f = v.get(key);
    return f && f->is_string() ? f->str : std::string{};
}

bool parse_check(const std::string& name, const json::Value* c, OffsetCheck& out, std::string& err) {
    if (!c || !c->is_object()) return err = name + ": check missing", false;
    if (const auto* eq = c->get("equals")) {
        out.kind = OffsetCheck::Kind::Equals;
        out.cls = str_field(*eq, "class");
        out.property = str_field(*eq, "property");
        if (out.cls.empty() || out.property.empty()) return err = name + ": equals needs class + property", false;
    } else if (const auto* w = c->get("within")) {
        out.kind = OffsetCheck::Kind::Within;
        out.cls = str_field(*w, "class");
        out.after = str_field(*w, "after");
        out.before = str_field(*w, "before");
        if (out.cls.empty() || out.after.empty() || out.before.empty()) return err = name + ": within needs class + after + before", false;
    } else if (const auto* s = c->get("struct_size"); s && s->is_string()) {
        out.kind = OffsetCheck::Kind::StructSize;
        out.cls = s->str;
    } else {
        return err = name + ": unknown check", false;
    }
    return true;
}

} // namespace

std::optional<Manifest> parse(std::string_view text, std::string* err) {
    std::string why;
    const auto fail = [&](std::string w) -> std::optional<Manifest> {
        if (err) *err = std::move(w);
        return std::nullopt;
    };
    json::Value root;
    if (!json::parse(text, root, &why)) return fail("json: " + why);
    const auto* schema = root.get("schema");
    if (!schema || schema->number_or(0) != 1) return fail("unsupported schema");
    Manifest m;
    m.module = str_field(root, "module");
    if (m.module.empty()) return fail("missing module");

    if (const auto* fns = root.get("functions")) {
        if (!fns->is_object()) return fail("functions must be an object");
        for (const auto& [name, f] : fns->obj) {
            Function fn{.name = name};
            const auto rva = str_field(f, "rva");
            char* end = nullptr;
            fn.rva = static_cast<std::uintptr_t>(std::strtoull(rva.c_str(), &end, 16));
            if (rva.empty() || !end || *end || !fn.rva) return fail(name + ": bad rva");
            auto bytes = parse_hex(str_field(f, "prefix"));
            if (!bytes || bytes->size() < kMinPrefix) return fail(name + ": prefix must be >= 6 hex bytes");
            fn.prefix = std::move(*bytes);
            fn.signature = str_field(f, "signature");
            fn.owner = str_field(f, "owner");
            fn.source = str_field(f, "source");
            fn.reflect = str_field(f, "reflect");
            if (fn.source.empty()) return fail(name + ": source tag missing");
            m.functions.emplace(name, std::move(fn));
        }
    }
    if (const auto* offs = root.get("offsets")) {
        if (!offs->is_object()) return fail("offsets must be an object");
        for (const auto& [name, o] : offs->obj) {
            Offset off{.name = name};
            const auto* value = o.get("value");
            if (!value || !value->is_number() || value->num < 0) return fail(name + ": value must be a non-negative number");
            off.value = static_cast<std::int64_t>(value->num);
            off.size = static_cast<std::int32_t>(o.get("size") ? o.get("size")->number_or(0) : 0);
            off.source = str_field(o, "source");
            if (off.source.empty()) return fail(name + ": source tag missing");
            if (!parse_check(name, o.get("check"), off.check, why)) return fail(why);
            m.offsets.emplace(name, std::move(off));
        }
    }
    return m;
}

} // namespace elmp::binary

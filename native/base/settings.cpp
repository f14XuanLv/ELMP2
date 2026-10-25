#include "base/settings.h"

#include "base/json.h"
#include "base/log.h"

#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <optional>

namespace elmp::settings {

namespace {

struct Store {
    std::map<std::string, Entry, std::less<>> entries;
    std::filesystem::path file;
    json::Value extra = json::Value::object(); // persisted keys this build does not know
    std::vector<Listener> listeners;
};

Store& store() {
    static Store s;
    return s;
}

std::string check(const Spec& spec, const Value& v) {
    if (v.index() != spec.def.index()) return "wrong type for " + spec.key;
    if (const auto* d = std::get_if<double>(&v)) {
        if (!std::isfinite(*d) || *d < spec.min || *d > spec.max)
            return std::format("{} must be within [{}, {}]", spec.key, spec.min, spec.max);
    }
    return {};
}

json::Value to_json(const Value& v) {
    if (const auto* b = std::get_if<bool>(&v)) return *b;
    if (const auto* d = std::get_if<double>(&v)) return *d;
    return std::get<std::string>(v);
}

std::optional<Value> from_json(const json::Value& j, const Value& like) {
    if (std::holds_alternative<bool>(like) && j.is_bool()) return Value(j.b);
    if (std::holds_alternative<double>(like) && j.is_number()) return Value(j.num);
    if (std::holds_alternative<std::string>(like) && j.is_string()) return Value(j.str);
    return std::nullopt;
}

} // namespace

void define(Spec spec) {
    auto& s = store();
    const auto key = spec.key;
    Value v = spec.def;
    s.entries.insert_or_assign(key, Entry{std::move(spec), std::move(v)});
}

std::string load(const std::filesystem::path& file) {
    auto& s = store();
    s.file = file;
    std::string err;
    auto root = json_io::load(file, kSchema, &err);
    if (!root) return err == "missing" ? std::string{} : err;
    std::string problems;
    if (const auto* values = root->get("values"); values && values->is_object()) {
        for (const auto& [k, j] : values->obj) {
            auto it = s.entries.find(k);
            if (it == s.entries.end() || !it->second.spec.persist) {
                s.extra.set(k, j);
                continue;
            }
            auto v = from_json(j, it->second.spec.def);
            const auto why = v ? check(it->second.spec, *v) : "wrong type for " + k;
            if (!why.empty()) {
                problems += (problems.empty() ? "" : "; ") + why;
                continue;
            }
            it->second.value = std::move(*v);
        }
    }
    return problems;
}

std::string save() {
    auto& s = store();
    if (s.file.empty()) return "settings file not set";
    json::Value values = s.extra;
    for (const auto& [k, e] : s.entries)
        if (e.spec.persist) values.set(k, to_json(e.value));
    json::Value root = json::Value::object();
    root.set("schema", kSchema);
    root.set("values", std::move(values));
    std::string err;
    return json_io::save(s.file, root, &err) ? std::string{} : err;
}

std::string set_value(std::string_view key, Value v) {
    auto& s = store();
    auto it = s.entries.find(key);
    if (it == s.entries.end()) return "unknown setting " + std::string(key);
    if (auto why = check(it->second.spec, v); !why.empty()) return why;
    it->second.value = std::move(v);
    std::string err;
    if (it->second.spec.persist) err = save();
    for (const auto& l : s.listeners) l(it->first);
    return err.empty() ? std::string{} : "saved in memory, not persisted: " + err;
}

std::string set(std::string_view key, std::string_view text) {
    auto& s = store();
    auto it = s.entries.find(key);
    if (it == s.entries.end()) return "unknown setting " + std::string(key);
    const auto& def = it->second.spec.def;
    if (std::holds_alternative<bool>(def)) {
        if (text == "1" || text == "true" || text == "on") return set_value(key, true);
        if (text == "0" || text == "false" || text == "off") return set_value(key, false);
        return std::string(key) + " expects 0/1";
    }
    if (std::holds_alternative<double>(def)) {
        double d = 0;
        const auto r = std::from_chars(text.data(), text.data() + text.size(), d);
        if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) return std::string(key) + " expects a number";
        return set_value(key, d);
    }
    return set_value(key, std::string(text));
}

bool get_bool(std::string_view key) {
    auto& s = store();
    auto it = s.entries.find(key);
    const auto* b = it == s.entries.end() ? nullptr : std::get_if<bool>(&it->second.value);
    return b && *b;
}

double get_number(std::string_view key) {
    auto& s = store();
    auto it = s.entries.find(key);
    const auto* d = it == s.entries.end() ? nullptr : std::get_if<double>(&it->second.value);
    return d ? *d : 0.0;
}

std::string get_string(std::string_view key) {
    auto& s = store();
    auto it = s.entries.find(key);
    const auto* str = it == s.entries.end() ? nullptr : std::get_if<std::string>(&it->second.value);
    return str ? *str : std::string{};
}

std::string to_text(const Value& v) {
    if (const auto* b = std::get_if<bool>(&v)) return *b ? "1" : "0";
    if (const auto* d = std::get_if<double>(&v)) return std::format("{}", *d);
    return std::get<std::string>(v);
}

void on_change(Listener listener) { store().listeners.push_back(std::move(listener)); }

std::vector<Entry> all() {
    std::vector<Entry> out;
    for (const auto& [_, e] : store().entries) out.push_back(e);
    return out;
}

} // namespace elmp::settings

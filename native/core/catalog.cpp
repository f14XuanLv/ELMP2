#include "core/catalog.h"

#include "base/json.h"

#include <format>

namespace elmp::core::catalog {

namespace {

constexpr int kSchema = 1;
constexpr std::string_view kCulture = "en-US";

std::vector<Spirit> g_spirits;
std::vector<Passive> g_passives;
bool g_loaded = false;

std::string text(const json::Value& row, std::string_view field, std::string_view fallback) {
    const auto* f = row.get(field);
    const auto* t = f ? f->get(kCulture) : nullptr;
    return std::string(t ? t->string_or(fallback) : fallback);
}

} // namespace

std::string load(const std::filesystem::path& file) {
    std::string err;
    const auto root = json_io::load(file, kSchema, &err);
    if (!root) return std::format("{}: {}", file.string(), err);
    if (const auto* s = root->get("spirits"); s && s->is_object())
        for (const auto& [id, row] : s->obj)
            g_spirits.push_back({id, text(row, "name", id), text(row, "desc", ""), std::string(row.get("icon") ? row.get("icon")->string_or("") : ""),
                                 row.get("type") && row.get("type")->string_or("").ends_with("::Main")});
    if (const auto* p = root->get("passives"); p && p->is_object())
        for (const auto& [id, row] : p->obj)
            g_passives.push_back({id, text(row, "name", id), text(row, "desc", ""), std::string(row.get("icon") ? row.get("icon")->string_or("") : ""),
                                  static_cast<int>(row.get("slots") ? row.get("slots")->number_or(0) : 0),
                                  row.get("available") && row.get("available")->bool_or(false)});
    g_loaded = !g_spirits.empty() && !g_passives.empty();
    return g_loaded ? std::string{} : "catalog has no spirits / passives";
}

bool loaded() { return g_loaded; }

const Spirit* spirit(std::string_view id) {
    for (const auto& s : g_spirits)
        if (s.id == id) return &s;
    return nullptr;
}

const Passive* passive(std::string_view id) {
    for (const auto& p : g_passives)
        if (p.id == id) return &p;
    return nullptr;
}

const std::vector<Spirit>& spirits() { return g_spirits; }
const std::vector<Passive>& passives() { return g_passives; }

} // namespace elmp::core::catalog

// Storage: JSON edge cases, atomic replace with .bak, schema / corruption reporting, and settings
// persistence (invalid persisted values are reported, never silently applied; transient keys never hit disk).

#include "base/json.h"
#include "base/settings.h"
#include "check.h"

#include <Windows.h>

#include <filesystem>

using namespace elmp;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir() {
    wchar_t buf[MAX_PATH];
    GetTempPathW(MAX_PATH, buf);
    auto dir = fs::path(buf) / (L"elmp_test_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void json_edges() {
    json::Value v;
    std::string err;
    CHECK(json::parse("{\"a\":[1,2.5,-3e2,true,null],\"s\":\"\\u00e9\\ud83d\\ude00\\n\"}", v, &err));
    CHECK(v.get("a") && v.get("a")->arr.size() == 5 && v.get("a")->arr[2].num == -300);
    CHECK(v.get("s") && v.get("s")->str == "\xC3\xA9\xF0\x9F\x98\x80\n");
    CHECK(!json::parse(R"({"a":1,"a":2})", v, &err));   // duplicate key
    CHECK(!json::parse("\"\\ud83d\"", v, &err));        // lone surrogate
    CHECK(!json::parse(R"({"a":1} x)", v, &err));       // trailing data
    CHECK(!json::parse("[1,2", v, &err));
    CHECK(!json::parse(std::string(100, '[') + std::string(100, ']'), v, &err)); // depth limit
    CHECK(!json::parse("1e999", v, &err));               // non-finite
    json::Value round, src;
    CHECK(json::parse("{\"a\":[1,{\"b\":null}],\"s\":\"\\u00e9\\t\"}", src, &err));
    CHECK(json::parse(json::dump(src), round, &err) && json::dump(round) == json::dump(src));
    json::Value o = json::Value::object();
    o.set("k", "x\"y");
    o.set("n", 0.1);
    CHECK(json::parse(json::dump(o), round, &err) && round.get("k")->str == "x\"y" && round.get("n")->num == 0.1);
}

void atomic_write(const fs::path& dir) {
    const auto f = dir / "sub" / "a.json";
    std::string err;
    CHECK(json_io::write_atomic(f, "first", &err));
    CHECK(json_io::write_atomic(f, "second", &err));
    CHECK(json_io::read_file(f).value_or("") == "second");
    CHECK(json_io::read_file(fs::path(f).concat(L".bak")).value_or("") == "first");
    CHECK(!fs::exists(fs::path(f).concat(L".tmp")));

    CHECK(json_io::write_atomic(f, R"({"schema":3,"x":1})", &err));
    CHECK(!json_io::load(f, 1, &err) && err.starts_with("schema mismatch"));
    CHECK(json_io::write_atomic(f, R"({"schema":1,"x":)", &err));
    CHECK(!json_io::load(f, 1, &err) && err.starts_with("corrupt json"));
    CHECK(!json_io::load(dir / "none.json", 1, &err) && err == "missing");
}

void settings_roundtrip(const fs::path& dir) {
    const auto f = dir / "settings.json";
    settings::define({.key = "pvpDamage", .def = 0.1, .min = 0, .max = 1});
    settings::define({.key = "equipAnywhere", .def = false});
    settings::define({.key = "muteInput", .def = false, .persist = false});
    CHECK(settings::load(f).empty()); // missing file is fine
    CHECK(settings::set("pvpDamage", "0.5").empty());
    CHECK(!settings::set("pvpDamage", "2").empty());   // out of range
    CHECK(!settings::set("pvpDamage", "abc").empty());
    CHECK(!settings::set("nope", "1").empty());
    CHECK(settings::set("muteInput", "1").empty());
    CHECK(settings::get_number("pvpDamage") == 0.5 && settings::get_bool("muteInput"));
    auto root = json_io::load(f, settings::kSchema);
    CHECK(root && root->get("values") && root->get("values")->get("pvpDamage") && !root->get("values")->get("muteInput"));

    // A corrupted persisted value is reported and the default stays.
    CHECK(json_io::write_atomic(f, R"({"schema":1,"values":{"pvpDamage":7,"equipAnywhere":true,"future":3}})"));
    settings::define({.key = "pvpDamage", .def = 0.1, .min = 0, .max = 1});
    CHECK(!settings::load(f).empty());
    CHECK(settings::get_number("pvpDamage") == 0.1 && settings::get_bool("equipAnywhere"));
    CHECK(settings::set("equipAnywhere", "0").empty());
    root = json_io::load(f, settings::kSchema);
    CHECK(root && root->get("values")->get("future") && root->get("values")->get("future")->num == 3); // unknown keys kept
}

} // namespace

TEST_MAIN({
    const auto dir = temp_dir();
    json_edges();
    atomic_write(dir);
    settings_roundtrip(dir);
    fs::remove_all(dir);
})

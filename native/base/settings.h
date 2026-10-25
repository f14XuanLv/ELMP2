#pragma once

// Typed settings (F1.17 / §7.11): every key is declared once with its type, default, range and whether
// it persists. `elmp set` writes through to Saved/ELMP/ELMP/settings.json (schema 1, atomic write);
// transient keys (e.g. muteInput) never reach the file. Game thread only.

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace elmp::settings {

using Value = std::variant<bool, double, std::string>;

struct Spec {
    std::string key;
    Value def;
    bool persist = true;
    double min = -1e300, max = 1e300; // numbers only
    std::string help;
};

inline constexpr int kSchema = 1;

void define(Spec spec);
// Loads persisted values; unknown keys are kept in the file untouched, invalid ones are reported.
// Returns an empty string on success (or missing file), otherwise the reason (file left as is).
std::string load(const std::filesystem::path& file);
std::string save();

// Parses text by the key's type ("1"/"0"/"true"/"false" for bools). Empty string = accepted.
std::string set(std::string_view key, std::string_view text);
std::string set_value(std::string_view key, Value v);

bool get_bool(std::string_view key);
double get_number(std::string_view key);
std::string get_string(std::string_view key);
std::string to_text(const Value& v);

using Listener = std::function<void(const std::string& key)>;
void on_change(Listener listener);

struct Entry {
    Spec spec;
    Value value;
};
std::vector<Entry> all();

} // namespace elmp::settings

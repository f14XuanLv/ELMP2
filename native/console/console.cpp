#include "console/console.h"

#include "base/log.h"

#include <Unreal/FOutputDevice.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include <cctype>
#include <format>
#include <map>
#include <vector>

namespace elmp::console {

using namespace RC::Unreal;

namespace {

struct Command {
    std::string usage;
    Handler handler;
};

std::map<std::string, Command, std::less<>>& table() {
    static std::map<std::string, Command, std::less<>> t;
    return t;
}

Hook::GlobalCallbackId g_hook{};
bool g_installed = false;

std::vector<std::string> split(std::string_view line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size()) break;
        size_t j = i;
        while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
        out.emplace_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void help(Args, std::string& out) {
    out += "elmp commands:\n";
    for (const auto& [name, c] : table()) out += std::format("  elmp {:<10} {}\n", name, c.usage);
}

} // namespace

void add(const char* name, const char* usage, Handler handler) { table().insert_or_assign(name, Command{usage, handler}); }

bool run(std::string_view line, std::string& out) {
    auto words = split(line);
    if (words.empty() || lower(words[0]) != "elmp") return false;
    if (words.size() == 1) {
        help({}, out);
        return true;
    }
    const auto name = lower(words[1]);
    auto it = table().find(name);
    if (it == table().end()) {
        out += "unknown command '" + name + "'\n";
        help({}, out);
        return true;
    }
    const std::vector<std::string> args(words.begin() + 2, words.end());
    try {
        it->second.handler(args, out);
    } catch (const std::exception& e) {
        out += std::string("error: ") + e.what() + "\n";
    }
    return true;
}

void install() {
    if (g_installed) return;
    g_installed = true;
    add("help", "list commands", help);
    Hook::FCallbackOptions opt;
    opt.OwnerModName = STR("ELMP");
    opt.HookName = STR("console");
    g_hook = Hook::RegisterProcessConsoleExecCallback(
        [](auto& data, UObject*, const TCHAR* cmd, FOutputDevice& ar, UObject*) {
            if (!cmd || data.GetCurrentResolvedReturnValue()) return;
            std::string out;
            if (!run(log::utf8(cmd), out)) return;
            data.TrySetReturnValue(true);
            size_t start = 0;
            while (start < out.size()) {
                auto end = out.find('\n', start);
                if (end == std::string::npos) end = out.size();
                const auto line = log::wide(std::string_view(out).substr(start, end - start));
                ar.Log(line.c_str());
                start = end + 1;
            }
            log::info("console: {}\n{}", log::utf8(cmd), out);
        },
        opt);
}

void uninstall() {
    if (!g_installed) return;
    Hook::UnregisterCallback(g_hook);
    g_installed = false;
}

} // namespace elmp::console

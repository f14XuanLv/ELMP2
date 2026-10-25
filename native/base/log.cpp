#include "base/log.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>

namespace elmp::log {

namespace {
std::atomic<Sink> g_sink{nullptr};
}

void set_sink(Sink sink) { g_sink.store(sink); }

void write(Level level, std::string_view text) {
    if (auto sink = g_sink.load()) return sink(level, text);
    std::fprintf(stderr, "[ELMP] %.*s\n", static_cast<int>(text.size()), text.data());
}

std::string utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

} // namespace elmp::log

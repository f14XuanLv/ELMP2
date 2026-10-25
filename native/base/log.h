#pragma once

// Logging front end. base does not depend on UE4SS: the mod layer installs a sink that forwards to
// RC::Output (UE4SS.log); without a sink (CTest) messages go to stderr.

#include <format>
#include <string>
#include <string_view>

namespace elmp::log {

enum class Level { Verbose, Info, Warning, Error };

using Sink = void (*)(Level, std::string_view utf8);
void set_sink(Sink sink);
void write(Level level, std::string_view utf8);

template <class... A>
void info(std::format_string<A...> f, A&&... a) { write(Level::Info, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void warn(std::format_string<A...> f, A&&... a) { write(Level::Warning, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void error(std::format_string<A...> f, A&&... a) { write(Level::Error, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void verbose(std::format_string<A...> f, A&&... a) { write(Level::Verbose, std::format(f, std::forward<A>(a)...)); }

std::string utf8(std::wstring_view wide);
std::wstring wide(std::string_view utf8);

} // namespace elmp::log

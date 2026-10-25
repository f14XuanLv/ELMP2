#pragma once

// Read-only parser for v1 `*.profile.lua` files (C12 migration, ARCHITECTURE §10). Only the subset v1's
// store.serialize wrote: optional `--` comment lines, `return <value>`, tables with `name = v`,
// `["key"] = v`, `[int] = v` and positional entries, %q strings, integers / %.17g numbers, true / false.
// Arrays (only positional entries) become JSON arrays, everything else JSON objects with string keys.
// Anything else is an error with line:column — the caller never guesses.

#include "base/json.h"

#include <string>
#include <string_view>

namespace elmp::lua_literal {

bool parse(std::string_view text, json::Value& out, std::string* err);

} // namespace elmp::lua_literal

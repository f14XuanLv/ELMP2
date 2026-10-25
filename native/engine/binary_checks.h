#pragma once

// Validation of the raw struct offsets in data/binary/EL.json against reflected neighbours (§7.1).
// Runs once the native classes are available (first Settled); each offset is usable only if its check
// passed — consumers ask offset(name) and fail closed on nullopt.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace elmp::binary_checks {

struct Status {
    std::string name;
    std::int64_t value = 0;
    bool ok = false;
    std::string detail;
};

void run();
bool ran();
const std::vector<Status>& results();
std::optional<std::int64_t> offset(std::string_view name);

} // namespace elmp::binary_checks

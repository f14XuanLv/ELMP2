#pragma once

// Slot leases (M2 / FR-SLOT-1). Slot 1 = P1 (read-only, never leased); 2..100 = ELMP internal;
// 101..1000 = test (Dev builds only, ELMP or the dev tool); >1000 = ranges declared by third-party mods
// in their elmp.json — overlapping declarations are refused for the later loader.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace elmp::slots {

enum class OwnerKind : std::uint8_t { Elmp, DevTool, ThirdParty };

struct Owner {
    std::string name;
    OwnerKind kind = OwnerKind::ThirdParty;
};

enum class Result : std::uint8_t { Ok, ReadOnly, OutOfRange, HeldByOther, NotDeclared };
const char* describe(Result r) noexcept;

inline constexpr std::int64_t kP1 = 1;
inline constexpr std::int64_t kInternalFirst = 2, kInternalLast = 100;
inline constexpr std::int64_t kTestFirst = 101, kTestLast = 1000;

class Table {
public:
    explicit Table(bool dev_build) : dev_(dev_build) {}

    // Empty string = accepted. Only for ThirdParty owners, range must be > 1000 and not overlap.
    std::string declare(const Owner& owner, std::int64_t first, std::int64_t last);
    Result acquire(const Owner& owner, std::int64_t slot); // re-acquiring an own lease is Ok
    bool release(const Owner& owner, std::int64_t slot);
    void release_all(const std::string& owner);
    std::optional<std::string> owner_of(std::int64_t slot) const;
    std::vector<std::pair<std::int64_t, std::string>> leases() const;

private:
    struct Range {
        std::int64_t first, last;
        std::string owner;
    };
    Result allowed(const Owner& owner, std::int64_t slot) const;

    bool dev_;
    mutable std::mutex mutex_;
    std::vector<Range> ranges_;
    std::map<std::int64_t, std::string> leases_;
};

} // namespace elmp::slots

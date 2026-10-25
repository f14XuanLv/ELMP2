#pragma once

// Single include point for UE4SS / UEPseudo types used by engine/ and above, plus the few object
// primitives everything shares. Only engine/ (and modules that install native hooks) include this.

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace elmp::ue {

using RC::Unreal::FName;
using RC::Unreal::FProperty;
using RC::Unreal::UClass;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;
using RC::Unreal::UScriptStruct;
using RC::Unreal::UStruct;

// Live = not null, not unreachable, not being destroyed.
bool alive(const UObject* o) noexcept;

std::wstring name_of(const UObject* o);      // short object name (cold paths only)
std::wstring class_name_of(const UObject* o); // short class name (cold paths only)
std::string utf8_name(const UObject* o);
std::string utf8(const FName& n);

// FName comparison by ComparisonIndex (no string conversion, §4).
inline std::uint32_t name_key(const FName& n) { return n.GetComparisonIndex().ToUnstableInt(); }

// Game-thread identity. Set by the frame pump on its first tick; used to assert thread affinity.
bool on_game_thread() noexcept;
void mark_game_thread() noexcept;

} // namespace elmp::ue

#pragma once

// Reflection access layer (§4). Properties and functions are resolved by name once (type- and
// size-checked) into offsets / parameter layouts; at run time fields are read directly and functions are
// a single ProcessEvent on a stack parameter block. A failed resolution makes the owning schema unusable
// (its capability fails closed) and is retried later only if the caller asks (blueprint classes load
// after the save is read — kb lesson: failures are not cached).
//
// Every name lookup and every ProcessEvent issued by ELMP is counted (reflect.lookups / reflect.calls):
// the G0 gate is "0 calls per frame with no seats".

#include "engine/handle.h"
#include "engine/ue.h"

#include "base/metrics.h"

#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace elmp::reflect {

enum class Kind : std::uint8_t { Any, Bool, Byte, Int, Int64, Float, Double, Object, Class, Name, Str, Struct, Array, Map, Enum, Weak, Delegate };
const char* kind_name(Kind k) noexcept;
Kind kind_of(ue::FProperty* p);

struct PropRef {
    std::int32_t offset = -1;
    std::int32_t size = 0;
    Kind kind = Kind::Any;
    std::uint8_t field_mask = 0xFF; // bool bitfields
    std::uint8_t byte_offset = 0;
    ue::FProperty* prop = nullptr; // containers / structs need the property for their layout

    bool ok() const { return offset >= 0; }
    template <class T>
    T* at(const void* base) const {
        return reinterpret_cast<T*>(static_cast<std::uint8_t*>(const_cast<void*>(base)) + offset);
    }
    template <class T>
    T read(const void* base) const {
        T v;
        std::memcpy(&v, at<std::uint8_t>(base), sizeof(T));
        return v;
    }
    bool read_bool(const void* base) const { return (at<std::uint8_t>(base)[byte_offset] & field_mask) != 0; }
    void write_bool(void* base, bool v) const {
        auto& b = at<std::uint8_t>(base)[byte_offset];
        b = v ? (b | field_mask) : (b & ~field_mask);
    }
    ue::UObject* object(const void* base) const; // object property → live object or nullptr
};

struct ParmRef {
    std::int16_t offset = 0;
    std::int16_t size = 0;
    Kind kind = Kind::Any;
    bool pod = true;
    ue::FProperty* prop = nullptr;
};

struct FuncRef {
    ue::UFunction* fn = nullptr;
    ObjHandle handle;
    std::uint16_t parms_size = 0;
    std::int16_t ret = -1; // index into parms, -1 = void
    std::uint8_t count = 0;
    bool pod = true;
    std::array<ParmRef, 12> parms{};

    bool ok() const { return fn != nullptr; }
};

// Builder used by schema resolve functions; the first failure is kept with its reason.
class Resolver {
public:
    Resolver(std::string_view what, ue::UStruct* type);
    PropRef prop(std::wstring_view name, Kind kind, std::int32_t size = 0);
    FuncRef func(std::wstring_view name, int nparms = -1); // nparms excludes the return value
    bool ok() const { return failure_.empty(); }
    const std::string& failure() const { return failure_; }
    ue::UStruct* type() const { return type_; }

private:
    void fail(std::string why);
    std::string what_;
    ue::UStruct* type_;
    std::string failure_;
};

// Names compare case-insensitively (FName semantics; e.g. the Spine plugin exports `getAnimationName`).
bool same_name(std::wstring_view a, std::wstring_view b) noexcept;

ue::UObject* find_object(std::wstring_view path); // StaticFindObject (counted)
// find_object, else load through the asset registry (cold path: spawn / skin). Blueprint classes
// ("…/BP_X.BP_X_C") are loaded through their blueprint asset first.
ue::UObject* load_object(std::wstring_view path);
ue::UClass* find_class(std::wstring_view path);
ue::FProperty* find_property(ue::UStruct* type, std::wstring_view name); // counted, walks supers
std::int32_t properties_size(ue::UStruct* type);

// Stack parameter block for one call. Non-POD parameters are initialised / destroyed via FProperty.
class Frame {
public:
    explicit Frame(const FuncRef& f);
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    template <class T>
    Frame& arg(int i, const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (void* p = slot(i, sizeof(T))) std::memcpy(p, &v, sizeof(T));
        return *this;
    }
    Frame& arg_bool(int i, bool v);
    Frame& arg_string(int i, std::wstring_view v); // FString (allocated with the engine allocator)
    Frame& arg_name(int i, const ue::FName& n) {
        if (void* p = slot(i, sizeof(ue::FName))) std::memcpy(p, &n, sizeof(ue::FName));
        return *this;
    }
    template <class T>
    T ret() const {
        static_assert(std::is_trivially_copyable_v<T>);
        T v{};
        if (f_.ret >= 0 && sizeof(T) <= static_cast<std::size_t>(f_.parms[f_.ret].size)) std::memcpy(&v, buf_.data() + f_.parms[f_.ret].offset, sizeof(T));
        return v;
    }
    bool ret_bool() const;
    std::wstring ret_string() const; // FString return
    void* raw(int i) { return i >= 0 && i < f_.count ? buf_.data() + f_.parms[i].offset : nullptr; }

    bool invoke(ue::UObject* self); // ProcessEvent; false if the function or self is gone

private:
    void* slot(int i, std::size_t size);
    const FuncRef& f_;
    bool ok_;
    alignas(16) std::array<std::uint8_t, 1024> buf_;
};

// TArray memory (data, num, max); elements are `element_size(p)` bytes apart.
struct RawArray {
    std::uint8_t* data;
    std::int32_t num, max;
};
inline const RawArray& array_at(const void* base, const PropRef& p) { return *p.at<RawArray>(base); }
std::int32_t element_size(const PropRef& array_prop);

// Live (key, value) pairs of a TMap property, in storage order (cold paths: profile reads).
struct Pair {
    const std::uint8_t* key;
    const std::uint8_t* value;
};
std::vector<Pair> map_pairs(const void* base, const PropRef& map_prop);

std::uint64_t calls();
std::uint64_t lookups();

} // namespace elmp::reflect

#pragma once

// ObjHandle (M12, §5): {index, serial} = FWeakObjectPtr. The only form in which a UObject may be kept
// across frames; get() returns nullptr once the object is gone or its slot was reused.

#include "engine/ue.h"

#include <cstdint>
#include <functional>

namespace elmp {

struct ObjHandle {
    std::int32_t index = 0;
    std::int32_t serial = 0;

    static ObjHandle of(const ue::UObject* o) {
        if (!o) return {};
        RC::Unreal::FWeakObjectPtr w(o);
        return {w.ObjectIndex, w.ObjectSerialNumber};
    }
    ue::UObject* get() const {
        if (!serial) return nullptr;
        RC::Unreal::FWeakObjectPtr w;
        w.ObjectIndex = index;
        w.ObjectSerialNumber = serial;
        ue::UObject* o = w.Get();
        return ue::alive(o) ? o : nullptr;
    }
    bool valid() const { return get() != nullptr; }
    bool empty() const { return serial == 0; }
    void reset() { index = serial = 0; }
    bool operator==(const ObjHandle&) const = default;
};

template <class T>
struct Handle : ObjHandle {
    Handle() = default;
    Handle(ObjHandle h) : ObjHandle(h) {}
    static Handle of(const T* o) { return Handle(ObjHandle::of(o)); }
    T* get() const { return static_cast<T*>(ObjHandle::get()); }
};

} // namespace elmp

template <>
struct std::hash<elmp::ObjHandle> {
    std::size_t operator()(const elmp::ObjHandle& h) const noexcept {
        return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(static_cast<std::uint32_t>(h.index)) << 32) | static_cast<std::uint32_t>(h.serial));
    }
};

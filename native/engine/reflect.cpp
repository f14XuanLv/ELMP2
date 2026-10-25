#include "engine/reflect.h"

#include "base/log.h"

#include <Unreal/FString.hpp>

#include <algorithm>
#include <format>

namespace elmp::reflect {

using namespace RC::Unreal;

namespace {

metrics::Counter& g_calls = metrics::counter("reflect.calls");
metrics::Counter& g_lookups = metrics::counter("reflect.lookups");
metrics::Counter& g_failed_calls = metrics::counter("reflect.calls_refused");

struct RawString {
    wchar_t* data;
    std::int32_t num, max;
};

} // namespace

const char* kind_name(Kind k) noexcept {
    switch (k) {
    case Kind::Any: return "any";
    case Kind::Bool: return "bool";
    case Kind::Byte: return "byte";
    case Kind::Int: return "int";
    case Kind::Int64: return "int64";
    case Kind::Float: return "float";
    case Kind::Double: return "double";
    case Kind::Object: return "object";
    case Kind::Class: return "class";
    case Kind::Name: return "name";
    case Kind::Str: return "str";
    case Kind::Struct: return "struct";
    case Kind::Array: return "array";
    case Kind::Map: return "map";
    case Kind::Enum: return "enum";
    case Kind::Weak: return "weak";
    case Kind::Delegate: return "delegate";
    }
    return "?";
}

Kind kind_of(FProperty* p) {
    if (!p) return Kind::Any;
    const auto cls = p->GetClass().GetName();
    if (cls == STR("BoolProperty")) return Kind::Bool;
    if (cls == STR("ByteProperty")) return Kind::Byte;
    if (cls == STR("IntProperty") || cls == STR("UInt32Property")) return Kind::Int;
    if (cls == STR("Int64Property") || cls == STR("UInt64Property")) return Kind::Int64;
    if (cls == STR("FloatProperty")) return Kind::Float;
    if (cls == STR("DoubleProperty")) return Kind::Double;
    if (cls == STR("ObjectProperty") || cls == STR("ObjectPtrProperty")) return Kind::Object;
    if (cls == STR("ClassProperty") || cls == STR("ClassPtrProperty")) return Kind::Class;
    if (cls == STR("NameProperty")) return Kind::Name;
    if (cls == STR("StrProperty")) return Kind::Str;
    if (cls == STR("StructProperty")) return Kind::Struct;
    if (cls == STR("ArrayProperty")) return Kind::Array;
    if (cls == STR("MapProperty")) return Kind::Map;
    if (cls == STR("EnumProperty")) return Kind::Enum;
    if (cls == STR("WeakObjectProperty")) return Kind::Weak;
    if (cls.find(STR("DelegateProperty")) != RC::StringType::npos) return Kind::Delegate;
    return Kind::Any;
}

UObject* PropRef::object(const void* base) const {
    UObject* o = read<UObject*>(base);
    return ue::alive(o) ? o : nullptr;
}

UObject* find_object(std::wstring_view path) {
    ++g_lookups;
    const std::wstring p(path);
    return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, p.c_str());
}

UClass* find_class(std::wstring_view path) { return static_cast<UClass*>(find_object(path)); }

FProperty* find_property(UStruct* type, std::wstring_view name) {
    if (!type) return nullptr;
    ++g_lookups;
    const auto flags = static_cast<EFieldIterationFlags>(static_cast<uint8>(EFieldIterationFlags::IncludeSuper));
    for (FProperty* p : TFieldRange<FProperty>(type, flags)) {
        if (p && p->GetName() == name) return p;
    }
    return nullptr;
}

std::int32_t properties_size(UStruct* type) { return type ? type->GetPropertiesSize() : 0; }

// ---------------- Resolver ----------------

Resolver::Resolver(std::string_view what, UStruct* type) : what_(what), type_(type) {
    if (!type_) fail("type not loaded");
}

void Resolver::fail(std::string why) {
    if (failure_.empty()) failure_ = what_ + ": " + std::move(why);
}

PropRef Resolver::prop(std::wstring_view name, Kind kind, std::int32_t size) {
    PropRef r;
    if (!type_) return r;
    FProperty* p = find_property(type_, name);
    if (!p) {
        fail("no property " + log::utf8(name));
        return r;
    }
    const Kind k = kind_of(p);
    if (kind != Kind::Any && k != kind) {
        fail(std::format("{} is {}, expected {}", log::utf8(name), kind_name(k), kind_name(kind)));
        return r;
    }
    if (size && p->GetSize() != size) {
        fail(std::format("{} size {} != {}", log::utf8(name), p->GetSize(), size));
        return r;
    }
    r.offset = p->GetOffset_Internal();
    r.size = p->GetSize();
    r.kind = k;
    if (k == Kind::Bool) {
        auto* bp = static_cast<FBoolProperty*>(p);
        r.byte_offset = bp->GetByteOffset();
        r.field_mask = bp->GetFieldMask();
    }
    return r;
}

FuncRef Resolver::func(std::wstring_view name, int nparms) {
    FuncRef f;
    if (!type_) return f;
    ++g_lookups;
    UFunction* fn = nullptr;
    const auto flags = static_cast<EFieldIterationFlags>(static_cast<uint8>(EFieldIterationFlags::IncludeSuper) |
                                                         static_cast<uint8>(EFieldIterationFlags::IncludeInterfaces));
    for (UFunction* candidate : TFieldRange<UFunction>(type_, flags)) {
        if (candidate && candidate->GetName() == name) {
            fn = candidate;
            break;
        }
    }
    if (!fn) {
        fail("no function " + log::utf8(name));
        return f;
    }
    f.fn = fn;
    f.handle = ObjHandle::of(fn);
    f.parms_size = fn->GetParmsSize();
    int inputs = 0;
    for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None)) {
        if (!p || !p->HasAnyPropertyFlags(CPF_Parm)) continue;
        if (f.count == f.parms.size()) {
            fail(log::utf8(name) + ": too many parameters");
            return FuncRef{};
        }
        ParmRef& pr = f.parms[f.count];
        pr.offset = static_cast<std::int16_t>(p->GetOffset_Internal());
        pr.size = static_cast<std::int16_t>(p->GetSize());
        pr.kind = kind_of(p);
        pr.pod = p->HasAnyPropertyFlags(CPF_IsPlainOldData);
        pr.prop = p;
        if (!pr.pod) f.pod = false;
        if (p->HasAnyPropertyFlags(CPF_ReturnParm)) f.ret = static_cast<std::int16_t>(f.count);
        else ++inputs;
        ++f.count;
    }
    if (f.ret >= 0 && f.ret != f.count - 1) {
        // Keep "arg(i)" indices = declaration order of the non-return parameters.
        std::rotate(f.parms.begin() + f.ret, f.parms.begin() + f.ret + 1, f.parms.begin() + f.count);
        f.ret = static_cast<std::int16_t>(f.count - 1);
    }
    if (nparms >= 0 && inputs != nparms) {
        fail(std::format("{} has {} parameters, expected {}", log::utf8(name), inputs, nparms));
        return FuncRef{};
    }
    if (f.parms_size > 1024) {
        fail(log::utf8(name) + ": parameter block too large");
        return FuncRef{};
    }
    return f;
}

// ---------------- Frame ----------------

Frame::Frame(const FuncRef& f) : f_(f), ok_(f.ok()) {
    std::memset(buf_.data(), 0, f_.parms_size);
    if (!ok_ || f_.pod) return;
    for (int i = 0; i < f_.count; ++i)
        if (!f_.parms[i].pod) f_.parms[i].prop->InitializeValue(buf_.data() + f_.parms[i].offset);
}

Frame::~Frame() {
    if (!ok_ || f_.pod) return;
    for (int i = 0; i < f_.count; ++i)
        if (!f_.parms[i].pod) f_.parms[i].prop->DestroyValue(buf_.data() + f_.parms[i].offset);
}

void* Frame::slot(int i, std::size_t size) {
    if (!ok_ || i < 0 || i >= f_.count || i == f_.ret || size > static_cast<std::size_t>(f_.parms[i].size)) return nullptr;
    return buf_.data() + f_.parms[i].offset;
}

Frame& Frame::arg_bool(int i, bool v) {
    if (void* p = slot(i, 1)) *static_cast<std::uint8_t*>(p) = v ? 1 : 0;
    return *this;
}

bool Frame::ret_bool() const { return ret<std::uint8_t>() != 0; }

std::wstring Frame::ret_string() const {
    if (f_.ret < 0 || f_.parms[f_.ret].kind != Kind::Str) return {};
    const auto* s = reinterpret_cast<const RawString*>(buf_.data() + f_.parms[f_.ret].offset);
    return (s->data && s->num > 1) ? std::wstring(s->data, static_cast<std::size_t>(s->num - 1)) : std::wstring{};
}

bool Frame::invoke(UObject* self) {
    if (!ok_ || !ue::alive(self) || f_.handle.get() != static_cast<UObject*>(f_.fn)) {
        ++g_failed_calls;
        return false;
    }
    ++g_calls;
    self->ProcessEvent(f_.fn, buf_.data());
    return true;
}

std::uint64_t calls() { return g_calls.load(); }
std::uint64_t lookups() { return g_lookups.load(); }

} // namespace elmp::reflect

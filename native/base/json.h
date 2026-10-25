#pragma once

// Minimal JSON value / parser / writer (UTF-8, order-preserving objects) plus atomic file I/O.
// Used for data/ tables, settings and cold storage. Pure C++ (CTest-covered).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace elmp::json {

struct Value {
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : type(Type::Bool), b(v) {}
    Value(double v) : type(Type::Number), num(v) {}
    Value(int v) : type(Type::Number), num(v) {}
    Value(std::int64_t v) : type(Type::Number), num(static_cast<double>(v)) {}
    Value(std::string v) : type(Type::String), str(std::move(v)) {}
    Value(const char* v) : type(Type::String), str(v) {}
    static Value array() { Value v; v.type = Type::Array; return v; }
    static Value object() { Value v; v.type = Type::Object; return v; }

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    const Value* get(std::string_view key) const;
    Value& set(std::string_view key, Value v); // object: insert or replace (keeps order)
    Value& push(Value v);                      // array

    // Typed reads with fallback (wrong type → fallback).
    double number_or(double fallback) const { return is_number() ? num : fallback; }
    bool bool_or(bool fallback) const { return is_bool() ? b : fallback; }
    std::string_view string_or(std::string_view fallback) const { return is_string() ? std::string_view(str) : fallback; }
};

bool parse(std::string_view text, Value& out, std::string* err = nullptr);
std::string dump(const Value& v, int indent = 2);

} // namespace elmp::json

namespace elmp::json_io {

std::optional<std::string> read_file(const std::filesystem::path& path, std::string* err = nullptr);

// Write to <path>.tmp, flush to disk, then atomically replace <path>; the previous content is kept as
// <path>.bak. Never leaves a half-written <path>.
bool write_atomic(const std::filesystem::path& path, std::string_view content, std::string* err = nullptr);

// Parse a JSON file whose root object carries "schema": <expected>. Missing file → nullopt with err
// "missing"; bad JSON / schema → nullopt with the reason. Never falls back to .bak silently.
std::optional<json::Value> load(const std::filesystem::path& path, int expected_schema, std::string* err = nullptr);
bool save(const std::filesystem::path& path, const json::Value& root, std::string* err = nullptr);

} // namespace elmp::json_io

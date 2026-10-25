#include "base/json.h"

#include <Windows.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace elmp::json {

const Value* Value::get(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (const auto& [k, v] : obj)
        if (k == key) return &v;
    return nullptr;
}

Value& Value::set(std::string_view key, Value v) {
    if (!is_object()) *this = object();
    for (auto& [k, existing] : obj)
        if (k == key) return existing = std::move(v);
    return obj.emplace_back(std::string(key), std::move(v)).second;
}

Value& Value::push(Value v) {
    if (!is_array()) *this = array();
    return arr.emplace_back(std::move(v));
}

namespace {

constexpr int kMaxDepth = 64;

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;

    bool fail(const char* why) {
        if (err.empty()) err = std::string(why) + " at offset " + std::to_string(i);
        return false;
    }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool lit(std::string_view word) {
        if (s.substr(i, word.size()) != word) return fail("bad literal");
        i += word.size();
        return true;
    }
    static void put_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(std::uint32_t& cp) {
        if (i + 4 > s.size()) return fail("short \\u escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= c - '0';
            else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& out) {
        ++i; // opening quote
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) break;
            switch (s[i++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    std::uint32_t lo;
                    if (s.substr(i, 2) != "\\u") return fail("lone surrogate");
                    i += 2;
                    if (!hex4(lo) || lo < 0xDC00 || lo >= 0xE000) return fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return fail("lone surrogate");
                }
                put_utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(Value& out) {
        const size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
        double v = 0;
        const auto r = std::from_chars(s.data() + start, s.data() + i, v);
        if (r.ec != std::errc{} || r.ptr != s.data() + i || !std::isfinite(v)) return fail("bad number");
        out = Value(v);
        return true;
    }
    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        switch (s[i]) {
        case 'n': out = Value(); return lit("null");
        case 't': out = Value(true); return lit("true");
        case 'f': out = Value(false); return lit("false");
        case '"': out = Value(std::string{}); return string(out.str);
        case '[': {
            ++i;
            out = Value::array();
            ws();
            if (i < s.size() && s[i] == ']') return ++i, true;
            while (true) {
                if (!value(out.arr.emplace_back(), depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') return ++i, true;
                return fail("expected , or ]");
            }
        }
        case '{': {
            ++i;
            out = Value::object();
            ws();
            if (i < s.size() && s[i] == '}') return ++i, true;
            while (true) {
                ws();
                if (i >= s.size() || s[i] != '"') return fail("expected key");
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected :");
                ++i;
                Value v;
                if (!value(v, depth + 1)) return false;
                if (out.get(key)) return fail("duplicate key");
                out.obj.emplace_back(std::move(key), std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') return ++i, true;
                return fail("expected , or }");
            }
        }
        default: return number(out);
        }
    }
};

void escape(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void write(std::string& out, const Value& v, int indent, int level) {
    const auto newline = [&](int lv) {
        if (indent <= 0) return;
        out += '\n';
        out.append(static_cast<size_t>(lv * indent), ' ');
    };
    switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.b ? "true" : "false"; break;
    case Value::Type::Number: {
        if (std::floor(v.num) == v.num && std::fabs(v.num) < 9.007199254740992e15) {
            out += std::to_string(static_cast<long long>(v.num));
        } else {
            char buf[32];
            const auto r = std::to_chars(buf, buf + sizeof buf, v.num);
            out.append(buf, r.ptr);
        }
        break;
    }
    case Value::Type::String: escape(out, v.str); break;
    case Value::Type::Array:
        out += '[';
        for (size_t k = 0; k < v.arr.size(); ++k) {
            if (k) out += ',';
            newline(level + 1);
            write(out, v.arr[k], indent, level + 1);
        }
        if (!v.arr.empty()) newline(level);
        out += ']';
        break;
    case Value::Type::Object:
        out += '{';
        for (size_t k = 0; k < v.obj.size(); ++k) {
            if (k) out += ',';
            newline(level + 1);
            escape(out, v.obj[k].first);
            out += indent > 0 ? ": " : ":";
            write(out, v.obj[k].second, indent, level + 1);
        }
        if (!v.obj.empty()) newline(level);
        out += '}';
        break;
    }
}

} // namespace

bool parse(std::string_view text, Value& out, std::string* err) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    Parser p{text};
    if (!p.value(out, 0)) {
        if (err) *err = p.err;
        return false;
    }
    p.ws();
    if (p.i != text.size()) {
        if (err) *err = "trailing data at offset " + std::to_string(p.i);
        return false;
    }
    return true;
}

std::string dump(const Value& v, int indent) {
    std::string out;
    write(out, v, indent, 0);
    if (indent > 0) out += '\n';
    return out;
}

} // namespace elmp::json

namespace elmp::json_io {

std::optional<std::string> read_file(const std::filesystem::path& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = std::filesystem::exists(path) ? "cannot open" : "missing";
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) {
        if (err) *err = "read error";
        return std::nullopt;
    }
    return std::move(ss).str();
}

bool write_atomic(const std::filesystem::path& path, std::string_view content, std::string* err) {
    const auto fail = [&](const char* what) {
        if (err) *err = std::string(what) + " (win32 error " + std::to_string(GetLastError()) + ")";
        return false;
    };
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += L".tmp";
    auto bak = path;
    bak += L".bak";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("create temp file");
    DWORD written = 0;
    const bool ok = WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) && written == content.size() &&
                    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return fail("write temp file");
    }
    if (std::filesystem::exists(path, ec)) {
        if (!ReplaceFileW(path.c_str(), tmp.c_str(), bak.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            DeleteFileW(tmp.c_str());
            return fail("replace file");
        }
    } else if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return fail("move file");
    }
    return true;
}

std::optional<json::Value> load(const std::filesystem::path& path, int expected_schema, std::string* err) {
    auto text = read_file(path, err);
    if (!text) return std::nullopt;
    json::Value root;
    std::string why;
    if (!json::parse(*text, root, &why)) {
        if (err) *err = "corrupt json: " + why;
        return std::nullopt;
    }
    const auto* schema = root.get("schema");
    if (!schema || !schema->is_number() || static_cast<int>(schema->num) != expected_schema) {
        if (err) *err = "schema mismatch (expected " + std::to_string(expected_schema) + ")";
        return std::nullopt;
    }
    return root;
}

bool save(const std::filesystem::path& path, const json::Value& root, std::string* err) {
    return write_atomic(path, json::dump(root), err);
}

} // namespace elmp::json_io

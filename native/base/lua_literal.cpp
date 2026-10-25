#include "base/lua_literal.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace elmp::lua_literal {

namespace {

struct Parser {
    std::string_view s;
    std::size_t i = 0;
    std::string err;
    int depth = 0;

    bool fail(std::string_view what) {
        if (err.empty()) {
            int line = 1, col = 1;
            for (std::size_t k = 0; k < i && k < s.size(); ++k) s[k] == '\n' ? (++line, col = 1) : ++col;
            err = std::format("{}:{}: {}", line, col, what);
        }
        return false;
    }

    void skip() {
        while (i < s.size()) {
            if (std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            else if (s.substr(i, 2) == "--") {
                if (s.substr(i, 4) == "--[[") { // block comment
                    const auto end = s.find("]]", i + 4);
                    i = end == std::string_view::npos ? s.size() : end + 2;
                } else {
                    while (i < s.size() && s[i] != '\n') ++i;
                }
            } else break;
        }
    }

    bool word(std::string_view w) {
        if (s.substr(i, w.size()) != w) return false;
        const auto e = i + w.size();
        if (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_')) return false;
        i = e;
        return true;
    }

    bool string(std::string& out) {
        const char q = s[i++];
        while (i < s.size() && s[i] != q) {
            char c = s[i++];
            if (c == '\n') return fail("newline in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) break;
            c = s[i++];
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'a': out += '\a'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'v': out += '\v'; break;
            case '\\': out += '\\'; break;
            case '"': out += '"'; break;
            case '\'': out += '\''; break;
            case '\n': out += '\n'; break; // %q writes a newline as "\<newline>"
            default:
                if (std::isdigit(static_cast<unsigned char>(c))) {
                    int v = c - '0';
                    for (int k = 0; k < 2 && i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++k) v = v * 10 + (s[i++] - '0');
                    if (v > 255) return fail("bad decimal escape");
                    out += static_cast<char>(v);
                } else {
                    return fail("unsupported escape");
                }
            }
        }
        if (i >= s.size()) return fail("unterminated string");
        ++i;
        return true;
    }

    bool number(json::Value& out) {
        const auto start = i;
        if (s[i] == '-' || s[i] == '+') ++i;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '.' || ((s[i] == '-' || s[i] == '+') && (s[i - 1] == 'e' || s[i - 1] == 'E')))) ++i;
        double v = 0;
        const auto r = std::from_chars(s.data() + start, s.data() + i, v);
        if (r.ec != std::errc{} || r.ptr != s.data() + i || !std::isfinite(v)) return fail("bad number");
        out = json::Value(v);
        return true;
    }

    bool value(json::Value& out) {
        skip();
        if (i >= s.size()) return fail("value expected");
        const char c = s[i];
        if (c == '"' || c == '\'') {
            std::string str;
            if (!string(str)) return false;
            out = json::Value(std::move(str));
            return true;
        }
        if (c == '{') return table(out);
        if (word("true")) return out = json::Value(true), true;
        if (word("false")) return out = json::Value(false), true;
        if (word("nil")) return fail("nil is not supported");
        if (c == '-' || c == '+' || c == '.' || std::isdigit(static_cast<unsigned char>(c))) return number(out);
        return fail("unexpected character");
    }

    bool table(json::Value& out) {
        if (++depth > 32) return fail("nesting too deep");
        ++i; // {
        std::vector<json::Value> positional;
        std::vector<std::pair<std::string, json::Value>> keyed;
        while (true) {
            skip();
            if (i >= s.size()) return fail("unterminated table");
            if (s[i] == '}') {
                ++i;
                break;
            }
            std::string key;
            bool has_key = false;
            if (s[i] == '[') {
                ++i;
                skip();
                json::Value k;
                if (!value(k)) return false;
                if (k.is_string()) key = k.str;
                else if (k.is_number() && k.num == std::floor(k.num)) key = std::format("{}", static_cast<long long>(k.num));
                else return fail("unsupported key");
                skip();
                if (i >= s.size() || s[i] != ']') return fail("] expected");
                ++i;
                has_key = true;
            } else if (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_') {
                const auto start = i;
                while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
                const auto j = i;
                skip();
                if (i < s.size() && s[i] == '=') {
                    key = std::string(s.substr(start, j - start));
                    has_key = true;
                } else {
                    i = start; // a bare value (true / false)
                }
            }
            json::Value v;
            if (has_key) {
                skip();
                if (i >= s.size() || s[i] != '=') return fail("= expected");
                ++i;
            }
            if (!value(v)) return false;
            if (has_key) keyed.emplace_back(std::move(key), std::move(v));
            else positional.push_back(std::move(v));
            skip();
            if (i < s.size() && (s[i] == ',' || s[i] == ';')) ++i;
        }
        --depth;
        if (!positional.empty() && !keyed.empty()) return fail("mixed array / record table");
        if (!keyed.empty() || positional.empty()) {
            out = json::Value::object();
            for (auto& [k, v] : keyed) {
                if (out.get(k)) return fail("duplicate key " + k);
                out.set(k, std::move(v));
            }
            if (keyed.empty()) out = json::Value::object(); // {} stays an (empty) object
        } else {
            out = json::Value::array();
            for (auto& v : positional) out.push(std::move(v));
        }
        return true;
    }
};

} // namespace

bool parse(std::string_view text, json::Value& out, std::string* err) {
    Parser p{text};
    p.skip();
    if (!p.word("return")) {
        if (err) *err = "1:1: 'return' expected";
        return false;
    }
    json::Value v;
    bool ok = p.value(v);
    if (ok) {
        p.skip();
        if (p.i != text.size()) ok = p.fail("trailing content");
    }
    if (!ok) {
        if (err) *err = p.err;
        return false;
    }
    out = std::move(v);
    return true;
}

} // namespace elmp::lua_literal

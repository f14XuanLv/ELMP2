// v1 *.profile.lua migration parser: the exact shapes v1 store.serialize wrote (incl. a real save_index
// file), and refusals for anything outside that subset.

#include "base/lua_literal.h"
#include "check.h"

using namespace elmp;

namespace {

void v1_index() {
    const char* text = R"(-- ELMP profile
return {
    schema = 1,
    slots = {
        GameSlot_0 = {
            hashes = {
                ["0f4f3f935619c730a0ad49fe032ca0839a0095648881fe785e7ac9c8d81a8dd8"] = true,
                a0d631e106ce3500d533c110fa34651a6c7a7a77ae584bc05b64eb9449250b7c = true,
            },
            id = "5996a75fab6c5cb5d6b6ae07fa7fea89",
            recent = {
                "a0d631e106ce3500d533c110fa34651a6c7a7a77ae584bc05b64eb9449250b7c",
                "0f4f3f935619c730a0ad49fe032ca0839a0095648881fe785e7ac9c8d81a8dd8",
            },
        },
    },
}
)";
    json::Value v;
    std::string err;
    CHECK(lua_literal::parse(text, v, &err));
    const auto* slot = v.get("slots") ? v.get("slots")->get("GameSlot_0") : nullptr;
    CHECK(slot && slot->get("id")->str == "5996a75fab6c5cb5d6b6ae07fa7fea89");
    CHECK(slot && slot->get("recent")->is_array() && slot->get("recent")->arr.size() == 2);
    CHECK(slot && slot->get("hashes")->get("a0d631e106ce3500d533c110fa34651a6c7a7a77ae584bc05b64eb9449250b7c")->b);
    CHECK(v.get("schema")->num == 1);
}

void party_shapes() {
    json::Value v;
    std::string err;
    CHECK(lua_literal::parse(R"(return { characters = { ["2"] = { active = true, equipment = { passives = {}, spirits = { { command = 3, id = "s5000", set = 1 } }, summonSet = 1 }, identity = "enemy", input = "devices", skin = 4 } }, saveId = "ab", schema = 1 })", v, &err));
    const auto* c = v.get("characters") ? v.get("characters")->get("2") : nullptr;
    CHECK(c && c->get("skin")->num == 4 && c->get("identity")->str == "enemy");
    CHECK(c && c->get("equipment")->get("passives")->is_object() && c->get("equipment")->get("passives")->obj.empty()); // {} = empty
    CHECK(c && c->get("equipment")->get("spirits")->arr.size() == 1);
    CHECK(lua_literal::parse("return { [1] = 1.5, x = -2e3 }", v, &err) && v.get("1")->num == 1.5 && v.get("x")->num == -2000);
    CHECK(lua_literal::parse("return \"a\\\"b\\\\c\\\nd\\9\\0\"", v, &err) && v.str == std::string("a\"b\\c\nd\t") + '\0');
}

void refusals() {
    json::Value v;
    std::string err;
    CHECK(!lua_literal::parse("{ a = 1 }", v, &err));                 // no return
    CHECK(!lua_literal::parse("return { a = nil }", v, &err));        // nil
    CHECK(!lua_literal::parse("return { a = f() }", v, &err));        // code
    CHECK(!lua_literal::parse("return { 1, a = 2 }", v, &err));       // mixed
    CHECK(!lua_literal::parse("return { a = 1, a = 2 }", v, &err));   // duplicate
    CHECK(!lua_literal::parse("return { a = 1 } x", v, &err));        // trailing
    CHECK(!lua_literal::parse("return { a = \"open", v, &err));       // unterminated
    CHECK(!lua_literal::parse("return { a = 1", v, &err) && err.starts_with("1:"));
    CHECK(!lua_literal::parse(std::string("return ") + std::string(40, '{') + std::string(40, '}'), v, &err)); // depth
}

} // namespace

TEST_MAIN({
    v1_index();
    party_shapes();
    refusals();
})

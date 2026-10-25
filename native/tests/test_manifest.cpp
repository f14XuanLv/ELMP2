// Binary manifest parsing and the prefix check (preflight): the shipped data/binary/EL.json must parse,
// prefixes are compared byte for byte, and malformed entries are refused (fail closed).

#include "base/binary_manifest.h"
#include "base/json.h"
#include "base/preflight.h"
#include "check.h"

#include <vector>

using namespace elmp;

namespace {

void shipped_manifest_parses() {
    std::string err;
    auto text = json_io::read_file(ELMP_DATA_DIR "/binary/EL.json", &err);
    CHECK(text.has_value());
    if (!text) return;
    auto m = binary::parse(*text, &err);
    CHECK(m.has_value());
    if (!m) return std::fprintf(stderr, "EL.json: %s\n", err.c_str()), void();
    CHECK(m->module == "EnderLiliesSteam-Win64-Shipping.exe");
    const auto* consume = m->fn("InputConsume");
    CHECK(consume && consume->rva == 0xCEF310 && consume->prefix.size() == 10 && consume->prefix[0] == 0x48);
    CHECK(m->fn("GuardIsPlayerThunk") && m->fn("GuardIsPlayerThunk")->prefix.size() == 6);
    // X-5: the IsPlayer body must never be a hook target.
    for (const auto& [_, f] : m->functions) CHECK(f.rva != 0xD150F0);
    const auto* heal = m->offset("HealComponent.RemainingHealCount");
    CHECK(heal && heal->value == 296 && heal->check.kind == binary::OffsetCheck::Kind::Within && heal->check.before == "<end>");
    CHECK(m->offset("InputSnapshot.size") && m->offset("InputSnapshot.size")->check.kind == binary::OffsetCheck::Kind::StructSize);
}

void prefix_check_on_image() {
    std::string err;
    auto m = binary::parse(R"({"schema":1,"module":"x.exe","functions":{
        "A":{"rva":"0x10","prefix":"4889E5554157","source":"t"},
        "B":{"rva":"0x20","prefix":"4889E5554158","source":"t"},
        "C":{"rva":"0xFFF0","prefix":"4889E5554157","source":"t"}}})",
                           &err);
    CHECK(m.has_value());
    if (!m) return;
    std::vector<std::uint8_t> image(0x100, 0xCC);
    const std::uint8_t code[] = {0x48, 0x89, 0xE5, 0x55, 0x41, 0x57, 0x90};
    std::copy(std::begin(code), std::end(code), image.begin() + 0x10);
    std::copy(std::begin(code), std::end(code), image.begin() + 0x20);
    const auto st = preflight::check_functions(*m, image.data(), image.size());
    CHECK(st.size() == 3);
    for (const auto& s : st) {
        if (s.name == "A") CHECK(s.ok);
        if (s.name == "B") CHECK(!s.ok && s.reason == "prefix mismatch");  // last byte differs
        if (s.name == "C") CHECK(!s.ok && s.reason == "rva outside image"); // no read past the image
    }
}

void malformed_entries_refused() {
    std::string err;
    CHECK(!binary::parse(R"({"schema":2,"module":"x"})", &err));
    CHECK(!binary::parse(R"({"schema":1,"module":"x","functions":{"A":{"rva":"0x10","prefix":"4889E555","source":"t"}}})", &err)); // < 6 bytes
    CHECK(!binary::parse(R"({"schema":1,"module":"x","functions":{"A":{"rva":"0x10","prefix":"4889E5554157"}}})", &err));        // no source
    CHECK(!binary::parse(R"({"schema":1,"module":"x","functions":{"A":{"rva":"zz","prefix":"4889E5554157","source":"t"}}})", &err));
    CHECK(!binary::parse(R"({"schema":1,"module":"x","functions":{"A":{"rva":"0x10","prefix":"4889E555415G","source":"t"}}})", &err));
    CHECK(!binary::parse(R"({"schema":1,"module":"x","offsets":{"O":{"value":4,"source":"t"}}})", &err)); // no check
    CHECK(!binary::parse(R"({"schema":1,"module":"x","offsets":{"O":{"value":4,"source":"t","check":{"within":{"class":"C"}}}}})", &err));
    CHECK(binary::parse(R"({"schema":1,"module":"x","offsets":{"O":{"value":4,"source":"t","check":{"equals":{"class":"C","property":"P"}}}}})", &err));
}

} // namespace

TEST_MAIN({
    shipped_manifest_parses();
    prefix_check_on_image();
    malformed_entries_refused();
})

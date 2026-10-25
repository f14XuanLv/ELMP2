#include "base/preflight.h"

#include "base/clock.h"
#include "base/json.h"

#include <elmp/build_info.h>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace elmp::preflight {

namespace {

struct Bcrypt {
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE hash{};
    ~Bcrypt() {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
};

Result g_result;
bool g_ran = false;

std::wstring lower(std::wstring s) {
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

std::size_t image_size(const std::uint8_t* base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt->OptionalHeader.SizeOfImage : 0;
}

bool executable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}

} // namespace

std::string sha256_file(const std::filesystem::path& path) {
    Bcrypt b;
    if (BCryptOpenAlgorithmProvider(&b.alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 || BCryptCreateHash(b.alg, &b.hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("cannot initialize SHA-256");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    std::vector<char> buf(1 << 20);
    while (file) {
        file.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (const auto n = file.gcount(); n > 0 && BCryptHashData(b.hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0) < 0)
            throw std::runtime_error("SHA-256 update failed");
    }
    if (!file.eof()) throw std::runtime_error("cannot read " + path.string());
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(b.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw std::runtime_error("SHA-256 finish failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (auto byte : digest) {
        out += hex[byte >> 4];
        out += hex[byte & 15];
    }
    return out;
}

std::vector<FunctionStatus> check_functions(const binary::Manifest& m, const std::uint8_t* base, std::size_t size) {
    std::vector<FunctionStatus> out;
    for (const auto& [name, f] : m.functions) {
        FunctionStatus s{.name = name, .rva = f.rva};
        if (f.rva + f.prefix.size() > size) s.reason = "rva outside image";
        else if (base && !binary::prefix_matches({base + f.rva, f.prefix.size()}, f.prefix)) s.reason = "prefix mismatch";
        else s.ok = true;
        out.push_back(std::move(s));
    }
    return out;
}

const Result& run(const std::filesystem::path& manifest_path) {
    if (g_ran) return g_result;
    g_ran = true;
    const auto t0 = clock::now_us();
    Result& r = g_result;
    const auto finish = [&](std::string reason) -> const Result& {
        r.reason = std::move(reason);
        r.ok = r.reason.empty();
        r.ms = static_cast<double>(clock::now_us() - t0) / 1000.0;
        return r;
    };

    std::array<wchar_t, 32768> name{};
    const auto len = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (!len || len >= name.size()) return finish("cannot identify game executable");
    r.exe = std::filesystem::path(name.data());
    if (lower(r.exe.filename().wstring()) != lower(build::game_exe)) return finish("unexpected game executable name");
    try {
        r.sha256 = sha256_file(r.exe);
    } catch (const std::exception& e) {
        return finish(e.what());
    }
    if (std::ranges::none_of(build::game_sha256, [&](const char* h) { return r.sha256 == h; }))
        return finish("unsupported game executable (SHA-256 not listed in elmp.json)");

    std::string err;
    auto text = json_io::read_file(manifest_path, &err);
    if (!text) return finish("binary manifest " + manifest_path.string() + ": " + err);
    auto manifest = binary::parse(*text, &err);
    if (!manifest) return finish("binary manifest: " + err);
    r.manifest = std::move(*manifest);

    auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    r.image_base = reinterpret_cast<std::uintptr_t>(base);
    r.functions = check_functions(r.manifest, base, image_size(base));
    for (auto& s : r.functions) {
        if (s.ok && !executable(base + s.rva)) s.ok = false, s.reason = "not executable";
    }
    std::string bad;
    for (const auto& s : r.functions)
        if (!s.ok) bad += (bad.empty() ? "" : ", ") + s.name + " (" + s.reason + ")";
    return finish(bad.empty() ? "" : "function prefix check failed: " + bad);
}

const Result& current() { return g_result; }

void* address(std::string_view function_name) {
    if (!g_result.ok) return nullptr;
    const auto* f = g_result.manifest.fn(function_name);
    return f ? reinterpret_cast<void*>(g_result.image_base + f->rva) : nullptr;
}

} // namespace elmp::preflight

#pragma once

// Startup preflight (M1 / C1.1 / F0.2): the running EXE's SHA-256 must be listed in elmp.json and every
// function in data/binary/EL.json must match its byte prefix. Any failure disables ELMP as a whole
// (no degraded mode); the reason is logged and shown by `elmp preflight`.

#include "base/binary_manifest.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace elmp::preflight {

struct FunctionStatus {
    std::string name;
    std::uintptr_t rva = 0;
    bool ok = false;
    std::string reason;
};

struct Result {
    bool ok = false;
    std::string reason;
    std::string sha256;
    std::filesystem::path exe;
    std::vector<FunctionStatus> functions;
    binary::Manifest manifest;
    std::uintptr_t image_base = 0;
    double ms = 0.0;
};

std::string sha256_file(const std::filesystem::path& path); // lower-case hex; throws on I/O errors

// Prefix check of every manifest function against a mapped image [base, base + size).
std::vector<FunctionStatus> check_functions(const binary::Manifest& m, const std::uint8_t* base, std::size_t size);

// Runs once (first call does the work): EXE name + hash, manifest load, prefix checks.
const Result& run(const std::filesystem::path& manifest_path);
const Result& current(); // run() must have been called

// Address of a verified manifest function; nullptr when preflight failed or the name is unknown.
void* address(std::string_view function_name);

} // namespace elmp::preflight

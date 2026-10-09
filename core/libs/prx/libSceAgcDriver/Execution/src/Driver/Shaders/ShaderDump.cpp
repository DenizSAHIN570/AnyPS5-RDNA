#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderDump.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderDumpFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace AgcDriver::DriverDetail {

namespace {

std::uint64_t codeHash(std::span<const std::uint32_t> code) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word : code) {
        for (std::uint32_t shift = 0; shift < 32; shift += 8) {
            hash ^= (word >> shift) & 0xffu;
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}

ShaderDumpFormat::RegisterList pairsOf(const Registers& registers) {
    ShaderDumpFormat::RegisterList pairs;
    pairs.reserve(registers.size());
    for (const auto [offset, value] : registers) pairs.emplace_back(offset, value);
    return pairs;
}

const std::filesystem::path& dumpDirectory() {
    static const std::filesystem::path directory = [] {
        const char* text = std::getenv("APS5_DUMP_REGISTERED_SHADERS");
        if (text == nullptr) return std::filesystem::path{};
        if (*text == '\0') throw std::runtime_error("APS5_DUMP_REGISTERED_SHADERS is set but empty; it names the dump directory");
        std::filesystem::path path(text);
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (!std::filesystem::is_directory(path)) throw std::runtime_error("APS5_DUMP_REGISTERED_SHADERS: cannot create directory " + path.string());
        return path;
    }();
    return directory;
}

}

void DumpRegisteredShader(const ShaderSnapshot& snapshot) {
    const auto& directory = dumpDirectory();
    if (directory.empty()) return;
    if (snapshot.registeredState == nullptr) throw std::runtime_error("shader dump: registered state is missing");
    const ShaderDumpFormat::Registers registers{pairsOf(snapshot.registeredState->shader), pairsOf(snapshot.registeredState->context), pairsOf(snapshot.registeredState->userConfig)};
    const auto bytes = ShaderDumpFormat::Serialize(snapshot.type, snapshot.codeAddress, snapshot.headerAddress, snapshot.code, snapshot.header, registers);
    char name[96];
    std::snprintf(name, sizeof(name), "shader_%llx_t%u_%016llx.ash", static_cast<unsigned long long>(snapshot.codeAddress), static_cast<unsigned>(snapshot.type), static_cast<unsigned long long>(codeHash(snapshot.code)));
    const auto path = directory / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("APS5_DUMP_REGISTERED_SHADERS: cannot open " + path.string());
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("APS5_DUMP_REGISTERED_SHADERS: cannot write " + path.string());
}

}

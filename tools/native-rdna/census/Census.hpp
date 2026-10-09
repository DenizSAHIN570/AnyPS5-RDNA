#ifndef TOOLS_NATIVE_RDNA_CENSUS_CENSUS_HPP
#define TOOLS_NATIVE_RDNA_CENSUS_CENSUS_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ShaderCensus {

struct ShaderDump {
    std::uint32_t type = 0;
    std::uint64_t codeAddress = 0;
    std::uint64_t headerAddress = 0;
    std::vector<std::uint32_t> code;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> shaderRegisters;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> contextRegisters;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> userConfigRegisters;
    std::vector<std::byte> header;
};

ShaderDump ParseShaderDump(std::span<const std::byte> bytes);

struct OpcodeCount {
    std::string family;
    std::uint64_t count = 0;
    std::uint64_t shaders = 0;
};

struct ShaderSummary {
    std::string name;
    std::uint32_t type = 0;
    std::uint64_t codeAddress = 0;
    std::uint64_t entryOffsetBytes = 0;
    std::size_t codeWords = 0;
    std::size_t instructions = 0;
    std::size_t unsupported = 0;
    std::string decodeError;
    std::optional<std::uint32_t> programResource1;
    std::optional<std::uint32_t> programResource2;
};

struct Census {
    std::vector<ShaderSummary> shaders;
    std::map<std::uint32_t, OpcodeCount> opcodes;
    std::map<std::string, std::uint64_t> families;
    std::map<std::string, std::uint64_t> features;
    std::map<std::uint32_t, std::uint64_t> exportTargets;
    std::map<std::string, std::uint64_t> unsupportedReasons;
};

void AddShader(Census& census, const std::string& name, const ShaderDump& dump);

std::string ToJson(const Census& census);

}

#endif

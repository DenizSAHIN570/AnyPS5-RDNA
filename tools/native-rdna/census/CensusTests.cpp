#include "Census.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderDumpFormat.hpp"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::abort();
    }
}

constexpr std::uint64_t CodeAddress = 0x100000;

const std::vector<std::uint32_t> Program = {
    0xbe801f00u,
    0x7e0202f9u, 0x00040502u,
    0x060206fau, 0xff00b102u,
    0x7e0802ffu, 0x12345678u,
    0xd5400005u, 0x040e0501u,
    0xbf8c0070u,
    0xf800180fu, 0x05040100u,
    0xbf810000u,
};

AgcDriver::ShaderDumpFormat::Registers pixelRegisters(std::uint64_t entry) {
    return {{{0x008, static_cast<std::uint32_t>(entry >> 8)}, {0x009, static_cast<std::uint32_t>(entry >> 40)}, {0x00a, 0x002c0041u}, {0x00b, 0x00000010u}}, {{0x1b3, 1}}, {}};
}

std::uint64_t value(const std::map<std::string, std::uint64_t>& map, const char* key) {
    const auto found = map.find(key);
    return found == map.end() ? 0 : found->second;
}

void roundTrip() {
    const std::vector<std::byte> header = {std::byte{1}, std::byte{2}, std::byte{3}};
    const auto bytes = AgcDriver::ShaderDumpFormat::Serialize(1, CodeAddress, 0x2000, Program, header, pixelRegisters(CodeAddress));
    const auto dump = ShaderCensus::ParseShaderDump(bytes);
    Require(dump.type == 1 && dump.codeAddress == CodeAddress && dump.headerAddress == 0x2000, "fixed fields survive a round trip");
    Require(dump.code == Program, "code words survive a round trip");
    Require(dump.shaderRegisters.size() == 4 && dump.contextRegisters.size() == 1 && dump.userConfigRegisters.empty(), "register lists survive a round trip");
    Require(dump.header == header, "header bytes survive a round trip");

    auto truncated = bytes;
    truncated.pop_back();
    bool threw = false;
    try {
        static_cast<void>(ShaderCensus::ParseShaderDump(truncated));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw, "a truncated dump is rejected");

    auto wrongMagic = bytes;
    wrongMagic[0] = std::byte{0};
    threw = false;
    try {
        static_cast<void>(ShaderCensus::ParseShaderDump(wrongMagic));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw, "a dump with another magic is rejected");
}

void counts() {
    ShaderCensus::Census census;
    const auto bytes = AgcDriver::ShaderDumpFormat::Serialize(1, CodeAddress, 0x2000, Program, {}, pixelRegisters(CodeAddress));
    ShaderCensus::AddShader(census, "pixel.ash", ShaderCensus::ParseShaderDump(bytes));
    Require(census.shaders.size() == 1, "one shader is counted");
    const auto& shader = census.shaders.front();
    Require(shader.decodeError.empty(), "the program decodes");
    Require(shader.instructions == 8, "every instruction is decoded");
    Require(shader.programResource1 == 0x002c0041u && shader.programResource2 == 0x00000010u, "the stage's resource registers are recorded");
    Require(value(census.features, "sdwa") == 1, "the SDWA move is counted");
    Require(value(census.features, "dpp16") == 1, "the DPP add is counted");
    Require(value(census.features, "literal_constant") == 1, "the literal is counted");
    Require(value(census.features, "s_getpc_b64") == 1, "s_getpc_b64 is counted");
    Require(value(census.features, "s_waitcnt_any") == 1, "s_waitcnt is counted");
    Require(census.exportTargets.size() == 1 && census.exportTargets.begin()->first == 0 && census.exportTargets.begin()->second == 1, "the mrt0 export is counted");
    Require(value(census.families, "SOPP") == 2 && value(census.families, "VOP1") == 2 && value(census.families, "EXP") == 1, "families are counted");
    std::uint64_t total = 0;
    for (const auto& [opcode, entry] : census.opcodes) total += entry.count;
    Require(total == 8, "opcode counts add up to the instruction count");
    const auto json = ShaderCensus::ToJson(census);
    Require(json.find("\"pixel.ash\"") != std::string::npos && json.find("\"sdwa\": 1") != std::string::npos, "the JSON report names the shader and the features");
}

void entryOffset() {
    std::vector<std::uint32_t> code(64, 0xbf800000u);
    code.push_back(0xbf810000u);
    ShaderCensus::Census census;
    const auto entry = CodeAddress + 256;
    const auto bytes = AgcDriver::ShaderDumpFormat::Serialize(1, CodeAddress, 0x2000, code, {}, pixelRegisters(entry));
    ShaderCensus::AddShader(census, "offset.ash", ShaderCensus::ParseShaderDump(bytes));
    const auto& shader = census.shaders.front();
    Require(shader.decodeError.empty() && shader.entryOffsetBytes == 256 && shader.instructions == 1, "decoding starts at the registered entry point");

    ShaderCensus::Census outside;
    const auto outsideBytes = AgcDriver::ShaderDumpFormat::Serialize(1, CodeAddress, 0x2000, code, {}, pixelRegisters(CodeAddress + 0x10000));
    ShaderCensus::AddShader(outside, "outside.ash", ShaderCensus::ParseShaderDump(outsideBytes));
    Require(!outside.shaders.front().decodeError.empty(), "an entry point outside the code is reported");
}

}

int main() {
    roundTrip();
    counts();
    entryOffset();
    std::printf("PASS: shader census\n");
    return 0;
}

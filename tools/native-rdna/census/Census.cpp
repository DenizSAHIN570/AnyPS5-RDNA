#include "Census.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderDumpFormat.hpp"
#include <cstdio>
#include <cstring>
#include <exception>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace ShaderCensus {

namespace {

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes(bytes) {}

    std::uint32_t U32() {
        std::uint32_t value = 0;
        read(&value, sizeof(value));
        return value;
    }

    std::uint64_t U64() {
        std::uint64_t value = 0;
        read(&value, sizeof(value));
        return value;
    }

    std::vector<std::byte> Bytes(std::size_t count) {
        need(count);
        std::vector<std::byte> out(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
        return out;
    }

    std::size_t Remaining() const { return bytes.size() - offset; }

private:
    void need(std::size_t count) const {
        if (count > bytes.size() - offset) throw std::runtime_error("shader dump is truncated");
    }

    void read(void* out, std::size_t count) {
        need(count);
        std::memcpy(out, bytes.data() + offset, count);
        offset += count;
    }

    std::span<const std::byte> bytes;
    std::size_t offset = 0;
};

std::vector<std::pair<std::uint32_t, std::uint32_t>> readPairs(Reader& reader, std::uint32_t count) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
    pairs.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto offset = reader.U32();
        const auto value = reader.U32();
        pairs.emplace_back(offset, value);
    }
    return pairs;
}

std::optional<std::uint32_t> registerValue(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& registers, std::uint32_t offset) {
    for (const auto& [key, value] : registers)
        if (key == offset) return value;
    return std::nullopt;
}

struct StageRegisters {
    std::uint32_t program;
    std::uint32_t resource;
};

std::optional<StageRegisters> stageRegisters(std::uint32_t type) {
    switch (type) {
    case 0: return StageRegisters{0x20c, 0x213};
    case 1: return StageRegisters{0x008, 0x00b};
    case 2: return StageRegisters{0x0c8, 0x08b};
    case 4: return StageRegisters{0x0c8, 0x08b};
    case 5: return StageRegisters{0x148, 0x10b};
    case 6: return StageRegisters{0x088, 0x08b};
    case 7: return StageRegisters{0x108, 0x10b};
    default: return std::nullopt;
    }
}

const char* familyName(ShaderRecompiler::RdnaInstructionFamily family) {
    using enum ShaderRecompiler::RdnaInstructionFamily;
    switch (family) {
    case Unknown: return "Unknown";
    case SOP1: return "SOP1";
    case SOP2: return "SOP2";
    case SOPK: return "SOPK";
    case SOPC: return "SOPC";
    case SOPP: return "SOPP";
    case VOP1: return "VOP1";
    case VOP2: return "VOP2";
    case VOP3: return "VOP3";
    case VOP3P: return "VOP3P";
    case VOPC: return "VOPC";
    case VINTRP: return "VINTRP";
    case SMEM: return "SMEM";
    case MUBUF: return "MUBUF";
    case MTBUF: return "MTBUF";
    case FLAT: return "FLAT";
    case DS: return "DS";
    case MIMG: return "MIMG";
    case EXP: return "EXP";
    }
    throw std::runtime_error("unknown RDNA instruction family");
}

std::optional<std::uint32_t> baseWords(const ShaderRecompiler::RdnaInstruction& instruction) {
    using ShaderRecompiler::RdnaInstructionFamily;
    switch (instruction.family) {
    case RdnaInstructionFamily::SOP1:
    case RdnaInstructionFamily::SOP2:
    case RdnaInstructionFamily::SOPK:
    case RdnaInstructionFamily::SOPC:
    case RdnaInstructionFamily::SOPP:
        return 1;
    case RdnaInstructionFamily::VOP1:
    case RdnaInstructionFamily::VOP2:
    case RdnaInstructionFamily::VOPC: {
        const auto source = instruction.rawWords[0] & 0x1ffu;
        return source == 0xf9 || source == 0xfa || source == 0xe9 || source == 0xea ? 2 : 1;
    }
    case RdnaInstructionFamily::VOP3:
    case RdnaInstructionFamily::VOP3P:
        return 2;
    default:
        return std::nullopt;
    }
}

bool hasLiteral(const ShaderRecompiler::RdnaInstruction& instruction) {
    const auto base = baseWords(instruction);
    return base && instruction.wordCount > *base;
}

void countFeatures(Census& census, const ShaderRecompiler::RdnaInstruction& instruction) {
    using ShaderRecompiler::RdnaInstructionFamily;
    using ShaderRecompiler::RdnaOpcode;
    auto& features = census.features;
    const auto family = instruction.family;
    if (family == RdnaInstructionFamily::VOP1 || family == RdnaInstructionFamily::VOP2 || family == RdnaInstructionFamily::VOPC) {
        switch (instruction.rawWords[0] & 0x1ffu) {
        case 0xf9: ++features["sdwa"]; break;
        case 0xfa: ++features["dpp16"]; break;
        case 0xe9: ++features["dpp8"]; break;
        case 0xea: ++features["dpp8_fetch_inactive"]; break;
        default: break;
        }
    }
    if (hasLiteral(instruction)) ++features["literal_constant"];
    if (family == RdnaInstructionFamily::VINTRP) ++features["vintrp"];
    if (family == RdnaInstructionFamily::EXP) ++census.exportTargets[instruction.exportTarget];
    if (family == RdnaInstructionFamily::MUBUF || family == RdnaInstructionFamily::MTBUF) {
        if (instruction.glc) ++features["buffer_glc"];
        if (instruction.slc) ++features["buffer_slc"];
        if (instruction.dlc) ++features["buffer_dlc"];
    }
    if (family == RdnaInstructionFamily::MIMG && instruction.imageNsaDwordCount != 0) ++features["mimg_nsa"];
    switch (instruction.op) {
    case RdnaOpcode::SGetpcB64: ++features["s_getpc_b64"]; break;
    case RdnaOpcode::SSetpcB64: ++features["s_setpc_b64"]; break;
    case RdnaOpcode::SSwappcB64: ++features["s_swappc_b64"]; break;
    case RdnaOpcode::SCallB64: ++features["s_call_b64"]; break;
    case RdnaOpcode::SWaitcnt: ++features["s_waitcnt_any"]; break;
    case RdnaOpcode::SWaitcntDepctr: ++features["s_waitcnt_depctr"]; break;
    case RdnaOpcode::SSetregB32:
    case RdnaOpcode::SSetregImm32B32: ++features["s_setreg"]; break;
    case RdnaOpcode::SGetregB32: ++features["s_getreg"]; break;
    case RdnaOpcode::SDcacheDiscard:
    case RdnaOpcode::SDcacheDiscardX2: ++features["s_dcache_discard"]; break;
    case RdnaOpcode::VMadLegacyF32: ++features["v_mad_legacy_f32"]; break;
    case RdnaOpcode::VMacLegacyF32: ++features["v_mac_legacy_f32"]; break;
    case RdnaOpcode::VMulLoI32: ++features["v_mul_lo_i32"]; break;
    default: break;
    }
}

void appendEscaped(std::string& out, const std::string& text) {
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
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

std::string hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "\"0x%llx\"", static_cast<unsigned long long>(value));
    return text;
}

}

ShaderDump ParseShaderDump(std::span<const std::byte> bytes) {
    if (bytes.size() < AgcDriver::ShaderDumpFormat::FixedBytes) throw std::runtime_error("shader dump is smaller than its fixed header");
    Reader reader(bytes);
    if (reader.U32() != AgcDriver::ShaderDumpFormat::Magic) throw std::runtime_error("not a shader dump (magic mismatch)");
    if (const auto version = reader.U32(); version != AgcDriver::ShaderDumpFormat::Version) throw std::runtime_error("unsupported shader dump version " + std::to_string(version));
    ShaderDump dump;
    dump.type = reader.U32();
    const auto headerBytes = reader.U32();
    dump.codeAddress = reader.U64();
    dump.headerAddress = reader.U64();
    const auto codeWords = reader.U32();
    const auto shaderCount = reader.U32();
    const auto contextCount = reader.U32();
    const auto userConfigCount = reader.U32();
    dump.code.resize(codeWords);
    for (auto& word : dump.code) word = reader.U32();
    dump.shaderRegisters = readPairs(reader, shaderCount);
    dump.contextRegisters = readPairs(reader, contextCount);
    dump.userConfigRegisters = readPairs(reader, userConfigCount);
    dump.header = reader.Bytes(headerBytes);
    if (reader.Remaining() != 0) throw std::runtime_error("shader dump has trailing bytes");
    return dump;
}

void AddShader(Census& census, const std::string& name, const ShaderDump& dump) {
    ShaderSummary summary;
    summary.name = name;
    summary.type = dump.type;
    summary.codeAddress = dump.codeAddress;
    summary.codeWords = dump.code.size();
    std::size_t entryWord = 0;
    if (const auto stage = stageRegisters(dump.type)) {
        const auto low = registerValue(dump.shaderRegisters, stage->program);
        const auto high = registerValue(dump.shaderRegisters, stage->program + 1);
        if (low && high) {
            const auto entry = ((static_cast<std::uint64_t>(*high) << 32) | *low) << 8;
            if (entry < dump.codeAddress || entry - dump.codeAddress >= dump.code.size() * 4ull) {
                summary.decodeError = "entry point is outside the shader code";
            } else {
                summary.entryOffsetBytes = entry - dump.codeAddress;
                entryWord = static_cast<std::size_t>(summary.entryOffsetBytes / 4);
            }
        }
        summary.programResource1 = registerValue(dump.shaderRegisters, stage->resource - 1);
        summary.programResource2 = registerValue(dump.shaderRegisters, stage->resource);
    }
    if (summary.decodeError.empty()) {
        try {
            const auto code = std::span(dump.code).subspan(entryWord);
            const auto program = ShaderRecompiler::RdnaInstructionDecoder{}.Decode(code);
            summary.instructions = program.instructions.size();
            std::set<std::uint32_t> seen;
            for (const auto& instruction : program.instructions) {
                const auto family = familyName(instruction.family);
                ++census.families[family];
                if (!instruction.unsupportedReason.empty()) {
                    ++summary.unsupported;
                    ++census.unsupportedReasons[std::string(instruction.unsupportedReason)];
                }
                const auto value = static_cast<std::uint32_t>(instruction.op);
                auto& entry = census.opcodes[value];
                entry.family = family;
                ++entry.count;
                if (seen.insert(value).second) ++entry.shaders;
                countFeatures(census, instruction);
            }
        } catch (const std::exception& error) {
            summary.decodeError = error.what();
        }
    }
    census.shaders.push_back(std::move(summary));
}

std::string ToJson(const Census& census) {
    std::string out = "{\n  \"version\": 1,\n  \"shaders\": [";
    for (std::size_t i = 0; i < census.shaders.size(); ++i) {
        const auto& shader = census.shaders[i];
        out += i == 0 ? "\n    {" : ",\n    {";
        out += "\"name\": ";
        appendEscaped(out, shader.name);
        out += ", \"type\": " + std::to_string(shader.type);
        out += ", \"codeAddress\": " + hex(shader.codeAddress);
        out += ", \"entryOffsetBytes\": " + std::to_string(shader.entryOffsetBytes);
        out += ", \"codeWords\": " + std::to_string(shader.codeWords);
        out += ", \"instructions\": " + std::to_string(shader.instructions);
        out += ", \"unsupported\": " + std::to_string(shader.unsupported);
        out += ", \"programResource1\": " + (shader.programResource1 ? hex(*shader.programResource1) : std::string("null"));
        out += ", \"programResource2\": " + (shader.programResource2 ? hex(*shader.programResource2) : std::string("null"));
        out += ", \"decodeError\": ";
        if (shader.decodeError.empty()) out += "null";
        else appendEscaped(out, shader.decodeError);
        out += "}";
    }
    out += "\n  ],\n  \"opcodes\": [";
    bool first = true;
    for (const auto& [value, entry] : census.opcodes) {
        out += first ? "\n    " : ",\n    ";
        first = false;
        out += "{\"value\": " + std::to_string(value) + ", \"family\": ";
        appendEscaped(out, entry.family);
        out += ", \"count\": " + std::to_string(entry.count) + ", \"shaders\": " + std::to_string(entry.shaders) + "}";
    }
    const auto writeMap = [&out](const char* key, const auto& map) {
        out += "\n  \"";
        out += key;
        out += "\": {";
        bool firstEntry = true;
        for (const auto& [name, count] : map) {
            out += firstEntry ? "" : ", ";
            firstEntry = false;
            if constexpr (std::is_same_v<std::decay_t<decltype(name)>, std::string>) appendEscaped(out, name);
            else appendEscaped(out, std::to_string(name));
            out += ": " + std::to_string(count);
        }
        out += "}";
    };
    out += "\n  ],";
    writeMap("families", census.families);
    out += ",";
    writeMap("features", census.features);
    out += ",";
    writeMap("exportTargets", census.exportTargets);
    out += ",";
    writeMap("unsupportedReasons", census.unsupportedReasons);
    out += "\n}\n";
    return out;
}

}

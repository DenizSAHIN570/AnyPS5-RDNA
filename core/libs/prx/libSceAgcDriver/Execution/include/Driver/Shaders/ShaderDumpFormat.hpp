#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_SHADERDUMPFORMAT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_SHADERDUMPFORMAT_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::ShaderDumpFormat {

inline constexpr std::uint32_t Magic = 0x44485341u;
inline constexpr std::uint32_t Version = 1;
inline constexpr std::size_t FixedBytes = 48;

using RegisterList = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

struct Registers {
    RegisterList shader;
    RegisterList context;
    RegisterList userConfig;
};

namespace Detail {

inline void Append(std::vector<std::byte>& bytes, const void* value, std::size_t size) {
    const auto offset = bytes.size();
    bytes.resize(offset + size);
    std::memcpy(bytes.data() + offset, value, size);
}

inline void AppendU32(std::vector<std::byte>& bytes, std::uint32_t value) { Append(bytes, &value, sizeof(value)); }

inline void AppendU64(std::vector<std::byte>& bytes, std::uint64_t value) { Append(bytes, &value, sizeof(value)); }

inline std::uint32_t Count32(std::size_t value, const char* what) {
    if (value > 0xffffffffu) throw std::runtime_error(std::string("shader dump: too many ") + what);
    return static_cast<std::uint32_t>(value);
}

}

inline std::vector<std::byte> Serialize(std::uint8_t type, std::uint64_t codeAddress, std::uint64_t headerAddress, std::span<const std::uint32_t> code, std::span<const std::byte> header, const Registers& registers) {
    std::vector<std::byte> bytes;
    bytes.reserve(FixedBytes + code.size_bytes() + header.size() + 8 * (registers.shader.size() + registers.context.size() + registers.userConfig.size()));
    Detail::AppendU32(bytes, Magic);
    Detail::AppendU32(bytes, Version);
    Detail::AppendU32(bytes, type);
    Detail::AppendU32(bytes, Detail::Count32(header.size(), "header bytes"));
    Detail::AppendU64(bytes, codeAddress);
    Detail::AppendU64(bytes, headerAddress);
    Detail::AppendU32(bytes, Detail::Count32(code.size(), "code words"));
    Detail::AppendU32(bytes, Detail::Count32(registers.shader.size(), "shader registers"));
    Detail::AppendU32(bytes, Detail::Count32(registers.context.size(), "context registers"));
    Detail::AppendU32(bytes, Detail::Count32(registers.userConfig.size(), "user config registers"));
    for (const auto word : code) Detail::AppendU32(bytes, word);
    for (const auto* list : {&registers.shader, &registers.context, &registers.userConfig}) {
        for (const auto& [offset, value] : *list) {
            Detail::AppendU32(bytes, offset);
            Detail::AppendU32(bytes, value);
        }
    }
    bytes.insert(bytes.end(), header.begin(), header.end());
    return bytes;
}

}

#endif

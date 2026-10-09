#ifndef TOOLS_NATIVE_RDNA_DRM_DISPATCH_PM4_HPP
#define TOOLS_NATIVE_RDNA_DRM_DISPATCH_PM4_HPP

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace Gfx12Pm4 {

inline constexpr std::uint32_t OpNop = 0x10;
inline constexpr std::uint32_t OpDispatchDirect = 0x15;
inline constexpr std::uint32_t OpContextControl = 0x28;
inline constexpr std::uint32_t OpSetShReg = 0x76;
inline constexpr std::uint32_t NopPad = 0xffff1000u;
inline constexpr std::uint32_t ShRegBase = 0xb000;
inline constexpr std::uint32_t ShRegEnd = 0xc000;

inline constexpr std::uint32_t ComputeStartX = 0xb810;
inline constexpr std::uint32_t ComputeNumThreadX = 0xb81c;
inline constexpr std::uint32_t ComputePerfcountEnable = 0xb82c;
inline constexpr std::uint32_t ComputePgmLo = 0xb830;
inline constexpr std::uint32_t ComputeDispatchPktAddrLo = 0xb838;
inline constexpr std::uint32_t ComputePgmRsrc1 = 0xb848;
inline constexpr std::uint32_t ComputeResourceLimits = 0xb854;
inline constexpr std::uint32_t ComputeStaticThreadMgmtSe0 = 0xb858;
inline constexpr std::uint32_t ComputeTmpringSize = 0xb860;
inline constexpr std::uint32_t ComputeStaticThreadMgmtSe2 = 0xb864;
inline constexpr std::uint32_t ComputeStaticThreadMgmtSe8 = 0xb88c;
inline constexpr std::uint32_t ComputeUserAccum0 = 0xb890;
inline constexpr std::uint32_t ComputePgmRsrc3 = 0xb8a0;
inline constexpr std::uint32_t ComputeStaticThreadMgmtSe4 = 0xb8ac;
inline constexpr std::uint32_t ComputeUserData0 = 0xb900;
inline constexpr std::uint32_t ComputeDispatchTunnel = 0xb9f4;

inline constexpr std::uint32_t InitiatorComputeShaderEn = 1u << 0;
inline constexpr std::uint32_t InitiatorCsW32En = 1u << 15;

inline constexpr std::uint32_t Header(std::uint32_t opcode, std::uint32_t payloadDwords, bool computeShader) {
    return (3u << 30) | (((payloadDwords - 1u) & 0x3fffu) << 16) | ((opcode & 0xffu) << 8) | (computeShader ? 2u : 0u);
}

struct ComputeProgram {
    std::uint64_t codeAddress = 0;
    std::uint32_t rsrc1 = 0;
    std::uint32_t rsrc2 = 0;
    std::uint32_t rsrc3 = 0;
    std::uint32_t threadsX = 64;
    std::uint32_t threadsY = 1;
    std::uint32_t threadsZ = 1;
    std::vector<std::uint32_t> userData;
    bool wave32 = true;
};

class Builder {
public:
    void ContextControl() {
        words.push_back(Header(OpContextControl, 2, false));
        words.push_back(0x80000000u);
        words.push_back(0x80000000u);
    }

    void SetShRegs(std::uint32_t reg, std::initializer_list<std::uint32_t> values) { SetShRegs(reg, std::vector<std::uint32_t>(values)); }

    void SetShRegs(std::uint32_t reg, const std::vector<std::uint32_t>& values) {
        if (reg < ShRegBase || reg >= ShRegEnd || (reg & 3u) != 0) throw std::invalid_argument("SET_SH_REG: register outside the SH space");
        if (values.empty() || reg + 4u * values.size() > ShRegEnd) throw std::invalid_argument("SET_SH_REG: bad value count");
        words.push_back(Header(OpSetShReg, static_cast<std::uint32_t>(values.size()) + 1u, true));
        words.push_back((reg - ShRegBase) >> 2);
        words.insert(words.end(), values.begin(), values.end());
    }

    void ComputePreamble() {
        SetShRegs(ComputeStartX, {0, 0, 0});
        SetShRegs(ComputePerfcountEnable, {0});
        SetShRegs(ComputeDispatchPktAddrLo, {0, 0});
        SetShRegs(ComputeResourceLimits, {0});
        SetShRegs(ComputeStaticThreadMgmtSe0, {0xffffffffu, 0xffffffffu});
        SetShRegs(ComputeTmpringSize, {0});
        SetShRegs(ComputeStaticThreadMgmtSe2, {0xffffffffu, 0xffffffffu});
        SetShRegs(ComputeStaticThreadMgmtSe8, {0xffffffffu});
        SetShRegs(ComputeUserAccum0, {0, 0, 0, 0});
        SetShRegs(ComputeStaticThreadMgmtSe4, {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu});
        SetShRegs(ComputeDispatchTunnel, {0});
    }

    void Program(const ComputeProgram& program) {
        if ((program.codeAddress & 0xffu) != 0) throw std::invalid_argument("compute program address must be 256-byte aligned");
        if (program.userData.size() > 16) throw std::invalid_argument("at most 16 compute user data registers");
        SetShRegs(ComputePgmLo, {static_cast<std::uint32_t>(program.codeAddress >> 8), static_cast<std::uint32_t>(program.codeAddress >> 40)});
        SetShRegs(ComputePgmRsrc1, {program.rsrc1, program.rsrc2});
        SetShRegs(ComputePgmRsrc3, {program.rsrc3});
        SetShRegs(ComputeNumThreadX, {program.threadsX, program.threadsY, program.threadsZ});
        if (!program.userData.empty()) SetShRegs(ComputeUserData0, program.userData);
    }

    void DispatchDirect(std::uint32_t x, std::uint32_t y, std::uint32_t z, bool wave32) {
        words.push_back(Header(OpDispatchDirect, 4, true));
        words.push_back(x);
        words.push_back(y);
        words.push_back(z);
        words.push_back(InitiatorComputeShaderEn | (wave32 ? InitiatorCsW32En : 0u));
    }

    void PadTo(std::uint32_t multiple) {
        while (words.size() % multiple != 0) words.push_back(NopPad);
    }

    const std::vector<std::uint32_t>& Words() const { return words; }

private:
    std::vector<std::uint32_t> words;
};

}

#endif

#include "Pm4.hpp"
#include "KernelBinary.hpp"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {

void Require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::abort();
    }
}

template <typename TFunction>
bool Throws(TFunction function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

}

int main() {
    Require(Gfx12Pm4::Header(Gfx12Pm4::OpSetShReg, 3, true) == 0xc0027602u, "SET_SH_REG header with two values, compute shader type");
    Require(Gfx12Pm4::Header(Gfx12Pm4::OpDispatchDirect, 4, true) == 0xc0031502u, "DISPATCH_DIRECT header");
    Require(Gfx12Pm4::Header(Gfx12Pm4::OpContextControl, 2, false) == 0xc0012800u, "CONTEXT_CONTROL header");

    Gfx12Pm4::Builder builder;
    builder.SetShRegs(Gfx12Pm4::ComputePgmLo, {0x12345678u, 0x9au});
    const auto& words = builder.Words();
    Require(words.size() == 4 && words[1] == 0x20c && words[2] == 0x12345678u && words[3] == 0x9au, "COMPUTE_PGM_LO is SH register 0x20c");

    Gfx12Pm4::Builder full;
    full.ComputePreamble();
    Gfx12Pm4::ComputeProgram program;
    program.codeAddress = 0x1000'0000'0100ull;
    program.rsrc1 = 0x400c0041u;
    program.rsrc2 = 0x84u;
    program.userData = {0xdead0000u, 0x1000u};
    full.Program(program);
    full.DispatchDirect(4, 1, 1, true);
    full.PadTo(8);
    const auto& ib = full.Words();
    Require(ib.size() % 8 == 0, "the IB is padded to 8 dwords");
    std::size_t dispatch = 0;
    bool foundProgram = false;
    for (std::size_t i = 0; i < ib.size();) {
        const auto header = ib[i];
        Require((header >> 30) == 3u || header == Gfx12Pm4::NopPad, "every packet is type 3");
        if (header == Gfx12Pm4::NopPad) {
            ++i;
            continue;
        }
        const auto opcode = (header >> 8) & 0xffu;
        const auto payload = ((header >> 16) & 0x3fffu) + 1u;
        if (opcode == Gfx12Pm4::OpSetShReg && ib[i + 1] == 0x20c) {
            foundProgram = ib[i + 2] == 0x00000001u && ib[i + 3] == 0x10u;
        }
        if (opcode == Gfx12Pm4::OpDispatchDirect) {
            dispatch = i;
            Require(ib[i + 1] == 4 && ib[i + 4] == 0x8001u, "dispatch dims and initiator with COMPUTE_SHADER_EN and CS_W32_EN");
        }
        i += 1 + payload;
    }
    Require(foundProgram, "the program address is split into PGM_LO (va bits 39:8) and PGM_HI (va bits 47:40)");
    Require(dispatch != 0, "the IB dispatches");

    Require(Throws([] { Gfx12Pm4::Builder b; b.SetShRegs(0x28000, {1}); }), "context registers are rejected by SET_SH_REG");
    Require(Throws([] { Gfx12Pm4::Builder b; Gfx12Pm4::ComputeProgram p; p.codeAddress = 0x1010; b.Program(p); }), "a misaligned program is rejected");

    Require(KernelBinary.size() >= 6 && KernelBinary.back() == 0xbfb00000u, "the gfx1201 kernel ends with s_endpgm");
    std::printf("PASS: gfx12 PM4 builder and kernel binary\n");
    return 0;
}

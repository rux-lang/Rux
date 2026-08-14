#include "CodeGen/X86_64/RuntimeHelpers.h"

#include "CodeGen/X86_64/Encoder.h"

#include <initializer_list>

namespace Rux {
X86_64RuntimeHelperEmitter::X86_64RuntimeHelperEmitter(RcuModuleBuilder &inputModuleBuilder,
                                                       const CallingConvention inputDefaultConvention)
    : moduleBuilder(inputModuleBuilder)
    , defaultConvention(inputDefaultConvention) {
}

std::uint32_t X86_64RuntimeHelperEmitter::Declare(const char *name) {
    return moduleBuilder
        .DeclareSymbol({.name = name, .typeName = {}, .kind = RcuSymKind::Func, .visibility = RcuSymVis::Local})
        .value_or(NoSymbol);
}

std::uint32_t X86_64RuntimeHelperEmitter::Require(const X86_64RuntimeHelper helper) {
    switch (helper) {
    case X86_64RuntimeHelper::IntegerPower:
        if (integerPowerSymbol == NoSymbol) {
            integerPowerSymbol = Declare("__rux_ipow");
        }
        return integerPowerSymbol;
    case X86_64RuntimeHelper::FloatPower64:
        if (floatPower64Symbol == NoSymbol) {
            floatPower64Symbol = Declare("__rux_powf64");
        }
        return floatPower64Symbol;
    case X86_64RuntimeHelper::FloatPower32:
        (void)Require(X86_64RuntimeHelper::FloatPower64);
        if (floatPower32Symbol == NoSymbol) {
            floatPower32Symbol = Declare("__rux_powf32");
        }
        return floatPower32Symbol;
    }
    return NoSymbol;
}

void X86_64RuntimeHelperEmitter::AddCallRelocation(const std::uint32_t sectionOffset,
                                                   const X86_64RuntimeHelper helper) {
    const std::uint32_t symbol = Require(helper);
    if (symbol != NoSymbol) {
        (void)moduleBuilder.AddRelocation(RcuModuleSection::Text, sectionOffset, symbol, RcuRelType::Rel32);
    }
}

void X86_64RuntimeHelperEmitter::EmitRequested() {
    EmitIntegerPower();
    EmitFloatPower64();
    EmitFloatPower32();
}

void X86_64RuntimeHelperEmitter::EmitIntegerPower() {
    if (integerPowerSymbol == NoSymbol || !moduleBuilder.BeginFunction(integerPowerSymbol)) {
        return;
    }

    X64Enc enc(moduleBuilder.SectionData(RcuModuleSection::Text));
    if (defaultConvention == CallingConvention::SysV) {
        // Keep the compact helper body below in its historical rcx/rdx form
        // after accepting native System V rdi/rsi arguments.
        enc.Byte(0x48);
        enc.Byte(0x89);
        enc.Byte(0xF9); // mov rcx, rdi
        enc.Byte(0x48);
        enc.Byte(0x89);
        enc.Byte(0xF2); // mov rdx, rsi
    }
    // clang-format off
    static constexpr std::uint8_t Body[] = {
        0x48, 0x85, 0xD2,                         // test rdx, rdx    ; exponent
        0x78, 0x20,                               // js   .negative   ; exp < 0 -> 0
        0xB8, 0x01, 0x00, 0x00, 0x00,             // mov  eax, 1      ; result = 1
        0x48, 0x85, 0xD2,                         // test rdx, rdx
        0x74, 0x18,                               // jz   .done       ; exp == 0
        0x48, 0xF7, 0xC2, 0x01, 0x00, 0x00, 0x00, // test rdx, 1
        0x74, 0x04,                               // jz   .square
        0x48, 0x0F, 0xAF, 0xC1,                   // imul rax, rcx    ; result *= base
        0x48, 0x0F, 0xAF, 0xC9,                   // imul rcx, rcx    ; base *= base
        0x48, 0xD1, 0xFA,                         // sar  rdx, 1      ; exp >>= 1
        0xEB, 0xE5,                               // jmp  .loop
        0x31, 0xC0,                               // xor  eax, eax    ; result = 0
        0xC3,                                     // ret
    };
    // clang-format on
    for (const std::uint8_t byte : Body) {
        enc.Byte(byte);
    }
    (void)moduleBuilder.EndFunction(integerPowerSymbol);
}

void X86_64RuntimeHelperEmitter::EmitFloatPower64() {
    if (floatPower64Symbol == NoSymbol || !moduleBuilder.BeginFunction(floatPower64Symbol)) {
        return;
    }

    X64Enc enc(moduleBuilder.SectionData(RcuModuleSection::Text));
    const auto bytes = [&](const std::initializer_list<std::uint8_t> values) {
        for (const std::uint8_t value : values) {
            enc.Byte(value);
        }
    };
    const auto jump = [&](const std::uint8_t condition) {
        enc.Byte(0x0F);
        enc.Byte(condition);
        const std::uint32_t offset = enc.Size();
        enc.Dword(0);
        return offset;
    };
    const auto patch = [&](const std::uint32_t offset) {
        enc.Patch32(offset, static_cast<std::int32_t>(enc.Size()) - static_cast<std::int32_t>(offset + 4));
    };

    // clang-format off
    bytes({0x48, 0x83, 0xEC, 0x10});             // sub  rsp, 16
    bytes({0xF2, 0x0F, 0x11, 0x04, 0x24});       // movsd [rsp], xmm0
    bytes({0xF2, 0x0F, 0x11, 0x4C, 0x24, 0x08}); // movsd [rsp+8], xmm1

    bytes({0x48, 0x8B, 0x44, 0x24, 0x08});       // mov  rax, [rsp+8]
    bytes({0x48, 0x01, 0xC0});                   // add  rax, rax
    const std::uint32_t exponentNonZero = jump(0x85);
    bytes({0x48, 0xB8}); enc.Qword(0x3FF0000000000000ull);
    bytes({0x48, 0x89, 0x04, 0x24});
    bytes({0xF2, 0x0F, 0x10, 0x04, 0x24});
    bytes({0x48, 0x83, 0xC4, 0x10});
    bytes({0xC3});
    patch(exponentNonZero);

    bytes({0x48, 0x8B, 0x04, 0x24});             // |base| == 0
    bytes({0x48, 0x01, 0xC0});
    const std::uint32_t baseNonZero = jump(0x85);
    bytes({0x48, 0x8B, 0x44, 0x24, 0x08});
    bytes({0x48, 0x85, 0xC0});
    const std::uint32_t zeroBaseNegativeExponent = jump(0x88);
    bytes({0x31, 0xC0});
    bytes({0x48, 0x89, 0x04, 0x24});
    bytes({0xF2, 0x0F, 0x10, 0x04, 0x24});
    bytes({0x48, 0x83, 0xC4, 0x10});
    bytes({0xC3});
    patch(zeroBaseNegativeExponent);
    bytes({0x48, 0xB8}); enc.Qword(0x7FF0000000000000ull);
    bytes({0x48, 0x89, 0x04, 0x24});
    bytes({0xF2, 0x0F, 0x10, 0x04, 0x24});
    bytes({0x48, 0x83, 0xC4, 0x10});
    bytes({0xC3});
    patch(baseNonZero);

    bytes({0x31, 0xD2});                         // decide result sign
    bytes({0x48, 0x8B, 0x04, 0x24});
    bytes({0x48, 0x85, 0xC0});
    const std::uint32_t positiveBase = jump(0x89);
    bytes({0xF2, 0x0F, 0x10, 0x54, 0x24, 0x08});
    bytes({0xF2, 0x48, 0x0F, 0x2C, 0xC2});
    bytes({0xF2, 0x48, 0x0F, 0x2A, 0xD8});
    bytes({0x66, 0x0F, 0x2E, 0xD3});
    const std::uint32_t nonIntegerExponent = jump(0x85);
    bytes({0x83, 0xE0, 0x01});
    bytes({0x89, 0xC2});
    enc.Byte(0xE9); const std::uint32_t magnitude = enc.Size(); enc.Dword(0);
    patch(nonIntegerExponent);
    bytes({0xBA, 0x02, 0x00, 0x00, 0x00});
    patch(positiveBase);
    patch(magnitude);

    bytes({0xDD, 0x44, 0x24, 0x08});             // |base| ** exponent
    bytes({0xDD, 0x04, 0x24});
    bytes({0xD9, 0xE1});
    bytes({0xD9, 0xF1});
    bytes({0xD9, 0xC0});
    bytes({0xD9, 0xFC});
    bytes({0xDC, 0xE9});
    bytes({0xD9, 0xC9});
    bytes({0xD9, 0xF0});
    bytes({0xD9, 0xE8});
    bytes({0xDE, 0xC1});
    bytes({0xD9, 0xFD});
    bytes({0xDD, 0xD9});

    bytes({0x85, 0xD2});
    const std::uint32_t store = jump(0x84);
    bytes({0x83, 0xFA, 0x02});
    const std::uint32_t nan = jump(0x84);
    bytes({0xD9, 0xE0});
    patch(store);
    bytes({0xDD, 0x1C, 0x24});
    bytes({0xF2, 0x0F, 0x10, 0x04, 0x24});
    bytes({0x48, 0x83, 0xC4, 0x10});
    bytes({0xC3});
    patch(nan);
    bytes({0xDD, 0xD8});
    bytes({0x48, 0xB8}); enc.Qword(0x7FF8000000000000ull);
    bytes({0x48, 0x89, 0x04, 0x24});
    bytes({0xF2, 0x0F, 0x10, 0x04, 0x24});
    bytes({0x48, 0x83, 0xC4, 0x10});
    bytes({0xC3});
    // clang-format on
    (void)moduleBuilder.EndFunction(floatPower64Symbol);
}

void X86_64RuntimeHelperEmitter::EmitFloatPower32() {
    if (floatPower32Symbol == NoSymbol || !moduleBuilder.BeginFunction(floatPower32Symbol)) {
        return;
    }

    X64Enc enc(moduleBuilder.SectionData(RcuModuleSection::Text));
    const auto bytes = [&](const std::initializer_list<std::uint8_t> values) {
        for (const std::uint8_t value : values) {
            enc.Byte(value);
        }
    };
    // clang-format off
    bytes({0xF3, 0x0F, 0x5A, 0xC0}); // cvtss2sd xmm0, xmm0
    bytes({0xF3, 0x0F, 0x5A, 0xC9}); // cvtss2sd xmm1, xmm1
    bytes({0x48, 0x83, 0xEC, 0x08}); // sub  rsp, 8
    std::uint32_t relocationOffset;
    enc.Call(relocationOffset);
    AddCallRelocation(relocationOffset, X86_64RuntimeHelper::FloatPower64);
    bytes({0x48, 0x83, 0xC4, 0x08}); // add  rsp, 8
    bytes({0xF2, 0x0F, 0x5A, 0xC0}); // cvtsd2ss xmm0, xmm0
    bytes({0xC3});
    // clang-format on
    (void)moduleBuilder.EndFunction(floatPower32Symbol);
}
} // namespace Rux

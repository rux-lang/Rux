#pragma once

#include "CodeGen/RcuModuleBuilder.h"
#include "Target/CallingConvention.h"

#include <cstdint>

namespace Rux {
enum class X86_64RuntimeHelper {
    IntegerPower,
    FloatPower64,
    FloatPower32,
};

/// Owns the synthesized helper functions referenced by one x86-64 RCU module. References declare helpers lazily;
/// EmitRequested writes only reachable bodies in dependency order after user functions have been emitted.
class X86_64RuntimeHelperEmitter {
public:
    X86_64RuntimeHelperEmitter(RcuModuleBuilder &moduleBuilder, CallingConvention defaultConvention);

    void AddCallRelocation(std::uint32_t sectionOffset, X86_64RuntimeHelper helper);
    void EmitRequested();

private:
    static constexpr std::uint32_t NoSymbol = ~std::uint32_t{0};

    RcuModuleBuilder &moduleBuilder;
    CallingConvention defaultConvention;
    std::uint32_t integerPowerSymbol = NoSymbol;
    std::uint32_t floatPower64Symbol = NoSymbol;
    std::uint32_t floatPower32Symbol = NoSymbol;

    [[nodiscard]] std::uint32_t Require(X86_64RuntimeHelper helper);
    [[nodiscard]] std::uint32_t Declare(const char *name);
    void EmitIntegerPower();
    void EmitFloatPower64();
    void EmitFloatPower32();
};
} // namespace Rux

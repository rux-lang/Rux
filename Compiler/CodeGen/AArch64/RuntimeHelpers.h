#pragma once

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/RcuModuleBuilder.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace Rux {
enum class AArch64RuntimeHelper {
    IntegerPower,
    FloatPower64,
    FloatPower32,
};

// Owns the synthesized helper functions referenced by one AArch64 RCU module.
// References declare helpers lazily; EmitRequested writes only reachable bodies
// after user functions, preserving helper dependency and body order.
class AArch64RuntimeHelperEmitter {
public:
    using DiagnosticReporter = std::function<void(std::string)>;

    AArch64RuntimeHelperEmitter(RcuModuleBuilder &moduleBuilder, unsigned &literalIndex,
                                DiagnosticReporter diagnosticReporter);

    void AddCallRelocation(std::uint32_t sectionOffset, AArch64RuntimeHelper helper);
    void EmitRequested();

private:
    static constexpr std::uint32_t NoSymbol = ~std::uint32_t{0};

    RcuModuleBuilder &moduleBuilder;
    A64Enc enc;
    unsigned &literalIndex;
    DiagnosticReporter reportDiagnostic;
    std::uint32_t integerPowerSymbol = NoSymbol;
    std::uint32_t floatPower64Symbol = NoSymbol;
    std::uint32_t floatPower32Symbol = NoSymbol;
    std::string currentFunction;

    [[nodiscard]] std::uint32_t Declare(std::string_view name);
    [[nodiscard]] std::uint32_t Require(AArch64RuntimeHelper helper);
    [[nodiscard]] std::uint32_t InternFloat64(std::string_view literal);
    void LoadFloat64Constant(A64Reg destination, std::string_view literal);
    void Must(A64Status status, std::string_view what);
    void EmitIntegerPower();
    void EmitFloatPower64();
    void EmitFloatPower32();
};
} // namespace Rux

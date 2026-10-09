#pragma once

// Where each float width's arithmetic comes from on a given architecture.
//
// float32 and float64 are machine floats everywhere. Every other width is computed by helper functions the compiler
// synthesizes from integer operations while lowering to LIR, except float80 on x86-64, where the x87 unit computes in
// exactly that format. One table decides this, so the lowering that rewrites operations into helper calls and the back
// end that emits x87 code cannot disagree about which operations reach it.

#include "Target/Target.h"
#include "Types/Type.h"

#include <cstdint>

namespace Rux::Target {
enum class FloatLowering : std::uint8_t {
    /// Not a float: nothing to decide.
    None,
    /// Machine registers and instructions: float32 and float64.
    Native,
    /// The x87 unit, for the operations `IsX87NativeOperation` names; everything else falls back to software.
    X87,
    /// Compiler-synthesized helper functions built from integer operations.
    Software,
};

/// The operations a float lowering might keep in LIR rather than rewrite into helper calls.
enum class FloatOperation : std::uint8_t {
    Arithmetic,       ///< add, sub, mul, div and remainder
    Compare,          ///< the six ordered and unordered comparisons
    ToNativeFloat,    ///< conversion to float32 or float64
    FromNativeFloat,  ///< conversion from float32 or float64
    FromSigned64,     ///< conversion from a signed integer of at most 64 bits
    ToInteger,        ///< conversion to any integer
    FromOtherInteger, ///< conversion from an unsigned or wider-than-64-bit integer
    OtherFloat,       ///< conversion to or from another software float width
};

/// Whether float80 is computed by the x87 unit on x86-64. The x86-64 back end lowers the operations
/// `IsX87NativeOperation` names to x87 code (`X86_64/X87Emitter.cpp`); turning this off computes float80 in software
/// on every architecture like the other non-native widths.
inline constexpr bool X87Float80 = true;

/// How `kind` is computed on `arch`.
[[nodiscard]] constexpr FloatLowering FloatLoweringFor(const TypeRef::Kind kind, const Arch arch) noexcept {
    switch (kind) {
    case TypeRef::Kind::Float32:
    case TypeRef::Kind::Float64:
        return FloatLowering::Native;
    case TypeRef::Kind::Float80:
        return X87Float80 && arch == Arch::X86_64 ? FloatLowering::X87 : FloatLowering::Software;
    case TypeRef::Kind::Float8:
    case TypeRef::Kind::Float16:
    case TypeRef::Kind::Float128:
    case TypeRef::Kind::Float256:
    case TypeRef::Kind::Float512:
        return FloatLowering::Software;
    default:
        return FloatLowering::None;
    }
}

/// Whether an x87-lowered width keeps `operation` in LIR for the back end. The rest -- conversion to any integer
/// (saturating, which `fistp` does not do), from unsigned or wider integers, and to or from the other software widths
/// -- goes through the software helpers like every other non-native width.
[[nodiscard]] constexpr bool IsX87NativeOperation(const FloatOperation operation) noexcept {
    switch (operation) {
    case FloatOperation::Arithmetic:
    case FloatOperation::Compare:
    case FloatOperation::ToNativeFloat:
    case FloatOperation::FromNativeFloat:
    case FloatOperation::FromSigned64:
        return true;
    case FloatOperation::ToInteger:
    case FloatOperation::FromOtherInteger:
    case FloatOperation::OtherFloat:
        return false;
    }
    return false;
}
} // namespace Rux::Target

#pragma once

// A small typed layer over LIR construction for the synthesized software-float helpers: values that carry their
// type, stack-backed variables, structured branches and loops, and the bit-level steps every helper shares.
//
// Helper bodies are never rewritten, so everything here is integer LIR. Every shift it emits is by less than the
// operand's width, because the back ends mask a shift count rather than saturate it. Definitions live in
// HirToLirSoftFloatCore.cpp.

#include "Lowering/HirToLir/HirToLirSoftFloat.h"
#include "Numeric/WideInteger.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Rux::HirToLirDetail {
/// A register and the type it holds. Operations on two values require one type for both.
struct SoftFloatValue {
    LirReg reg = LirNoReg;
    TypeRef type;
};

/// A stack slot holding a value of `type` that may be written any number of times, which is how a helper carries
/// state across branches and loop iterations without phis. Declare one outside a loop: each `Variable` is one slot.
struct SoftFloatVariable {
    LirReg slot = LirNoReg;
    TypeRef type;
};

/// `uint8` through `uint512`; `bits` must be one of those widths.
[[nodiscard]] TypeRef SoftFloatUnsignedType(std::uint32_t bits);
/// The float type `format` describes, float32 and float64 included.
[[nodiscard]] TypeRef SoftFloatFloatType(const FloatFormat &format);
/// The storage width of an integer, bool or float type in bits: 128 for float80.
[[nodiscard]] std::uint32_t SoftFloatBitsOf(const TypeRef &type);

class SoftFloatBuilder {
public:
    explicit SoftFloatBuilder(SoftFloatHelperSynthesis &inputSynthesis);

    [[nodiscard]] SoftFloatHelperSynthesis &Synthesis() const noexcept;
    [[nodiscard]] SoftFloatValue Parameter(std::size_t index) const;

    // Constants. A value is truncated to the type's width; a signed type may take a negative `Constant`.
    [[nodiscard]] SoftFloatValue Constant(std::int64_t value, const TypeRef &type);
    [[nodiscard]] SoftFloatValue Constant(const WideInteger &value, const TypeRef &type);
    [[nodiscard]] SoftFloatValue Unsigned(std::uint64_t value, std::uint32_t bits);
    [[nodiscard]] SoftFloatValue Bool(bool value);
    /// `1 << bit`, or zero when `bit` is at or above the width.
    [[nodiscard]] SoftFloatValue OneAt(std::uint32_t bit, const TypeRef &type);
    /// The low `bits` bits set, all of them when `bits` reaches the width.
    [[nodiscard]] SoftFloatValue LowMask(std::uint32_t bits, const TypeRef &type);

    // Arithmetic wraps at the width; `Div` and `Mod` and the ordered comparisons follow the type's signedness.
    [[nodiscard]] SoftFloatValue Add(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue Sub(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue Mul(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue Div(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue Mod(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue And(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue Or(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue Xor(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue BitNot(const SoftFloatValue &value);
    /// Logical negation of a bool.
    [[nodiscard]] SoftFloatValue Not(const SoftFloatValue &value);

    // Comparisons produce a bool.
    [[nodiscard]] SoftFloatValue Eq(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue Ne(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue Lt(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue Le(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue Gt(const SoftFloatValue &left, const SoftFloatValue &right);
    [[nodiscard]] SoftFloatValue Ge(const SoftFloatValue &left, const SoftFloatValue &right);
    /// `value == 0` and `value != 0`, against a zero of the value's own type.
    [[nodiscard]] SoftFloatValue IsZero(const SoftFloatValue &value);
    [[nodiscard]] SoftFloatValue IsNonZero(const SoftFloatValue &value);
    /// Whether bit `bit` of `value` is set, as a bool.
    [[nodiscard]] SoftFloatValue BitSet(const SoftFloatValue &value, std::uint32_t bit);

    /// `condition ? whenTrue : whenFalse` without a branch, through a mask; both sides are always computed.
    [[nodiscard]] SoftFloatValue Select(const SoftFloatValue &condition, const SoftFloatValue &whenTrue,
                                        const SoftFloatValue &whenFalse);
    /// An integer cast: extends by the source's signedness, or truncates. A bool becomes 0 or 1.
    [[nodiscard]] SoftFloatValue Resize(const SoftFloatValue &value, const TypeRef &type);

    // Shifts by a constant: logical, and zero once `amount` reaches the width.
    [[nodiscard]] SoftFloatValue ShiftLeft(const SoftFloatValue &value, std::uint32_t amount);
    [[nodiscard]] SoftFloatValue ShiftRight(const SoftFloatValue &value, std::uint32_t amount);
    // Shifts by a run-time amount of any integer type, read as unsigned. An amount at or above the width gives zero,
    // and `ShiftRightSticky` ORs every bit shifted out into bit zero, so it gives 0 or 1 there.
    [[nodiscard]] SoftFloatValue ShiftLeftClamped(const SoftFloatValue &value, const SoftFloatValue &amount);
    [[nodiscard]] SoftFloatValue ShiftRightClamped(const SoftFloatValue &value, const SoftFloatValue &amount);
    [[nodiscard]] SoftFloatValue ShiftRightSticky(const SoftFloatValue &value, const SoftFloatValue &amount);
    /// The leading zeros of an unsigned value as a uint32, the full width for zero.
    [[nodiscard]] SoftFloatValue CountLeadingZeros(const SoftFloatValue &value);

    /// A float's encoding as the unsigned integer of its storage width; float80's six padding bytes read as zero.
    [[nodiscard]] SoftFloatValue BitsOfFloat(const SoftFloatValue &value);
    /// The float of `type` whose encoding is `bits`, resized to the storage width first.
    [[nodiscard]] SoftFloatValue FloatOfBits(const SoftFloatValue &bits, const TypeRef &type);

    [[nodiscard]] SoftFloatVariable Variable(const TypeRef &type);
    [[nodiscard]] SoftFloatVariable Variable(const SoftFloatValue &initial);
    [[nodiscard]] SoftFloatValue Get(const SoftFloatVariable &variable);
    void Set(const SoftFloatVariable &variable, const SoftFloatValue &value);

    /// Structured control flow. Each body is emitted in its own block and falls through to a join block, which the
    /// builder then continues in, unless the body already ended its block with `Return`.
    void If(const SoftFloatValue &condition, const std::function<void()> &then);
    void IfElse(const SoftFloatValue &condition, const std::function<void()> &then,
                const std::function<void()> &otherwise);
    /// Re-evaluates `condition` before every iteration, in a block of its own.
    void While(const std::function<SoftFloatValue()> &condition, const std::function<void()> &body);

    /// A direct call, to a symbol from `SoftFloatHelperSynthesis::Require` for instance.
    [[nodiscard]] SoftFloatValue Call(const std::string &symbol, const TypeRef &result,
                                      const std::vector<SoftFloatValue> &arguments);
    void Return(const SoftFloatValue &value);
    /// Closes the current block with `Unreachable` if it is still open, which it is only when every path into it
    /// already returned. Call it once the body is complete.
    void Finish();

private:
    LirReg Emit(LirOpcode op, const TypeRef &type, std::vector<LirReg> sources, std::string argument = {});
    SoftFloatValue Binary(LirOpcode op, const SoftFloatValue &left, const SoftFloatValue &right);
    SoftFloatValue Compare(LirOpcode op, const SoftFloatValue &left, const SoftFloatValue &right);
    void Jump(std::uint32_t target);
    std::uint32_t Block(const char *label);
    void Enter(std::uint32_t block);

    SoftFloatHelperSynthesis *synthesis;
};

/// A float decoded the way `UnpackFloat` decodes it, held in registers.
///
/// `classification` is a uint8 `FloatClass`. Unpacking never yields `Subnormal`: a finite non-zero value is `Normal`,
/// its significand normalized with the leading bit at `precisionBits + 2` and the guard, round and sticky bits at 2, 1
/// and 0. `exponent` is the int32 unbiased exponent of that leading bit. Zero, infinity, NaN and invalid values have
/// a zero significand and exponent. `nanPayload` is a NaN's fraction field and zero otherwise. `significand` and
/// `nanPayload` have the format's working type.
struct SoftFloatUnpacked {
    SoftFloatValue classification;
    SoftFloatValue negative;
    SoftFloatValue exponent;
    SoftFloatValue significand;
    SoftFloatValue nanPayload;
};

/// The unsigned width a format's significand is worked in, which holds the significand with its three extra bits and
/// a product of two significands, except for float512, whose product needs two words. uint32 for float8 and float16,
/// uint64 for float32, uint128 for float64 and float80, uint256 for float128, uint512 above.
[[nodiscard]] std::uint32_t SoftFloatWorkingBits(const FloatFormat &format) noexcept;
[[nodiscard]] TypeRef SoftFloatWorkingType(const FloatFormat &format);

/// `UnpackFloat` on a value of `format`'s float type, or on its encoding as any unsigned integer.
[[nodiscard]] SoftFloatUnpacked UnpackSoftFloat(SoftFloatBuilder &builder, const FloatFormat &format,
                                                const SoftFloatValue &value);
[[nodiscard]] SoftFloatUnpacked UnpackSoftFloatBits(SoftFloatBuilder &builder, const FloatFormat &format,
                                                    const SoftFloatValue &bits);
/// `NormalizeFloat`: a zero significand makes the value `Zero`, anything else `Normal` with its leading bit at
/// `precisionBits + 2`, the bits shifted out kept in the sticky bit.
void NormalizeSoftFloat(SoftFloatBuilder &builder, const FloatFormat &format, SoftFloatUnpacked &value);
/// `PackFloat`, giving a value of `format`'s float type, or its encoding at the working width.
[[nodiscard]] SoftFloatValue PackSoftFloat(SoftFloatBuilder &builder, const FloatFormat &format,
                                           const SoftFloatUnpacked &value);
[[nodiscard]] SoftFloatValue PackSoftFloatBits(SoftFloatBuilder &builder, const FloatFormat &format,
                                               const SoftFloatUnpacked &value);
} // namespace Rux::HirToLirDetail

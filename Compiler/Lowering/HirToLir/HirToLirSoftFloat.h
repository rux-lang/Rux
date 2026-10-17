#pragma once

// Software-float lowering: the interface between the HIR-to-LIR rewrite of operations on non-native floats and the
// helper functions synthesized to compute them.
//
// Lowering emits float operations exactly as it does for float32 and float64. As each instruction is emitted, one that
// the target cannot compute itself is replaced by a call to a helper named for the operation and the format, and the
// helper is queued in a registry. Once every module is lowered the queue is drained: each helper becomes a private LIR
// function built from integer operations, and may queue further helpers it calls. A last check turns any float
// operation still left for a back end that cannot compute it into an internal error, rather than integer code that
// silently computes something else.

#include "Diagnostics/Diagnostics.h"
#include "Ir/Lir/Lir.h"
#include "Lowering/HirToLir/CheckedLirBuilder.h"
#include "Numeric/FloatFormat.h"
#include "Target/FloatLowering.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Rux::HirToLirDetail {
/// What a helper computes, with the signature `SoftFloatHelperSignatureOf` gives it. `F` is the key's `format` and
/// `T` its `other`. Each family is synthesized in its own file.
enum class SoftFloatOperation : std::uint8_t {
    Add,         ///< `(F, F) -> F`, in HirToLirSoftFloatArithmetic.cpp
    Subtract,    ///< `(F, F) -> F`, in HirToLirSoftFloatArithmetic.cpp
    Multiply,    ///< `(F, F) -> F`, in HirToLirSoftFloatMultiply.cpp
    Divide,      ///< `(F, F) -> F`, in HirToLirSoftFloatDivide.cpp
    Remainder,   ///< `(F, F) -> F`, in HirToLirSoftFloatDivide.cpp
    Compare,     ///< `(F, F) -> uint8`, a `SoftFloatOrdering`, in HirToLirSoftFloatCore.cpp
    ConvertTo,   ///< `(F) -> T`, in HirToLirSoftFloatConvert.cpp
    ConvertFrom, ///< `(T) -> F`, in HirToLirSoftFloatConvert.cpp
};

/// The code a compare helper returns. Every comparison operator is answered from it with integer operations, so an
/// unordered pair -- either side a NaN -- is false for everything but `!=`.
enum class SoftFloatOrdering : std::uint8_t {
    Less = 0,
    Equal = 1,
    Greater = 2,
    Unordered = 3,
};

/// One helper: what it computes, in which float, and for a conversion the type on the other side.
///
/// A conversion belongs to its source when the source is a float the target does not compute natively, and to its
/// destination otherwise: float128 to int32 is `ConvertTo` on float128, int32 to float128 `ConvertFrom` on float128,
/// and float16 to float128 `ConvertTo` on float16.
struct SoftFloatHelperKey {
    SoftFloatOperation operation = SoftFloatOperation::Add;
    /// A float the target computes in software, or float80. `ConvertFrom` may also name float32 or float64, for the
    /// integer conversions `ConvertsThroughSoftFloatHelper` routes through a helper, and `Remainder` may name them on a
    /// target without `Target::HasNativeFloatRemainder`.
    TypeRef::Kind format = TypeRef::Kind::Unknown;
    /// A conversion's other side, normalized by `SoftFloatHelperOperandKind`; `Unknown` for every other operation.
    TypeRef::Kind other = TypeRef::Kind::Unknown;

    friend auto operator<=>(const SoftFloatHelperKey &, const SoftFloatHelperKey &) = default;
};

/// The types a helper takes and returns. The call that replaces an operation and the function synthesized for it are
/// both built from this, so the two cannot disagree about the ABI.
struct SoftFloatHelperSignature {
    std::vector<TypeRef> parameters;
    TypeRef result;
};

/// `__rux_float_<f8|f16|f32|f64|f80|f128|f256|f512>_<op>`, where `<op>` is `add`, `sub`, `mul`, `div`, `rem`,
/// `compare`, `to_<type>` or `from_<type>`, and `<type>` is the primitive spelling of the key's `other`, such as
/// `int64` or `float32`.
[[nodiscard]] std::string SoftFloatHelperName(const SoftFloatHelperKey &key);
[[nodiscard]] SoftFloatHelperSignature SoftFloatHelperSignatureOf(const SoftFloatHelperKey &key);

/// The type a conversion helper takes or produces in place of `kind`. `int` and `uint` become the 64-bit integers they
/// are on every supported target, and a bool or a char the unsigned integer of its width, which is how a conversion
/// reads one. Every other kind is its own.
[[nodiscard]] TypeRef::Kind SoftFloatHelperOperandKind(TypeRef::Kind kind) noexcept;

/// Which float widths a target computes in software. Lowering uses the target's policy; a test substitutes its own, to
/// see what a policy not yet in force would leave to the back end.
struct SoftFloatPolicy {
    Target::Arch arch = Target::Arch::X86_64;
    Target::FloatLowering (*lowering)(TypeRef::Kind, Target::Arch) = &Target::FloatLoweringFor;

    [[nodiscard]] Target::FloatLowering For(const TypeRef::Kind kind) const {
        return lowering(kind, arch);
    }
};

/// The helper `instruction` has to become a call to, or nothing when the back end computes it as it is. `operandKind`
/// is the kind of the instruction's first source, which is what a comparison or a conversion is about. A cast of a
/// software float to a bool needs the compare helper, because it asks whether the value differs from zero.
[[nodiscard]] std::optional<SoftFloatHelperKey>
SoftFloatHelperFor(const LirInstr &instruction, TypeRef::Kind operandKind, const SoftFloatPolicy &policy);

/// Whether a conversion with no software float on either side still needs a `ConvertFrom` helper on `to`, because a
/// back end cannot perform it correctly by itself. Owned by the conversion family.
[[nodiscard]] bool ConvertsThroughSoftFloatHelper(TypeRef::Kind from, TypeRef::Kind to, Target::Arch arch);

class SoftFloatHelperRegistry;

/// One helper under construction, handed to the synthesis entry point of its family.
///
/// The function is already named, private, and given the parameters and result of `SoftFloatHelperSignatureOf`, and the
/// builder has the entry block selected. The entry point emits the body through `Builder` (or through `Emit` and
/// `Terminate`, which record a refusal), calls another helper by the symbol `Require` returns, and must end every block
/// it creates. A refusal, or a block left open, is reported as an internal error naming the helper.
class SoftFloatHelperSynthesis {
public:
    SoftFloatHelperSynthesis(SoftFloatHelperRegistry &inputRegistry, const SoftFloatHelperKey &inputKey,
                             LirFunc &inputFunction);

    [[nodiscard]] const SoftFloatHelperKey &Key() const noexcept;
    /// The layout of the key's `format`, the same description `SoftwareFloat` folds constants with.
    [[nodiscard]] const FloatFormat &Format() const noexcept;
    [[nodiscard]] LirFunc &Function() const noexcept;
    [[nodiscard]] CheckedLirBuilder &Builder() noexcept;
    /// The register holding parameter `index`, in signature order.
    [[nodiscard]] LirReg Parameter(std::size_t index) const;

    /// The symbol to call for another helper, with the signature `SoftFloatHelperSignatureOf` gives it. The helper is
    /// queued and synthesized as well, unless something already asked for it.
    [[nodiscard]] const std::string &Require(const SoftFloatHelperKey &helper);

    [[nodiscard]] LirReg NewReg();
    void Emit(LirInstr &&instruction);
    void Terminate(LirTerminator terminator);
    /// Records why the helper cannot be built. Only the first reason is kept.
    void Fail(std::string detail);
    [[nodiscard]] const std::string &Failure() const noexcept;

private:
    SoftFloatHelperRegistry *registry;
    SoftFloatHelperKey key;
    const FloatFormat *format;
    LirFunc *function;
    CheckedLirBuilder builder;
    std::string failure;
};

/// Every helper a package needs, each synthesized once under its one name.
class SoftFloatHelperRegistry {
public:
    /// The symbol of `key`'s helper, queuing it the first time it is asked for.
    const std::string &Request(const SoftFloatHelperKey &key);

    /// Synthesizes every queued helper, and every helper those ask for in turn, in the order they were first asked for.
    [[nodiscard]] std::vector<LirFunc> SynthesizePending(std::vector<Diagnostic> &diagnostics);

    /// Queue what `other` queued, in its order, as if its requests had been made here after this registry's own.
    void Absorb(const SoftFloatHelperRegistry &other);

private:
    std::map<SoftFloatHelperKey, std::string> symbols;
    std::vector<SoftFloatHelperKey> pending;
};

/// The synthesis entry point of each family. Each one receives a helper of its own operations only.
void SynthesizeSoftFloatComparison(SoftFloatHelperSynthesis &synthesis);     ///< Compare
void SynthesizeSoftFloatAddition(SoftFloatHelperSynthesis &synthesis);       ///< Add and Subtract
void SynthesizeSoftFloatMultiplication(SoftFloatHelperSynthesis &synthesis); ///< Multiply
void SynthesizeSoftFloatDivision(SoftFloatHelperSynthesis &synthesis);       ///< Divide and Remainder
void SynthesizeSoftFloatConversion(SoftFloatHelperSynthesis &synthesis);     ///< ConvertTo and ConvertFrom

/// A body for a helper whose family is not written yet: it stops the program with a panic naming the helper, so a
/// result is never silently wrong.
void EmitSoftFloatPlaceholder(SoftFloatHelperSynthesis &synthesis);

/// Reports, as internal errors, every arithmetic, comparison or conversion in `package` that `policy` says a back end
/// cannot compute. Lowering runs this last, once every operation should have become a helper call.
void VerifySoftFloatLowering(const LirPackage &package, const SoftFloatPolicy &policy,
                             std::vector<Diagnostic> &diagnostics);
} // namespace Rux::HirToLirDetail

#pragma once

// A test-only LIR interpreter that executes synthesized software-float helpers, so they can be compared bit for bit
// against Numeric/SoftwareFloat without generating machine code.

#include "Ir/Lir/Lir.h"
#include "Numeric/WideInteger.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rux::Testing {
/// Why an interpreted run stopped before its function returned.
enum class LirFault {
    None,
    DivideByZero,   ///< `Div` or `Mod` by zero, which the back ends trap on
    SignedOverflow, ///< signed `minimum / -1` or `minimum % -1`, which the wide paths trap on
    AssertFailed,   ///< an `Assert` whose condition was false
    Panic,          ///< a `Panic`, or an `Unreachable` terminator that was reached
    StepLimit,      ///< more steps, or deeper calls, than the options allow
    Unsupported,    ///< an opcode, type or operand form the interpreter does not model
    Malformed,      ///< LIR that breaks its own rules: an undefined register, a width mismatch, a bad address
};

/// The outcome of one interpreted call.
struct LirRunResult {
    /// The returned bit pattern at the return type's value width, absent for a function returning nothing or when a
    /// fault stopped the run.
    std::optional<WideInteger> value;
    LirFault fault = LirFault::None;
    /// What stopped the run, naming the function and opcode. Empty exactly when `fault` is `None`.
    std::string failure;
    /// Contract breaches the run survived, such as a shift amount at or above the width. The back ends define a
    /// result for those, which the run reproduces, but a synthesized helper must never depend on it.
    std::vector<std::string> violations;
    /// Instructions and terminators executed, across every call.
    std::uint64_t steps = 0;

    [[nodiscard]] bool Ok() const noexcept {
        return fault == LirFault::None && violations.empty();
    }

    /// The failure and every violation on one line each, for a test's failure message.
    [[nodiscard]] std::string Describe() const;
};

struct LirInterpreterOptions {
    std::uint64_t stepLimit = 50'000'000;
    std::uint32_t callDepthLimit = 256;
};

/// The width in bits an interpreted value of `type` is held at: the storage width for integers, chars and bools, the
/// format's value width for floats (80 for `float80`, which is stored in 16 bytes), and 64 for pointers.
///
/// @return zero for a type the interpreter does not model
[[nodiscard]] std::uint32_t LirValueWidth(const TypeRef &type);

/// Executes functions of one package. Every integer, char, bool and float up to 512 bits is a bit pattern, so a float
/// helper's arguments and result are exactly the `FloatEncoding::Bits()` it is compared against.
///
/// Semantics, matching the native back ends where they agree and trapping where a helper must not go:
/// - Arithmetic wraps at the result type's width; every operand must have that width.
/// - `Div`/`Mod` truncate toward zero when the type is signed. Division by zero and signed `min / -1` fault.
/// - `Shl`/`Shr`/`Lshr` mask the amount as the back ends do (to width - 1 above 64 bits, to 63 at or below), and an
///   amount at or above the width is recorded as a violation.
/// - `Cast` sign- or zero-extends by the source's signedness, truncates, normalizes to bool by testing the whole
///   source, and passes a value through unchanged between same-width integers and pointers. A value-changing float
///   conversion is unsupported: helpers reinterpret bits through memory instead.
/// - `Alloca` returns fresh bytes filled with 0xA5, since a real slot is uninitialized. `Store` writes the value's
///   bytes little-endian and zero-fills to the type's storage size; `Load` reads the value width back, so one slot
///   reads as a float or as a uint of the same size. Allocations are released when their function returns.
/// - `Phi` reads every incoming value before defining any, keyed by the block control came from.
/// - `Call` resolves a function by name in any module of the package; recursion is allowed.
/// - Float arithmetic and comparison, `CallIndirect`, `FieldPtr`, `IndexPtr`, named loads and global addresses are
///   unsupported, and fail naming the opcode.
class LirInterpreter {
public:
    /// Indexes the package's functions by name. The package must outlive the interpreter.
    explicit LirInterpreter(const LirPackage &package, LirInterpreterOptions options = {});

    /// Call `function` with `arguments`, one per parameter. An argument narrower than its parameter is zero-extended;
    /// a wider one must fit, or the run fails as malformed.
    [[nodiscard]] LirRunResult Run(std::string_view function, std::span<const WideInteger> arguments) const;

private:
    LirInterpreterOptions options;
    std::unordered_map<std::string_view, const LirFunc *> functions;
};

/// One call through a fresh interpreter, for a test that runs a function only once.
[[nodiscard]] LirRunResult RunLirFunction(const LirPackage &package, std::string_view function,
                                          std::span<const WideInteger> arguments,
                                          const LirInterpreterOptions &options = {});
} // namespace Rux::Testing

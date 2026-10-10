#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace Rux {

enum class RuntimeFailureKind {
    Assertion,
    Panic
};

/// Runtime failures are emitted as three writes because the message is a runtime string. Keeping the two static
/// fragments together here makes their logical byte layout independent of the target's writing mechanism.
struct RuntimeFailureLayout {
    std::string prefix;
    std::string location;

    [[nodiscard]] std::string Join(std::string_view message) const;
};

[[nodiscard]] RuntimeFailureLayout BuildRuntimeFailureLayout(RuntimeFailureKind kind, std::string_view function,
                                                             std::string_view file, std::size_t line,
                                                             std::size_t column);

/// What one module contributes to a program's stack traces. A program whose lowering synthesized the stack-trace
/// printer has every module emit a table of its functions' address ranges and names, and the last module a table of
/// those tables, which the printer reads; every panic and failed assertion then calls the printer before trapping.
struct BacktraceTables {
    bool enabled = false;
    std::size_t moduleIndex = 0;
    std::size_t moduleCount = 0;

    [[nodiscard]] bool IsLast() const noexcept {
        return moduleIndex + 1 == moduleCount;
    }
};

/// The table of tables the stack-trace printer starts from.
inline constexpr std::string_view kFunctionTablesSymbol = "__rux_function_tables";

/// The name of one module's table of function ranges.
[[nodiscard]] std::string FunctionTableSymbol(std::size_t moduleIndex);

} // namespace Rux

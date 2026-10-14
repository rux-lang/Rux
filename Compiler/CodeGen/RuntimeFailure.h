#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

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

/// What one module contributes to a program's stack traces. Every module emits a table of its functions' address
/// ranges and names under a symbol its own name decides, and every panic and failed assertion calls the stack-trace
/// printer before trapping. The program module, which holds the printer when anything can fail, also emits the table of
/// every module's table the printer reads. Nothing a source module emits depends on the other modules, so its object
/// can be reused while they change.
struct BacktraceTables {
    bool enabled = false;
    /// The symbol of this module's own table.
    std::string table;
    /// In the program module of a program that can fail, every module's table symbol in module order; otherwise empty.
    std::vector<std::string> programTables;

    [[nodiscard]] bool IsProgram() const noexcept {
        return !programTables.empty();
    }
};

/// The table of tables the stack-trace printer starts from.
inline constexpr std::string_view kFunctionTablesSymbol = "__rux_function_tables";

/// The name of the table of function ranges of the module named `moduleName`.
[[nodiscard]] std::string FunctionTableSymbol(std::string_view moduleName);

} // namespace Rux

#pragma once

// The compiler-supplied values a package declares with `intrinsic #name: Root;`.
//
// The compiler fills in a fixed set of fields for each root, and nothing else: a declaration naming a field outside the
// set would read storage nobody wrote. Semantic analysis checks declarations against this one table, and both the
// compile-time evaluator and lowering answer exactly the fields it lists.

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace Rux {
struct CompilerParameterRoot {
    std::string_view name;
    std::span<const std::string_view> fields;
};

namespace CompilerParameterDetail {
inline constexpr std::array<std::string_view, 8> TargetFields{"os",          "arch",      "abi",          "endian",
                                                              "pointerBits", "dataModel", "objectFormat", "triple"};
inline constexpr std::array<std::string_view, 10> BuildFields{"profile",   "mode",   "optimization", "debugAssertions",
                                                              "debugInfo", "isTest", "outputKind",   "timestamp",
                                                              "date",      "time"};
inline constexpr std::array<std::string_view, 7> SourceFields{"line",     "column",   "file",  "fileName",
                                                              "filePath", "function", "module"};
inline constexpr std::array<std::string_view, 1> CompilerFields{"version"};
} // namespace CompilerParameterDetail

/// Every root the compiler supplies a value for, with the fields it fills in. `Config` carries no fields; its values
/// are reached through its intrinsic methods.
inline constexpr std::array<CompilerParameterRoot, 5> CompilerParameterRoots{{
    {"Target", CompilerParameterDetail::TargetFields},
    {"Build", CompilerParameterDetail::BuildFields},
    {"Source", CompilerParameterDetail::SourceFields},
    {"Compiler", CompilerParameterDetail::CompilerFields},
    {"Config", {}},
}};

/// The root named `name`, or null when the compiler supplies no value of that type.
[[nodiscard]] inline const CompilerParameterRoot *FindCompilerParameterRoot(const std::string_view name) noexcept {
    const auto found = std::ranges::find(CompilerParameterRoots, name, &CompilerParameterRoot::name);
    return found == CompilerParameterRoots.end() ? nullptr : &*found;
}

/// Whether the compiler fills in `field` of a `root` value.
[[nodiscard]] inline bool IsSuppliedCompilerParameterField(const std::string_view root,
                                                           const std::string_view field) noexcept {
    const CompilerParameterRoot *found = FindCompilerParameterRoot(root);
    return found && std::ranges::contains(found->fields, field);
}
} // namespace Rux

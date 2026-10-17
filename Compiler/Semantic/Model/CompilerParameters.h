#pragma once

// The compiler-supplied values a package declares with `intrinsic #name: Root;`.
//
// The compiler fills in a fixed set of fields for each root, and nothing else: a declaration naming a field outside the
// set would read storage nobody wrote, while a field declared with another type would give semantic analysis a shape
// different from the value lowering supplies. Semantic analysis checks declarations against this one table, and both
// the compile-time evaluator and lowering answer exactly the fields it lists.

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace Rux {
enum class CompilerParameterFieldKind {
    Value,
    Enum,
    Struct,
};

struct CompilerParameterField {
    std::string_view name;
    std::string_view type;
    CompilerParameterFieldKind kind = CompilerParameterFieldKind::Value;
};

struct CompilerParameterRoot {
    std::string_view name;
    std::span<const CompilerParameterField> fields;
};

namespace CompilerParameterDetail {
using Kind = CompilerParameterFieldKind;
inline constexpr std::array<CompilerParameterField, 8> TargetFields{{
    {"os", "OperatingSystem", Kind::Enum},
    {"arch", "Architecture", Kind::Enum},
    {"abi", "ApplicationBinaryInterface", Kind::Enum},
    {"endian", "Endianness", Kind::Enum},
    {"pointerBits", "uint"},
    {"dataModel", "DataModel", Kind::Enum},
    {"objectFormat", "ObjectFormat", Kind::Enum},
    {"triple", "char8[..]"},
}};
inline constexpr std::array<CompilerParameterField, 10> BuildFields{{
    {"profile", "char8[..]"},
    {"mode", "BuildMode", Kind::Enum},
    {"optimization", "OptimizationMode", Kind::Enum},
    {"debugAssertions", "bool"},
    {"debugInfo", "bool"},
    {"isTest", "bool"},
    {"outputKind", "OutputKind", Kind::Enum},
    {"timestamp", "uint64"},
    {"date", "char8[..]"},
    {"time", "char8[..]"},
}};
inline constexpr std::array<CompilerParameterField, 7> SourceFields{{
    {"line", "uint"},
    {"column", "uint"},
    {"file", "char8[..]"},
    {"fileName", "char8[..]"},
    {"filePath", "char8[..]"},
    {"function", "char8[..]"},
    {"module", "char8[..]"},
}};
inline constexpr std::array<CompilerParameterField, 1> CompilerFields{{
    {"version", "SemanticVersion", Kind::Struct},
}};
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

/// The field the compiler supplies under `root`, or null when it supplies no field by that name.
[[nodiscard]] inline const CompilerParameterField *FindCompilerParameterField(const std::string_view root,
                                                                              const std::string_view field) noexcept {
    const CompilerParameterRoot *found = FindCompilerParameterRoot(root);
    if (!found) {
        return nullptr;
    }
    const auto supplied = std::ranges::find(found->fields, field, &CompilerParameterField::name);
    return supplied == found->fields.end() ? nullptr : &*supplied;
}

/// Whether the compiler fills in `field` of a `root` value.
[[nodiscard]] inline bool IsSuppliedCompilerParameterField(const std::string_view root,
                                                           const std::string_view field) noexcept {
    return FindCompilerParameterField(root, field) != nullptr;
}

/// Every language feature `#compiler.HasFeature` reports, letting source guard a construct that older releases would
/// not accept. Conditional folding and lowering both answer from this one list, so a `when` guard and an ordinary
/// expression agree.
inline constexpr std::array<std::string_view, 10> CompilerFeatures{
    "conditional-compilation",    "namespaced-intrinsics",
    "target-intrinsics",          "build-intrinsics",
    "compiler-feature-detection", "source-location-defaults",
    "extern-symbol-names",        "link-attribute",
    "no-return-attribute",        "when-attribute"};

/// Whether this compiler implements the language feature named `feature`.
[[nodiscard]] inline bool IsSupportedCompilerFeature(const std::string_view feature) noexcept {
    return std::ranges::contains(CompilerFeatures, feature);
}
} // namespace Rux

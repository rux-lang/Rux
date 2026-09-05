#pragma once

#include <filesystem>
#include <string_view>

namespace Rux::Driver {
enum class CompilePhase {
    Configuring,
    LoadingSources,
    Lexing,
    Parsing,
    LoadingDependency,
    Analyzing,
    LoweringToHir,
    OptimizingHir,
    LoweringToLir,
    OptimizingLir,
    EmittingAssembly,
    EmittingObjects,
    Linking,
};

enum class InspectionKind {
    Tokens,
    Ast,
    Semantic,
    Hir,
    Lir,
    Assembly,
    RcuObject,
    Rcu,
};

struct InspectionOutput {
    InspectionKind kind;
    std::filesystem::path path;
};

[[nodiscard]] std::string_view InspectionHeading(InspectionKind kind) noexcept;
[[nodiscard]] std::string_view InspectionDescription(InspectionKind kind) noexcept;

struct CompileProgress {
    CompilePhase phase;
    std::string_view subject;
    std::filesystem::path path;
};

[[nodiscard]] std::string_view CompilePhaseName(CompilePhase phase) noexcept;

} // namespace Rux::Driver

#include "Driver/CompileEvents.h"

#include <utility>

namespace Rux::Driver {
std::string_view CompilePhaseName(const CompilePhase phase) noexcept {
    static constexpr std::string_view names[]{
        "Configuring",       "Loading sources",      "Lexing",         "Parsing",         "Loading dependency",
        "Analyzing",         "Lowering to HIR",      "Optimizing HIR", "Lowering to LIR", "Optimizing LIR",
        "Emitting assembly", "Emitting RCU objects", "Linking"};
    return names[std::to_underlying(phase)];
}

std::string_view InspectionHeading(const InspectionKind kind) noexcept {
    static constexpr std::string_view headings[]{"token inspection output",
                                                 "AST inspection output",
                                                 "semantic inspection output",
                                                 "HIR inspection output",
                                                 "LIR inspection output",
                                                 "assembly inspection output",
                                                 "RCU object",
                                                 "RCU inspection output"};
    return headings[std::to_underlying(kind)];
}

std::string_view InspectionDescription(const InspectionKind kind) noexcept {
    static constexpr std::string_view descriptions[]{"lexical tokens and source locations",
                                                     "parsed abstract syntax tree",
                                                     "resolved symbols, signatures, type capabilities, and diagnostics",
                                                     "high-level intermediate representation",
                                                     "low-level intermediate representation",
                                                     "x86-64 textual assembly",
                                                     "binary Rux Compiled Unit",
                                                     "human-readable Rux Compiled Unit"};
    return descriptions[std::to_underlying(kind)];
}

} // namespace Rux::Driver

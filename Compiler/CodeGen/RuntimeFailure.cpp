#include "CodeGen/RuntimeFailure.h"

#include <format>

namespace Rux {

std::string RuntimeFailureLayout::Join(const std::string_view message) const {
    std::string result;
    result.reserve(prefix.size() + message.size() + location.size());
    result += prefix;
    result += message;
    result += location;
    return result;
}

RuntimeFailureLayout BuildRuntimeFailureLayout(const RuntimeFailureKind kind, const std::string_view function,
                                               const std::string_view file, const std::size_t line,
                                               const std::size_t column) {
    const std::string_view prefix = kind == RuntimeFailureKind::Assertion ? "Assertion failed: " : "Panic: ";
    const std::string_view stableFunction = function.empty() ? "<unknown>" : function;
    const std::string_view stableFile = file.empty() ? "<unknown>" : file;
    return {.prefix = std::string(prefix),
            .location = std::format("\n  at {} ({}:{}:{})\n", stableFunction, stableFile, line, column)};
}

} // namespace Rux

#include "CodeGen/RuntimeFailure.h"

#include "Object/Rcu/Rcu.h"

#include <cstdint>
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

std::string FunctionTableSymbol(const std::string_view moduleName) {
    // FNV-1a over the module's name: a module keeps its symbol however many modules come before it.
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char character : moduleName) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 0x100000001b3ULL;
    }
    return std::format("{}{:016x}", kFunctionTablePrefix, hash);
}

} // namespace Rux

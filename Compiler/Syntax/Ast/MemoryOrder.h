#pragma once

// The memory orders an atomic intrinsic call names, read from the call as it was written.
// An ordering is chosen while compiling, so every call writes one as a `MemoryOrder` case, such as
// `MemoryOrder::Acquire`. Semantic analysis and lowering read it with this one function, so they cannot disagree about
// which case an argument names.

#include "Syntax/Ast/Ast.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Rux {
enum class MemoryOrderKind : std::uint8_t {
    Relaxed = 0,
    Acquire = 1,
    Release = 2,
    AcquireRelease = 3,
    SequentiallyConsistent = 4,
};

[[nodiscard]] inline std::optional<MemoryOrderKind> MemoryOrderCase(const std::string_view name) noexcept {
    if (name == "Relaxed") {
        return MemoryOrderKind::Relaxed;
    }
    if (name == "Acquire") {
        return MemoryOrderKind::Acquire;
    }
    if (name == "Release") {
        return MemoryOrderKind::Release;
    }
    if (name == "AcquireRelease") {
        return MemoryOrderKind::AcquireRelease;
    }
    if (name == "SequentiallyConsistent") {
        return MemoryOrderKind::SequentiallyConsistent;
    }
    return std::nullopt;
}

/// The order an argument names, when it is written as `.Case` or `MemoryOrder::Case`; nullopt for any other expression.
[[nodiscard]] inline std::optional<MemoryOrderKind> MemoryOrderOf(const Expr &argument) noexcept {
    if (const auto *shorthand = AstCast<const EnumShorthandExpr>(&argument)) {
        return MemoryOrderCase(shorthand->variant);
    }
    if (const auto *path = AstCast<const PathExpr>(&argument);
        path && path->segments.size() >= 2 && path->segments[path->segments.size() - 2] == "MemoryOrder") {
        return MemoryOrderCase(path->segments.back());
    }
    return std::nullopt;
}
} // namespace Rux

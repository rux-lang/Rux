#pragma once

#include "Semantic/Type.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Rux {
/// One ordered operation in compiler-synthesized destruction for a concrete type. Child operations describe the
/// selected field, element, or variant payload after the parent has located it.
struct DropGlueStep {
    enum class Kind {
        InvokeDrop,
        Field,
        TupleElement,
        ArrayElements,
        EnumVariant,
    };

    Kind kind;
    TypeRef type;
    std::string name;
    std::uint64_t ordinal = 0;
    std::uint64_t count = 0;
    bool reverse = false;
    std::vector<DropGlueStep> children;
};

/// The complete destruction recipe for one concrete droppable type. Steps are stored in execution order, including
/// reverse field and element order, so later cleanup insertion does not need to rediscover aggregate semantics.
struct DropGluePlan {
    TypeRef type;
    std::string symbol;
    std::vector<DropGlueStep> steps;
};
} // namespace Rux

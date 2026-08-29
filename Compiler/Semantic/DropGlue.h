#pragma once

#include "Ir/CaseTypeForm.h"
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
    CaseTypeForm form = CaseTypeForm::Enumeration;
    TypeRef type;
    std::string name;
    std::uint64_t ordinal = 0;
    std::uint64_t count = 0;
    bool reverse = false;
    /// Linker name of the type's destructor, resolved while lowering because a generic implementation is
    /// instantiated. Empty for every kind but InvokeDrop.
    std::string dropSymbol;
    /// The tag value an EnumVariant step tests for, resolved with the same lookup a construction of that variant uses.
    std::string discriminant;
    /// Every payload an EnumVariant step's variant carries, in construction order, named fields first. Destruction
    /// needs the ones it will not touch too: a payload's offset follows the sizes of the payloads before it.
    std::vector<TypeRef> payloadTypes;
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

#pragma once

#include "Semantic/CompileTimeContext.h"
#include "Semantic/Type.h"

#include <optional>
#include <string>
#include <string_view>

namespace Rux {
// A built-in constant associated with a primitive type. `value` uses the same
// printable representation as HIR/LIR literals, so resolving one never creates
// storage or a linker-visible symbol.
struct PrimitiveConstant {
    TypeRef type;
    std::string value;
};

[[nodiscard]] std::optional<TypeRef> PrimitiveTypeFromName(std::string_view name);

[[nodiscard]] std::optional<PrimitiveConstant> LookupPrimitiveConstant(const TypeRef &type, std::string_view name,
                                                                       const CompileTimeContext &context);

[[nodiscard]] std::optional<PrimitiveConstant> LookupPrimitiveConstant(std::string_view typeName, std::string_view name,
                                                                       const CompileTimeContext &context);
} // namespace Rux

// The compile-time arithmetic an allocation is computed with: `sizeof` and `alignof`. The run-time checks that report
// an overflow instead of producing a wrapped byte count are the `CheckedAdd`, `CheckedSub` and `CheckedMul` methods
// every integer type carries in Core.

#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <array>
#include <format>

namespace Rux::SemanticDetail {

TypeRef AnalysisContext::CheckTypeQueryExpression(const TypeQueryExpr &expression) {
    const bool isSize = expression.query == TypeQueryExpr::Query::Size;
    const std::string_view spelling = isSize ? "sizeof" : "alignof";
    ValidateArrayType(*expression.type);
    const TypeRef queried = ResolveType(*expression.type);
    if (queried.IsUnknown() || queried->kind == TypeRef::Kind::TypeParam) {
        // A type parameter has no layout until it is substituted, so the query folds in each instantiation instead.
        return TypeRef::MakeUInt64();
    }

    if (const auto layout = LayoutOfTypeExpression(*expression.type)) {
        typeQueryValues.insert_or_assign(&expression, isSize ? layout->size : layout->alignment);
    }
    else {
        EmitError(
            expression.location,
            std::format("cannot determine the {} of type '{}'", isSize ? "size" : "alignment", queried.ToString()),
            {std::format("'{}' needs a type whose layout is known at compile time", spelling)});
    }
    return TypeRef::MakeUInt64();
}

} // namespace Rux::SemanticDetail

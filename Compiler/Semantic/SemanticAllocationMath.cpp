// The compile-time and run-time arithmetic an allocation is computed with: `sizeof` and `alignof`, and the checked
// operations that report an overflow instead of producing a wrapped byte count.
//
// An allocation size is a product of a count and an element size, and a wrapped product is an allocation far smaller
// than the caller asked for. Every operation here reports the overflow rather than trapping, so the caller decides what
// a request too large to express becomes.

#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <algorithm>
#include <array>
#include <format>

namespace Rux::SemanticDetail {
namespace {
/// The intrinsic key each checked operation is recognized by, in the order the registry declares them.
constexpr std::array<std::string_view, 3> kCheckedOperations{"CheckedAdd", "CheckedSub", "CheckedMul"};
} // namespace

TypeRef SemanticAnalyzerContext::CheckTypeQueryExpression(const TypeQueryExpr &expression) {
    const bool isSize = expression.query == TypeQueryExpr::Query::Size;
    const std::string_view spelling = isSize ? "sizeof" : "alignof";
    ValidateArrayType(*expression.type, /*allowFlexibleTail=*/false);
    const TypeRef queried = ResolveType(*expression.type);
    if (queried.IsUnknown() || queried.kind == TypeRef::Kind::TypeParam) {
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

bool SemanticAnalyzerContext::IsCheckedArithmeticIntrinsic(const std::string_view intrinsicName) {
    return std::ranges::find(kCheckedOperations, intrinsicName) != kCheckedOperations.end();
}

void SemanticAnalyzerContext::ValidateCheckedArithmeticIntrinsic(const FuncDecl &declaration) {
    if (!IsCheckedArithmeticIntrinsic(declaration.intrinsicName)) {
        return;
    }

    // The compiler emits the operation inline, so the declaration is a contract rather than a signature it can adapt
    // to: two operands, somewhere to put the result, and a report of whether the result is the true one.
    const auto reject = [&](std::string reason) {
        EmitError(declaration.location,
                  std::format("checked arithmetic intrinsic '{}' must be declared as "
                              "'func {}(left: uint64, right: uint64, result: *var uint64) -> bool'",
                              declaration.name, declaration.name),
                  {std::move(reason)});
    };

    if (declaration.params.size() != 3) {
        reject(std::format("it declares {} parameter{}", declaration.params.size(),
                           declaration.params.size() == 1 ? "" : "s"));
        return;
    }
    for (std::size_t index = 0; index < 2; ++index) {
        const TypeRef operand = ResolveType(*declaration.params[index].type);
        if (operand.kind != TypeRef::Kind::UInt64) {
            reject(std::format("parameter '{}' has type '{}'", declaration.params[index].name, operand.ToString()));
            return;
        }
    }
    const TypeRef result = ResolveType(*declaration.params[2].type);
    const auto *pointer = dynamic_cast<const PointerTypeExpr *>(declaration.params[2].type.get());
    if (!pointer || !pointer->pointeeMut || result.kind != TypeRef::Kind::Pointer || result.inner.empty() ||
        result.inner.front().kind != TypeRef::Kind::UInt64) {
        reject(std::format("parameter '{}' has type '{}'", declaration.params[2].name, result.ToString()));
        return;
    }
    const TypeRef returned =
        declaration.returnType ? ResolveType(*declaration.returnType->get()) : TypeRef::MakeOpaque();
    if (returned.kind != TypeRef::Kind::Bool8) {
        reject(std::format("it returns '{}'", returned.ToString()));
    }
}
} // namespace Rux::SemanticDetail

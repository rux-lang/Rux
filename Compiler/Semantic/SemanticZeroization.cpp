// Zeroization: the one write the compiler promises never to remove.
//
// Clearing a secret is a store nothing reads back, which is precisely the shape an optimizer is entitled to delete. A
// package cannot express "and mean it" in ordinary source, so the guarantee is the compiler's: `Zeroize` is recognized
// by name, its declaration is held to the signature the compiler emits for, and every write it lowers to is marked as
// one that must happen.

#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <format>

namespace Rux::SemanticDetail {
namespace {
constexpr std::string_view kZeroizeIntrinsic = "Zeroize";
} // namespace

bool SemanticAnalyzerContext::IsZeroizeIntrinsic(const std::string_view intrinsicName) {
    return intrinsicName == kZeroizeIntrinsic;
}

void SemanticAnalyzerContext::ValidateZeroizeIntrinsic(const FuncDecl &declaration) {
    if (!IsZeroizeIntrinsic(declaration.intrinsicName)) {
        return;
    }

    const auto reject = [&](std::string reason) {
        EmitError(declaration.location,
                  std::format("zeroization intrinsic '{}' must be declared as "
                              "'func {}(memory: *var uint8, length: uint64)'",
                              declaration.name, declaration.name),
                  {std::move(reason)});
    };

    if (declaration.params.size() != 2) {
        reject(std::format("it declares {} parameter{}", declaration.params.size(),
                           declaration.params.size() == 1 ? "" : "s"));
        return;
    }

    const TypeRef memory = ResolveType(*declaration.params[0].type);
    const auto *pointer = dynamic_cast<const PointerTypeExpr *>(declaration.params[0].type.get());
    if (!pointer || !pointer->pointeeMut || memory.kind != TypeRef::Kind::Pointer || memory.inner.empty() ||
        memory.inner.front().kind != TypeRef::Kind::UInt8) {
        reject(std::format("parameter '{}' has type '{}'", declaration.params[0].name, memory.ToString()));
        return;
    }
    const TypeRef length = ResolveType(*declaration.params[1].type);
    if (length.kind != TypeRef::Kind::UInt64) {
        reject(std::format("parameter '{}' has type '{}'", declaration.params[1].name, length.ToString()));
        return;
    }
    if (declaration.returnType) {
        const TypeRef returned = ResolveType(*declaration.returnType->get());
        reject(std::format("it returns '{}'", returned.ToString()));
    }
}
} // namespace Rux::SemanticDetail

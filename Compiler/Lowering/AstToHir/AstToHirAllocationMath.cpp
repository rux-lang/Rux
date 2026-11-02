// The integer conversions that keep the low bits, `Truncate` and `ConvertBits`, lowered where they are called.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <format>
#include <utility>

namespace Rux::AstToHirDetail {

HirExprPtr AstToHirContext::LowerTruncateCall(const CallExpr &call, const ResolvedCallableBinding &binding) {
    const auto *method = AstCast<const FuncDecl>(binding.selectedDeclaration);
    if (method && method->intrinsicName == "ConvertBits" && call.args.size() == 1) {
        // The generic form a generic body reaches the same conversion through.
        auto cast = std::make_unique<HirCastExpr>();
        cast->location = call.location;
        cast->targetType = ResolvedExpressionType(call);
        cast->type = cast->targetType;
        cast->operand = LowerExpr(*call.args[0]);
        return cast;
    }
    const auto *receiver = AstCast<const FieldExpr>(call.callee.get());
    if (!method || !method->intrinsicName.ends_with(".Truncate") || !receiver) {
        return nullptr;
    }
    // The conversion an `as` makes, without the range check: the low bits of the value at the target width, sign- or
    // zero-extended by the source's signedness when the target is wider.
    HirExprPtr value = LowerExpr(*receiver->object);
    // A receiver held behind a reference is read through it, as the method's own `&self` would.
    if (value->type->kind == TypeRef::Kind::Reference && !value->type->inner.empty()) {
        auto read = std::make_unique<HirUnaryExpr>();
        read->location = receiver->object->location;
        read->op = TokenKind::Star;
        read->type = value->type->inner.front();
        read->type.isMut = false;
        read->operand = std::move(value);
        value = std::move(read);
    }
    auto cast = std::make_unique<HirCastExpr>();
    cast->location = call.location;
    cast->targetType = ResolvedExpressionType(call);
    cast->type = cast->targetType;
    cast->operand = std::move(value);
    return cast;
}
} // namespace Rux::AstToHirDetail

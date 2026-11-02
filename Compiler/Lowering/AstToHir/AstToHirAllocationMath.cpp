// Checked arithmetic lowering. Each operation is emitted where it is called rather than through a runtime helper, so an
// allocation size costs the arithmetic itself plus one comparison, and the overflow report is a value the caller
// branches on. The integer conversions that keep the low bits, `Truncate` and `ConvertBits`, are lowered here too.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <format>
#include <utility>

namespace Rux::AstToHirDetail {
namespace {
/// The operands and the result are named, because each is read twice and the source wrote each expression once.
[[nodiscard]] std::string CheckedBindingName(const std::string_view role, const std::size_t ordinal) {
    return std::format("$checked.{}.{}", role, ordinal);
}
} // namespace

HirExprPtr AstToHirContext::LowerCheckedArithmeticCall(const std::string &intrinsicName, const CallExpr &call) {
    // Only the three checked operations are lowered here; any other intrinsic with three arguments is not one of them.
    if (intrinsicName != "CheckedAdd" && intrinsicName != "CheckedSub" && intrinsicName != "CheckedMul") {
        return nullptr;
    }
    if (call.args.size() != 3) {
        return nullptr;
    }

    const TypeRef valueType = TypeRef::MakeUInt64();
    const TypeRef flagType = TypeRef::MakeBool();
    const std::size_t ordinal = checkedArithmeticOrdinal++;
    const std::string leftName = CheckedBindingName("left", ordinal);
    const std::string rightName = CheckedBindingName("right", ordinal);
    const std::string resultName = CheckedBindingName("result", ordinal);
    const std::string valueName = CheckedBindingName("value", ordinal);

    const auto named = [&](const std::string &name, const TypeRef &type) {
        auto value = std::make_unique<HirVarExpr>();
        value->location = call.location;
        value->name = name;
        value->type = type;
        return value;
    };
    const auto bind = [&](const std::string &name, const TypeRef &type, HirExprPtr init) {
        auto declaration = std::make_unique<HirLetStmt>();
        declaration->location = call.location;
        declaration->name = name;
        declaration->type = type;
        declaration->init = std::move(init);
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Var;
        symbol.name = name;
        symbol.type = type;
        Define(symbol);
        return declaration;
    };
    const auto binary = [&](const TokenKind op, HirExprPtr left, HirExprPtr right, const TypeRef &type) {
        auto expression = std::make_unique<HirBinaryExpr>();
        expression->location = call.location;
        expression->op = op;
        expression->left = std::move(left);
        expression->right = std::move(right);
        expression->type = type;
        return expression;
    };

    auto block = std::make_unique<HirBlockExpr>();
    block->location = call.location;
    block->type = flagType;
    block->block.location = call.location;

    PushScope();
    const TypeRef resultType = TypeRef::MakePointer(valueType);
    block->block.stmts.push_back(bind(leftName, valueType, LowerExprAs(*call.args[0], valueType)));
    block->block.stmts.push_back(bind(rightName, valueType, LowerExprAs(*call.args[1], valueType)));
    block->block.stmts.push_back(bind(resultName, resultType, LowerExpr(*call.args[2])));

    // The operation wraps rather than trapping, which is what makes the report meaningful: the caller sees both the
    // value the machine produced and whether that value is the one the operands actually name.
    const TokenKind arithmetic = intrinsicName == "CheckedAdd" ? TokenKind::PlusPercent
                               : intrinsicName == "CheckedSub" ? TokenKind::MinusPercent
                                                               : TokenKind::StarPercent;
    block->block.stmts.push_back(bind(
        valueName, valueType, binary(arithmetic, named(leftName, valueType), named(rightName, valueType), valueType)));

    auto store = std::make_unique<HirAssignExpr>();
    store->location = call.location;
    store->op = TokenKind::Assign;
    auto target = std::make_unique<HirUnaryExpr>();
    target->location = call.location;
    target->op = TokenKind::Star;
    target->type = valueType;
    target->operand = named(resultName, resultType);
    store->target = std::move(target);
    store->value = named(valueName, valueType);
    store->type = valueType;
    auto storeStatement = std::make_unique<HirExprStmt>();
    storeStatement->location = call.location;
    storeStatement->expr = std::move(store);
    block->block.stmts.push_back(std::move(storeStatement));

    // An unsigned sum that wrapped is smaller than either operand; an unsigned difference wrapped exactly when the left
    // operand was the smaller one; a product wrapped when dividing it back does not return the other operand, which is
    // only asked once the left operand is known not to be zero.
    HirExprPtr overflowed;
    if (intrinsicName == "CheckedAdd") {
        overflowed = binary(TokenKind::Less, named(valueName, valueType), named(leftName, valueType), flagType);
    }
    else if (intrinsicName == "CheckedSub") {
        overflowed = binary(TokenKind::Less, named(leftName, valueType), named(rightName, valueType), flagType);
    }
    else {
        auto zero = std::make_unique<HirLiteralExpr>();
        zero->location = call.location;
        zero->type = valueType;
        zero->value = "0";
        auto nonZero = binary(TokenKind::BangEqual, named(leftName, valueType), std::move(zero), flagType);
        auto divided = binary(TokenKind::Slash, named(valueName, valueType), named(leftName, valueType), valueType);
        auto mismatched = binary(TokenKind::BangEqual, std::move(divided), named(rightName, valueType), flagType);
        overflowed = binary(TokenKind::AmpAmp, std::move(nonZero), std::move(mismatched), flagType);
    }
    block->value = std::move(overflowed);
    PopScope();
    return block;
}

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

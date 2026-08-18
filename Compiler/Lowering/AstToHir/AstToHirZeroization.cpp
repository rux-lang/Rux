// Zeroization lowering: a byte loop whose every write is marked as one that must happen, so the pass that removes
// stores nothing reads back leaves these alone.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <format>
#include <utility>

namespace Rux::AstToHirDetail {
namespace {
[[nodiscard]] std::string ZeroizeBindingName(const std::string_view role, const std::size_t ordinal) {
    return std::format("$zeroize.{}.{}", role, ordinal);
}
} // namespace

HirExprPtr AstToHirContext::LowerZeroizeCall(const CallExpr &call) {
    if (call.args.size() != 2) {
        return nullptr;
    }

    const TypeRef byteType = TypeRef::MakeUInt8();
    const TypeRef lengthType = TypeRef::MakeUInt64();
    const TypeRef memoryType = TypeRef::MakePointer(byteType);
    const std::size_t ordinal = zeroizationOrdinal++;
    const std::string memoryName = ZeroizeBindingName("memory", ordinal);
    const std::string lengthName = ZeroizeBindingName("length", ordinal);
    const std::string indexName = ZeroizeBindingName("index", ordinal);

    const auto named = [&](const std::string &name, const TypeRef &type) {
        auto value = std::make_unique<HirVarExpr>();
        value->location = call.location;
        value->name = name;
        value->type = type;
        return value;
    };
    const auto literal = [&](const std::string &text, const TypeRef &type) {
        auto value = std::make_unique<HirLiteralExpr>();
        value->location = call.location;
        value->value = text;
        value->type = type;
        return value;
    };
    const auto bind = [&](const std::string &name, const TypeRef &type, HirExprPtr init, const bool isMut) {
        auto declaration = std::make_unique<HirLetStmt>();
        declaration->location = call.location;
        declaration->isMut = isMut;
        declaration->name = name;
        declaration->type = type;
        declaration->init = std::move(init);
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Var;
        symbol.name = name;
        symbol.type = type;
        symbol.isMut = isMut;
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
    block->type = TypeRef::MakeOpaque();
    block->block.location = call.location;

    PushScope();
    block->block.stmts.push_back(bind(memoryName, memoryType, LowerExpr(*call.args[0]), false));
    block->block.stmts.push_back(bind(lengthName, lengthType, LowerExprAs(*call.args[1], lengthType), false));
    block->block.stmts.push_back(bind(indexName, lengthType, literal("0", lengthType), true));

    auto loop = std::make_unique<HirWhileStmt>();
    loop->location = call.location;
    loop->condition =
        binary(TokenKind::Less, named(indexName, lengthType), named(lengthName, lengthType), TypeRef::MakeBool());
    loop->body.location = call.location;

    // The write is the whole point of the call, so it says so about itself and survives every pass that would otherwise
    // notice that nothing reads the bytes back.
    auto element = std::make_unique<HirIndexExpr>();
    element->location = call.location;
    element->object = named(memoryName, memoryType);
    element->index = named(indexName, lengthType);
    element->type = byteType;
    auto clear = std::make_unique<HirAssignExpr>();
    clear->location = call.location;
    clear->op = TokenKind::Assign;
    clear->target = std::move(element);
    clear->value = literal("0", byteType);
    clear->type = byteType;
    clear->isVolatile = true;
    auto clearStatement = std::make_unique<HirExprStmt>();
    clearStatement->location = call.location;
    clearStatement->expr = std::move(clear);
    loop->body.stmts.push_back(std::move(clearStatement));

    auto step = std::make_unique<HirAssignExpr>();
    step->location = call.location;
    step->op = TokenKind::Assign;
    step->target = named(indexName, lengthType);
    step->value = binary(TokenKind::Plus, named(indexName, lengthType), literal("1", lengthType), lengthType);
    step->type = lengthType;
    auto stepStatement = std::make_unique<HirExprStmt>();
    stepStatement->location = call.location;
    stepStatement->expr = std::move(step);
    loop->body.stmts.push_back(std::move(stepStatement));

    block->block.stmts.push_back(std::move(loop));
    PopScope();
    return block;
}
} // namespace Rux::AstToHirDetail

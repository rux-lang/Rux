// Expression dispatch for AST-to-HIR lowering, plus the operator-overload path
// that has to run before an ordinary binary expression is lowered.

#include "Lowering/AstToHir/AstToHir.h"

#include "Ir/Hir/HirInternal.h"
#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace Rux {
// Operator → string
std::string_view OpStr(TokenKind op) {
    using TK = TokenKind;
    switch (op) {
    case TK::Plus:
        return "+";
    case TK::Minus:
        return "-";
    case TK::Star:
        return "*";
    case TK::Slash:
        return "/";
    case TK::Percent:
        return "%";
    case TK::StarStar:
        return "**";
    case TK::PlusPlus:
        return "++";
    case TK::MinusMinus:
        return "--";
    case TK::Amp:
        return "&";
    case TK::At:
        return "@";
    case TK::Pipe:
        return "|";
    case TK::Caret:
        return "^";
    case TK::Tilde:
        return "~";
    case TK::LessLess:
        return "<<";
    case TK::GreaterGreater:
        return ">>";
    case TK::GreaterGreaterGreater:
        return ">>>";
    case TK::AmpAmp:
        return "&&";
    case TK::PipePipe:
        return "||";
    case TK::Bang:
        return "!";
    case TK::Equal:
        return "==";
    case TK::BangEqual:
        return "!=";
    case TK::Less:
        return "<";
    case TK::LessEqual:
        return "<=";
    case TK::Greater:
        return ">";
    case TK::GreaterEqual:
        return ">=";
    case TK::Assign:
        return "=";
    case TK::PlusAssign:
        return "+=";
    case TK::MinusAssign:
        return "-=";
    case TK::StarAssign:
        return "*=";
    case TK::SlashAssign:
        return "/=";
    case TK::PercentAssign:
        return "%=";
    case TK::AmpAssign:
        return "&=";
    case TK::PipeAssign:
        return "|=";
    case TK::CaretAssign:
        return "^=";
    case TK::LessLessAssign:
        return "<<=";
    case TK::GreaterGreaterAssign:
        return ">>=";
    case TK::GreaterGreaterGreaterAssign:
        return ">>>=";
    default:
        return "?";
    }
}
} // namespace Rux

namespace Rux::AstToHirDetail {

namespace {
/// Name an expression's kind for an internal-error message, so a lowering failure says which node it could not handle.
std::string_view ExpressionKind(const Expr &expression) {
    if (dynamic_cast<const LiteralExpr *>(&expression))
        return "literal";
    if (dynamic_cast<const IdentExpr *>(&expression))
        return "identifier";
    if (dynamic_cast<const SelfExpr *>(&expression))
        return "self";
    if (dynamic_cast<const PathExpr *>(&expression))
        return "path";
    if (dynamic_cast<const SizeOfExpr *>(&expression))
        return "sizeof";
    if (dynamic_cast<const EnumShorthandExpr *>(&expression))
        return "enum shorthand";
    if (dynamic_cast<const IntrinsicExpr *>(&expression))
        return "intrinsic";
    if (dynamic_cast<const UnaryExpr *>(&expression))
        return "unary";
    if (dynamic_cast<const PostfixExpr *>(&expression))
        return "postfix";
    if (dynamic_cast<const BinaryExpr *>(&expression))
        return "binary";
    if (dynamic_cast<const AssignExpr *>(&expression))
        return "assignment";
    if (dynamic_cast<const TernaryExpr *>(&expression))
        return "ternary";
    if (dynamic_cast<const RangeExpr *>(&expression))
        return "range";
    if (dynamic_cast<const CallExpr *>(&expression))
        return "call";
    if (dynamic_cast<const IndexExpr *>(&expression))
        return "index";
    if (dynamic_cast<const FieldExpr *>(&expression))
        return "field";
    if (dynamic_cast<const StructInitExpr *>(&expression))
        return "struct initializer";
    if (dynamic_cast<const ArrayExpr *>(&expression))
        return "array";
    if (dynamic_cast<const SpreadExpr *>(&expression))
        return "spread";
    if (dynamic_cast<const TupleExpr *>(&expression))
        return "tuple";
    if (dynamic_cast<const CastExpr *>(&expression))
        return "cast";
    if (dynamic_cast<const IsExpr *>(&expression))
        return "type check";
    if (dynamic_cast<const BlockExpr *>(&expression))
        return "block";
    if (dynamic_cast<const MatchExpr *>(&expression))
        return "match";
    return "unknown";
}
} // namespace

void AstToHirContext::ReportUnsupportedExpression(const Expr &expression) {
    diagnostics.push_back({Diagnostic::Severity::Error,
                           currentFile,
                           expression.location,
                           std::format("cannot lower '{}' expression to HIR", ExpressionKind(expression)),
                           {"this is an internal compiler limitation; semantic analysis accepted an expression that "
                            "AST-to-HIR lowering does not support"},
                           "please report this compiler limitation with a minimal source example",
                           {}});
}

HirExprPtr AstToHirContext::TryLowerOverloadedBinary(const BinaryExpr &expression, HirExprPtr &left,
                                                     HirExprPtr &right) {
    const std::string opName = std::string(OpStr(expression.op));
    const FuncDecl *method = LookupMethod(left->type, opName, {right->type});

    // An operator a type does not declare is derived from one it does, so that declaring `==` and `<` is enough to
    // compare a type. A declared operator always wins, and both derivations below name each operand exactly once, so
    // neither introduces a second evaluation:
    //
    //   a != b  ->  !(a == b)
    //   a >  b  ->  b < a
    //
    // `<=` and `>=` would need `(a < b) || (a == b)`, which names both operands twice. Deriving that safely needs a
    // way to bind an operand to a temporary, which HIR does not yet offer, so semantic analysis asks for them to be
    // declared instead rather than risk evaluating a side-effecting operand twice.
    if (!method && expression.op == TokenKind::BangEqual) {
        if (const FuncDecl *equals = LookupMethod(left->type, "==", {right->type})) {
            HirExprPtr equality = LowerOverloadedBinaryCall(expression, left, right, *equals);
            equality->type = TypeRef::MakeBool();
            auto negated = std::make_unique<HirUnaryExpr>();
            negated->location = expression.location;
            negated->op = TokenKind::Bang;
            negated->type = TypeRef::MakeBool();
            negated->operand = std::move(equality);
            return negated;
        }
    }
    if (!method && expression.op == TokenKind::Greater) {
        // `b < a`, so the receiver is the right operand and the argument is the left one.
        if (const FuncDecl *lessThan = LookupMethod(right->type, "<", {left->type})) {
            return LowerOverloadedBinaryCall(expression, right, left, *lessThan);
        }
    }

    if (!method) {
        return nullptr;
    }
    return LowerOverloadedBinaryCall(expression, left, right, *method);
}

HirExprPtr AstToHirContext::LowerOverloadedBinaryCall(const BinaryExpr &expression, HirExprPtr &left, HirExprPtr &right,
                                                      const FuncDecl &resolved) {
    const FuncDecl *method = &resolved;

    const std::string receiverBase = NamedBaseTypeName(left->type);
    HirExprPtr selfArg;
    if (left->type.kind == TypeRef::Kind::Pointer) {
        selfArg = std::move(left);
    }
    else {
        auto address = std::make_unique<HirUnaryExpr>();
        address->location = left->location;
        address->op = TokenKind::At;
        address->type = TypeRef::MakePointer(left->type);
        address->operand = std::move(left);
        selfArg = std::move(address);
    }

    auto callee = std::make_unique<HirVarExpr>();
    callee->location = expression.location;
    callee->name = ConcreteMethodCalleeName(receiverBase, selfArg->type, *method);
    callee->type = MethodType(selfArg->type, *method);

    auto call = std::make_unique<HirCallExpr>();
    call->location = expression.location;
    call->isNoReturn = method->isNoReturn;
    call->type = callee->type.inner.empty() ? TypeRef::MakeUnknown() : callee->type.inner.back();
    call->callee = std::move(callee);
    call->args.push_back(std::move(selfArg));
    if (call->callee->type.inner.size() > 2) {
        const TypeRef &expectedType = call->callee->type.inner[1];
        if (UnsuffixedIntegerLiteralFits(*expression.right, expectedType)) {
            right->type = expectedType;
        }
        else if (IsNullLiteral(*expression.right) && expectedType.kind == TypeRef::Kind::Pointer) {
            right->type = expectedType;
            if (auto *literal = dynamic_cast<HirLiteralExpr *>(right.get())) {
                literal->value = "0";
            }
        }
    }
    call->args.push_back(std::move(right));
    return call;
}

HirExprPtr AstToHirContext::LowerExpr(const Expr &expr) {
    if (HirExprPtr basic = LowerBasicExpr(expr)) {
        return basic;
    }
    if (HirExprPtr aggregate = LowerAggregateExpr(expr)) {
        return aggregate;
    }
    if (auto *e = dynamic_cast<const CallExpr *>(&expr)) {
        return LowerCallExpr(*e);
    }
    if (auto *e = dynamic_cast<const BlockExpr *>(&expr)) {
        auto he = std::make_unique<HirBlockExpr>();
        he->location = e->location;
        he->block = LowerBlock(*e->block);
        return he;
    }
    if (auto *e = dynamic_cast<const SpreadExpr *>(&expr)) {
        return LowerExpr(*e->operand);
    }

    // Keep a poison value so a failed result remains safe to inspect. The
    // driver stops before optimization when the diagnostic is present.
    ReportUnsupportedExpression(expr);
    auto he = std::make_unique<HirLiteralExpr>();
    he->location = expr.location;
    he->value = "<expr>";
    return he;
}

} // namespace Rux::AstToHirDetail

namespace Rux {
AstToHirLowering::AstToHirLowering(const SemanticModel &model)
    : semanticModel(model) {
}

HirPackage AstToHirLowering::Generate() {
    diagnostics.clear();
    AstToHirDetail::AstToHirContext lowering(semanticModel, semanticModel.modules, semanticModel.compileTimeContext,
                                             diagnostics);
    return lowering.Run();
}

const std::vector<Diagnostic> &AstToHirLowering::Diagnostics() const noexcept {
    return diagnostics;
}
} // namespace Rux

#include "Lowering/AstToHir/AstToHir.h"

#include "Ir/Hir/HirInternal.h"
#include "Lowering/AstToHir/Detail/AstToHirContext.h"

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

HirExprPtr AstToHirContext::TryLowerOverloadedBinary(const BinaryExpr &expression, HirExprPtr &left,
                                                     HirExprPtr &right) {
    const std::string opName = std::string(OpStr(expression.op));
    const FuncDecl *method = LookupMethod(left->type, opName, {right->type});
    if (!method) {
        return nullptr;
    }

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

    // Fallback for unrecognized expression kinds
    auto he = std::make_unique<HirLiteralExpr>();
    he->location = expr.location;
    he->value = "<expr>";
    return he;
}

} // namespace Rux::AstToHirDetail

namespace Rux {
AstToHirLowering::AstToHirLowering(const SemanticModel &model)
    : semanticModel_(model) {
}

HirPackage AstToHirLowering::Generate() {
    AstToHirDetail::AstToHirContext lowering(semanticModel_, semanticModel_.modules, semanticModel_.compileTimeContext);
    return lowering.Run();
}
} // namespace Rux

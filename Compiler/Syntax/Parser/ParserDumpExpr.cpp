// Human-readable AST expression dumping.

#include "Syntax/Parser/Detail/AstDumpWriter.h"

#include <ostream>
#include <string_view>
#include <utility>

namespace Rux::ParserDumpDetail {
namespace {
[[nodiscard]] std::string TypeString(const TypeExpr *type) {
    return DeclarationPrinter::TypeString(type);
}

[[nodiscard]] std::string_view OperatorString(const TokenKind op) noexcept {
    switch (op) {
    case TokenKind::Plus:
        return "+";
    case TokenKind::Minus:
        return "-";
    case TokenKind::Star:
        return "*";
    case TokenKind::Slash:
        return "/";
    case TokenKind::Percent:
        return "%";
    case TokenKind::Amp:
        return "&";
    case TokenKind::At:
        return "@";
    case TokenKind::Pipe:
        return "|";
    case TokenKind::Caret:
        return "^";
    case TokenKind::Tilde:
        return "~";
    case TokenKind::LessLess:
        return "<<";
    case TokenKind::GreaterGreater:
        return ">>";
    case TokenKind::GreaterGreaterGreater:
        return ">>>";
    case TokenKind::AmpAmp:
        return "&&";
    case TokenKind::PipePipe:
        return "||";
    case TokenKind::QuestionQuestion:
        return "??";
    case TokenKind::Bang:
        return "!";
    case TokenKind::Equal:
        return "==";
    case TokenKind::BangEqual:
        return "!=";
    case TokenKind::Less:
        return "<";
    case TokenKind::LessEqual:
        return "<=";
    case TokenKind::Greater:
        return ">";
    case TokenKind::GreaterEqual:
        return ">=";
    case TokenKind::Assign:
        return "=";
    case TokenKind::MoveArrow:
        return "<-";
    case TokenKind::PlusAssign:
        return "+=";
    case TokenKind::MinusAssign:
        return "-=";
    case TokenKind::StarAssign:
        return "*=";
    case TokenKind::SlashAssign:
        return "/=";
    case TokenKind::PercentAssign:
        return "%=";
    case TokenKind::AmpAssign:
        return "&=";
    case TokenKind::PipeAssign:
        return "|=";
    case TokenKind::CaretAssign:
        return "^=";
    case TokenKind::LessLessAssign:
        return "<<=";
    case TokenKind::GreaterGreaterAssign:
        return ">>=";
    case TokenKind::GreaterGreaterGreaterAssign:
        return ">>>=";
    default:
        return "?";
    }
}
} // namespace

ExpressionPrinter::ExpressionPrinter(AstDumpWriter &inputWriter, BlockCallback blockCallback,
                                     PatternCallback patternCallback)
    : writer(inputWriter)
    , out(inputWriter.out)
    , indent(inputWriter.indent)
    , printBlock(std::move(blockCallback))
    , printPattern(std::move(patternCallback)) {
}

void ExpressionPrinter::Pad() const {
    writer.Pad();
}

void ExpressionPrinter::Print(const Expr &expression) {
    if (const auto *literal = AstCast<const LiteralExpr>(&expression)) {
        PrintLiteralExpr(*literal);
    }
    else if (const auto *identifier = AstCast<const IdentExpr>(&expression)) {
        Pad();
        out << "IdentExpr '" << identifier->name << "'\n";
    }
    else if (AstCast<const SelfExpr>(&expression) != nullptr) {
        Pad();
        out << "SelfExpr\n";
    }
    else if (const auto *path = AstCast<const PathExpr>(&expression)) {
        Pad();
        out << "PathExpr '";
        for (std::size_t index = 0; index < path->segments.size(); ++index) {
            if (index != 0) {
                out << "::";
            }
            out << path->segments[index];
        }
        out << "'\n";
    }
    else if (const auto *query = AstCast<const TypeQueryExpr>(&expression)) {
        Pad();
        out << (query->query == TypeQueryExpr::Query::Size ? "SizeOfExpr " : "AlignOfExpr ")
            << TypeString(query->type.get()) << '\n';
    }
    else if (const auto *shorthand = AstCast<const EnumShorthandExpr>(&expression)) {
        Pad();
        out << "EnumShorthandExpr '." << shorthand->variant << "'\n";
    }
    else if (const auto *intrinsic = AstCast<const IntrinsicExpr>(&expression)) {
        static constexpr const char *names[] = {"#source.line",        "#source.column",
                                                "#source.file",        "#source.fileName",
                                                "#source.filePath",    "#source.function",
                                                "#build.date",         "#build.time",
                                                "#source.module",      "#target.os",
                                                "#target.arch",        "#target.abi",
                                                "#target.endian",      "#target.pointerBits",
                                                "#target.dataModel",   "#target.objectFormat",
                                                "#target.triple",      "#target.HasFeature",
                                                "#build.profile",      "#build.mode",
                                                "#build.optimization", "#build.debugAssertions",
                                                "#build.debugInfo",    "#build.isTest",
                                                "#build.outputKind",   "#build.timestamp",
                                                "#compiler.version",   "#compiler.HasFeature",
                                                "#config.Get",         "#config.Has"};
        Pad();
        out << "IntrinsicExpr " << names[static_cast<int>(intrinsic->kind)] << '\n';
        ++indent;
        for (const auto &argument : intrinsic->args) {
            if (argument) {
                Print(*argument);
            }
        }
        --indent;
    }
    else if (const auto *unary = AstCast<const UnaryExpr>(&expression)) {
        Pad();
        out << "UnaryExpr " << OperatorString(unary->op) << '\n';
        ++indent;
        if (unary->operand) {
            Print(*unary->operand);
        }
        --indent;
    }
    else if (const auto *move = AstCast<const MoveExpr>(&expression)) {
        Pad();
        out << "MoveExpr <-\n";
        ++indent;
        if (move->operand) {
            Print(*move->operand);
        }
        --indent;
    }
    else if (const auto *tryExpression = AstCast<const TryExpr>(&expression)) {
        Pad();
        out << "TryExpr\n";
        ++indent;
        if (tryExpression->operand) {
            Print(*tryExpression->operand);
        }
        --indent;
    }
    else if (const auto *binary = AstCast<const BinaryExpr>(&expression)) {
        Pad();
        out << "BinaryExpr " << OperatorString(binary->op) << '\n';
        ++indent;
        if (binary->left) {
            Print(*binary->left);
        }
        if (binary->right) {
            Print(*binary->right);
        }
        --indent;
    }
    else if (const auto *assignment = AstCast<const AssignExpr>(&expression)) {
        Pad();
        out << "AssignExpr " << OperatorString(assignment->op) << '\n';
        ++indent;
        if (assignment->target) {
            Print(*assignment->target);
        }
        if (assignment->value) {
            Print(*assignment->value);
        }
        --indent;
    }
    else if (const auto *ternary = AstCast<const TernaryExpr>(&expression)) {
        Pad();
        out << "TernaryExpr\n";
        ++indent;
        Pad();
        out << "Condition\n";
        ++indent;
        if (ternary->condition) {
            Print(*ternary->condition);
        }
        --indent;
        Pad();
        out << "Then\n";
        ++indent;
        if (ternary->thenExpr) {
            Print(*ternary->thenExpr);
        }
        --indent;
        Pad();
        out << "Else\n";
        ++indent;
        if (ternary->elseExpr) {
            Print(*ternary->elseExpr);
        }
        --indent;
        --indent;
    }
    else if (const auto *range = AstCast<const RangeExpr>(&expression)) {
        Pad();
        out << "RangeExpr " << (range->inclusive ? "..." : "..") << '\n';
        ++indent;
        if (range->lo) {
            Print(*range->lo);
        }
        if (range->hi) {
            Print(*range->hi);
        }
        --indent;
    }
    else if (const auto *call = AstCast<const CallExpr>(&expression)) {
        Pad();
        out << "CallExpr\n";
        ++indent;
        Pad();
        out << "Callee\n";
        ++indent;
        if (call->callee) {
            Print(*call->callee);
        }
        --indent;
        if (!call->typeArgs.empty()) {
            Pad();
            out << "TypeArgs [";
            for (std::size_t index = 0; index < call->typeArgs.size(); ++index) {
                if (index != 0) {
                    out << ", ";
                }
                out << TypeString(call->typeArgs[index].get());
            }
            out << "]\n";
        }
        if (!call->args.empty()) {
            Pad();
            out << "Args [" << call->args.size() << "]\n";
            ++indent;
            for (const auto &argument : call->args) {
                if (argument) {
                    Print(*argument);
                }
            }
            --indent;
        }
        --indent;
    }
    else if (const auto *indexExpression = AstCast<const IndexExpr>(&expression)) {
        Pad();
        out << "IndexExpr\n";
        ++indent;
        if (indexExpression->object) {
            Print(*indexExpression->object);
        }
        if (indexExpression->index) {
            Print(*indexExpression->index);
        }
        --indent;
    }
    else if (const auto *fieldExpression = AstCast<const FieldExpr>(&expression)) {
        Pad();
        out << "FieldExpr '." << fieldExpression->field << "'\n";
        ++indent;
        if (fieldExpression->object) {
            Print(*fieldExpression->object);
        }
        --indent;
    }
    else if (const auto *structInitializer = AstCast<const StructInitExpr>(&expression)) {
        Pad();
        out << "StructInitExpr '" << structInitializer->typeName;
        if (!structInitializer->typeArgs.empty()) {
            out << "<";
            for (std::size_t index = 0; index < structInitializer->typeArgs.size(); ++index) {
                if (index != 0) {
                    out << ", ";
                }
                out << TypeString(structInitializer->typeArgs[index].get());
            }
            out << ">";
        }
        out << "'\n";
        ++indent;
        for (const auto &field : structInitializer->fields) {
            Pad();
            out << "." << field.name << " =\n";
            ++indent;
            if (field.value) {
                Print(*field.value);
            }
            --indent;
        }
        --indent;
    }
    else if (const auto *array = AstCast<const ArrayExpr>(&expression)) {
        Pad();
        out << "ArrayExpr [" << array->elements.size() << "]\n";
        ++indent;
        for (const auto &element : array->elements) {
            if (element) {
                Print(*element);
            }
        }
        --indent;
    }
    else if (const auto *repeat = AstCast<const ArrayRepeatExpr>(&expression)) {
        Pad();
        out << "ArrayRepeatExpr\n";
        ++indent;
        if (repeat->value) {
            Print(*repeat->value);
        }
        if (repeat->count) {
            Print(*repeat->count);
        }
        --indent;
    }
    else if (const auto *cast = AstCast<const CastExpr>(&expression)) {
        Pad();
        out << "CastExpr as " << TypeString(cast->type.get()) << '\n';
        ++indent;
        if (cast->operand) {
            Print(*cast->operand);
        }
        --indent;
    }
    else if (const auto *is = AstCast<const IsExpr>(&expression)) {
        Pad();
        out << "IsExpr is " << TypeString(is->type.get()) << '\n';
        ++indent;
        if (is->operand) {
            Print(*is->operand);
        }
        --indent;
    }
    else if (const auto *block = AstCast<const BlockExpr>(&expression)) {
        if (block->block) {
            printBlock(*block->block);
        }
    }
    else if (const auto *match = AstCast<const MatchExpr>(&expression)) {
        Pad();
        out << "MatchExpr\n";
        ++indent;
        if (match->subject) {
            Print(*match->subject);
        }
        for (const auto &arm : match->arms) {
            Pad();
            out << "Arm\n";
            ++indent;
            printPattern(*arm.pattern);
            if (arm.body) {
                Print(*arm.body);
            }
            --indent;
        }
        --indent;
    }
    else if (const auto *recovery = AstCast<const CatchExpr>(&expression)) {
        Pad();
        out << "CatchExpr\n";
        ++indent;
        if (recovery->subject) {
            Print(*recovery->subject);
        }
        for (const auto &arm : recovery->arms) {
            Pad();
            out << "Arm\n";
            ++indent;
            if (arm.pattern) {
                printPattern(*arm.pattern);
            }
            if (arm.body) {
                Print(*arm.body);
            }
            --indent;
        }
        --indent;
    }
    else if (const auto *mapped = AstCast<const MappedTryExpr>(&expression)) {
        Pad();
        out << "MappedTryExpr '" << mapped->binding << "'\n";
        ++indent;
        if (mapped->operand) {
            Print(*mapped->operand);
        }
        if (mapped->mapper) {
            Print(*mapped->mapper);
        }
        --indent;
    }
    else if (const auto *construct = AstCast<const NativeConstructExpr>(&expression)) {
        Pad();
        out << "NativeConstructExpr '."
            << (construct->kind == NativeConstructExpr::Kind::Success   ? "Success"
                : construct->kind == NativeConstructExpr::Kind::Failure ? "Failure"
                                                                        : "Some")
            << "'\n";
        ++indent;
        if (construct->operand) {
            Print(*construct->operand);
        }
        --indent;
    }
    else if (AstCast<const NoneExpr>(&expression)) {
        Pad();
        out << "NoneExpr\n";
    }
    else if (const auto *diverge = AstCast<const DivergeExpr>(&expression)) {
        Pad();
        static constexpr const char *kKeywords[] = {"return", "fail", "break", "continue"};
        out << "DivergeExpr '" << kKeywords[static_cast<int>(diverge->kind)] << "'";
        if (!diverge->label.empty()) {
            out << " " << diverge->label;
        }
        out << "\n";
        ++indent;
        if (diverge->value) {
            Print(*diverge->value);
        }
        --indent;
    }
}

void ExpressionPrinter::PrintLiteralExpr(const LiteralExpr &expression) const {
    Pad();
    out << "LiteralExpr (";
    switch (expression.token.kind) {
    case TokenKind::IntLiteral:
        out << "int";
        break;
    case TokenKind::FloatLiteral:
        out << "float";
        break;
    case TokenKind::StringLiteral:
        out << "string";
        break;
    case TokenKind::CharLiteral:
        out << "char32";
        break;
    case TokenKind::BoolLiteral:
        out << "bool8";
        break;
    case TokenKind::NullKeyword:
        out << "null";
        break;
    default:
        out << "?";
        break;
    }
    out << ") '" << expression.token.text << "'\n";
}
} // namespace Rux::ParserDumpDetail

// Human-readable AST dump (Parser::DumpAst).

#include "Syntax/Parser/Detail/AstDumpWriter.h"
#include "Syntax/Parser/Parser.h"

#include <format>
#include <fstream>
#include <ostream>
#include <string_view>
#include <vector>

namespace Rux::ParserDumpDetail {
AstDumpWriter::AstDumpWriter(std::ostream &output)
    : out(output) {
}

void AstDumpWriter::Pad() const {
    for (int index = 0; index < indent; ++index) {
        out << "  ";
    }
}
} // namespace Rux::ParserDumpDetail

namespace Rux {
// AstPrinter  –  human-readable tree dump
namespace {
class AstPrinter final : private ParserDumpDetail::AstDumpWriter {
public:
    explicit AstPrinter(std::ostream &output)
        : AstDumpWriter(output)
        , declarations(
              *this, [this](const Expr &expression) { PrintExpr(expression); },
              [this](const Block &block) { statements.PrintBlock(block); })
        , statements(
              *this, [this](const Expr &expression) { PrintExpr(expression); },
              [this](const Decl &declaration) { declarations.Print(declaration); }) {
    }

    void Print(const Module &mod) {
        out << "Module \"" << mod.name << "\"\n";
        ++indent;
        for (const auto &item : mod.items) {
            if (item) {
                declarations.Print(*item);
            }
        }
        --indent;
    }

private:
    ParserDumpDetail::DeclarationPrinter declarations;
    ParserDumpDetail::StatementPrinter statements;

    [[nodiscard]] static std::string TypeStr(const TypeExpr *type) {
        return ParserDumpDetail::DeclarationPrinter::TypeString(type);
    }

    static std::string_view OpStr(const TokenKind op) noexcept {
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
        case TokenKind::StarStar:
            return "**";
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

    // Expressions
    void PrintExpr(const Expr &expr) {
        if (const auto *litExpr = dynamic_cast<const LiteralExpr *>(&expr)) {
            PrintLiteralExpr(*litExpr);
        }
        else if (const auto *identExpr = dynamic_cast<const IdentExpr *>(&expr)) {
            Pad();
            out << "IdentExpr '" << identExpr->name << "'\n";
        }
        else if (const auto *selExpr = dynamic_cast<const SelfExpr *>(&expr)) {
            (void)selExpr;
            Pad();
            out << "SelfExpr\n";
        }
        else if (const auto *pathExpr = dynamic_cast<const PathExpr *>(&expr)) {
            Pad();
            out << "PathExpr '";
            for (std::size_t i = 0; i < pathExpr->segments.size(); ++i) {
                if (i) {
                    out << "::";
                }
                out << pathExpr->segments[i];
            }
            out << "'\n";
        }
        else if (const auto *sizeOfExpr = dynamic_cast<const SizeOfExpr *>(&expr)) {
            Pad();
            out << "SizeOfExpr " << TypeStr(sizeOfExpr->type.get()) << '\n';
        }
        else if (const auto *shorthand = dynamic_cast<const EnumShorthandExpr *>(&expr)) {
            Pad();
            out << "EnumShorthandExpr '." << shorthand->variant << "'\n";
        }
        else if (const auto *intr = dynamic_cast<const IntrinsicExpr *>(&expr)) {
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
            out << "IntrinsicExpr " << names[static_cast<int>(intr->kind)] << '\n';
            ++indent;
            for (const auto &arg : intr->args) {
                if (arg) {
                    PrintExpr(*arg);
                }
            }
            --indent;
        }
        else if (const auto *unaryExpr = dynamic_cast<const UnaryExpr *>(&expr)) {
            Pad();
            out << "UnaryExpr " << OpStr(unaryExpr->op) << '\n';
            ++indent;
            if (unaryExpr->operand) {
                PrintExpr(*unaryExpr->operand);
            }
            --indent;
        }
        else if (const auto *binaryExpr = dynamic_cast<const BinaryExpr *>(&expr)) {
            Pad();
            out << "BinaryExpr " << OpStr(binaryExpr->op) << '\n';
            ++indent;
            if (binaryExpr->left) {
                PrintExpr(*binaryExpr->left);
            }
            if (binaryExpr->right) {
                PrintExpr(*binaryExpr->right);
            }
            --indent;
        }
        else if (const auto *assignExpr = dynamic_cast<const AssignExpr *>(&expr)) {
            Pad();
            out << "AssignExpr " << OpStr(assignExpr->op) << '\n';
            ++indent;
            if (assignExpr->target) {
                PrintExpr(*assignExpr->target);
            }
            if (assignExpr->value) {
                PrintExpr(*assignExpr->value);
            }
            --indent;
        }
        else if (const auto *tern = dynamic_cast<const TernaryExpr *>(&expr)) {
            Pad();
            out << "TernaryExpr\n";
            ++indent;
            Pad();
            out << "Condition\n";
            ++indent;
            if (tern->condition) {
                PrintExpr(*tern->condition);
            }
            --indent;
            Pad();
            out << "Then\n";
            ++indent;
            if (tern->thenExpr) {
                PrintExpr(*tern->thenExpr);
            }
            --indent;
            Pad();
            out << "Else\n";
            ++indent;
            if (tern->elseExpr) {
                PrintExpr(*tern->elseExpr);
            }
            --indent;
            --indent;
        }
        else if (const auto *rng = dynamic_cast<const RangeExpr *>(&expr)) {
            Pad();
            out << "RangeExpr " << (rng->inclusive ? "..." : "..") << '\n';
            ++indent;
            if (rng->lo) {
                PrintExpr(*rng->lo);
            }
            if (rng->hi) {
                PrintExpr(*rng->hi);
            }
            --indent;
        }
        else if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
            Pad();
            out << "CallExpr\n";
            ++indent;
            Pad();
            out << "Callee\n";
            ++indent;
            if (call->callee) {
                PrintExpr(*call->callee);
            }
            --indent;
            if (!call->typeArgs.empty()) {
                Pad();
                out << "TypeArgs [";
                for (std::size_t i = 0; i < call->typeArgs.size(); ++i) {
                    if (i) {
                        out << ", ";
                    }
                    out << TypeStr(call->typeArgs[i].get());
                }
                out << "]\n";
            }
            if (!call->args.empty()) {
                Pad();
                out << "Args [" << call->args.size() << "]\n";
                ++indent;
                for (const auto &a : call->args) {
                    if (a) {
                        PrintExpr(*a);
                    }
                }
                --indent;
            }
            --indent;
        }
        else if (const auto *index = dynamic_cast<const IndexExpr *>(&expr)) {
            Pad();
            out << "IndexExpr\n";
            ++indent;
            if (index->object) {
                PrintExpr(*index->object);
            }
            if (index->index) {
                PrintExpr(*index->index);
            }
            --indent;
        }
        else if (const auto *fieldExpr = dynamic_cast<const FieldExpr *>(&expr)) {
            Pad();
            out << "FieldExpr '." << fieldExpr->field << "'\n";
            ++indent;
            if (fieldExpr->object) {
                PrintExpr(*fieldExpr->object);
            }
            --indent;
        }
        else if (const auto *structInitExpr = dynamic_cast<const StructInitExpr *>(&expr)) {
            Pad();
            out << "StructInitExpr '" << structInitExpr->typeName;
            if (!structInitExpr->typeArgs.empty()) {
                out << "<";
                for (std::size_t i = 0; i < structInitExpr->typeArgs.size(); ++i) {
                    if (i) {
                        out << ", ";
                    }
                    out << TypeStr(structInitExpr->typeArgs[i].get());
                }
                out << ">";
            }
            out << "'\n";
            ++indent;
            for (const auto &f : structInitExpr->fields) {
                Pad();
                out << "." << f.name << " =\n";
                ++indent;
                if (f.value) {
                    PrintExpr(*f.value);
                }
                --indent;
            }
            --indent;
        }
        else if (const auto *sliceExpr = dynamic_cast<const ArrayExpr *>(&expr)) {
            Pad();
            out << "ArrayExpr [" << sliceExpr->elements.size() << "]\n";
            ++indent;
            for (const auto &e : sliceExpr->elements) {
                if (e) {
                    PrintExpr(*e);
                }
            }
            --indent;
        }
        else if (const auto *castExpr = dynamic_cast<const CastExpr *>(&expr)) {
            Pad();
            out << "CastExpr as " << TypeStr(castExpr->type.get()) << '\n';
            ++indent;
            if (castExpr->operand) {
                PrintExpr(*castExpr->operand);
            }
            --indent;
        }
        else if (const auto *isExpr = dynamic_cast<const IsExpr *>(&expr)) {
            Pad();
            out << "IsExpr is " << TypeStr(isExpr->type.get()) << '\n';
            ++indent;
            if (isExpr->operand) {
                PrintExpr(*isExpr->operand);
            }
            --indent;
        }
        else if (const auto *blockExpr = dynamic_cast<const BlockExpr *>(&expr)) {
            if (blockExpr->block) {
                statements.PrintBlock(*blockExpr->block);
            }
        }
        else if (const auto *matchExpr = dynamic_cast<const MatchExpr *>(&expr)) {
            Pad();
            out << "MatchExpr\n";
            ++indent;
            if (matchExpr->subject) {
                PrintExpr(*matchExpr->subject);
            }
            for (const auto &arm : matchExpr->arms) {
                Pad();
                out << "Arm\n";
                ++indent;
                statements.PrintPattern(*arm.pattern);
                if (arm.body) {
                    PrintExpr(*arm.body);
                }
                --indent;
            }
            --indent;
        }
    }

    void PrintLiteralExpr(const LiteralExpr &e) const {
        Pad();
        out << "LiteralExpr (";
        switch (e.token.kind) {
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
        out << ") '" << e.token.text << "'\n";
    }
};
} // namespace

bool Parser::DumpAst(const ParseResult &result, const std::filesystem::path &path) {
    std::ofstream f(path);
    if (!f) {
        return false;
    }
    AstPrinter printer(f);
    printer.Print(result.module);
    if (!result.diagnostics.empty()) {
        f << "\n--- diagnostics ---\n";
        for (const auto &d : result.diagnostics) {
            f << std::format("{:>4}:{:<4}  {}  {}\n", d.location.line, d.location.column,
                             d.severity == ParserDiagnostic::Severity::Error ? "error  " : "warning", d.message);
        }
    }
    return f.good();
}
} // namespace Rux

// Human-readable AST dump (Parser::DumpAst).

#include "Syntax/Parser/Parser.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
// AstPrinter  –  human-readable tree dump
namespace {
class AstPrinter {
public:
    explicit AstPrinter(std::ostream &output)
        : out(output) {
    }

    void Print(const Module &mod) {
        out << "Module \"" << mod.name << "\"\n";
        ++indent;
        for (const auto &item : mod.items) {
            if (item) {
                PrintDecl(*item);
            }
        }
        --indent;
    }

private:
    std::ostream &out;
    int indent = 0;

    // Helpers
    void Pad() const {
        for (int i = 0; i < indent; ++i) {
            out << "  ";
        }
    }

    static std::string TypeStr(const TypeExpr *t) {
        if (!t) {
            return "<null>";
        }
        if (const auto *n = dynamic_cast<const NamedTypeExpr *>(t)) {
            std::string s = n->name;
            if (!n->typeArgs.empty()) {
                s += "<";
                for (std::size_t i = 0; i < n->typeArgs.size(); ++i) {
                    if (i) {
                        s += ", ";
                    }
                    s += TypeStr(n->typeArgs[i].get());
                }
                s += ">";
            }
            return s;
        }
        if (const auto *p = dynamic_cast<const PathTypeExpr *>(t)) {
            std::string s;
            for (std::size_t i = 0; i < p->segments.size(); ++i) {
                if (i) {
                    s += "::";
                }
                s += p->segments[i];
            }
            return s;
        }
        if (const auto *a = dynamic_cast<const SliceTypeExpr *>(t)) {
            std::string s = TypeStr(a->element.get()) + "[";
            if (a->size) {
                s += "N"; // size is an Expr, not easily stringified
            }
            return s + "]";
        }
        if (const auto *ptr = dynamic_cast<const PointerTypeExpr *>(t)) {
            return "*" + TypeStr(ptr->pointee.get());
        }
        if (const auto *tup = dynamic_cast<const TupleTypeExpr *>(t)) {
            std::string s = "(";
            for (std::size_t i = 0; i < tup->elements.size(); ++i) {
                if (i) {
                    s += ", ";
                }
                s += TypeStr(tup->elements[i].get());
            }
            return s + ")";
        }
        if (dynamic_cast<const SelfTypeExpr *>(t)) {
            return "self";
        }
        return "<type>";
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
        default:
            return "?";
        }
    }

    // Declarations

    void PrintDecl(const Decl &decl) {
        if (const auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            PrintFuncDecl(*fn);
        }
        else if (const auto *st = dynamic_cast<const StructDecl *>(&decl)) {
            PrintStructDecl(*st);
        }
        else if (const auto *en = dynamic_cast<const EnumDecl *>(&decl)) {
            PrintEnumDecl(*en);
        }
        else if (const auto *un = dynamic_cast<const UnionDecl *>(&decl)) {
            PrintUnionDecl(*un);
        }
        else if (const auto *iface = dynamic_cast<const InterfaceDecl *>(&decl)) {
            PrintInterfaceDecl(*iface);
        }
        else if (const auto *impl = dynamic_cast<const ImplDecl *>(&decl)) {
            PrintImplDecl(*impl);
        }
        else if (const auto *mod = dynamic_cast<const ModuleDecl *>(&decl)) {
            PrintModuleDecl(*mod);
        }
        else if (const auto *use = dynamic_cast<const UseDecl *>(&decl)) {
            PrintUseDecl(*use);
        }
        else if (const auto *cnst = dynamic_cast<const ConstDecl *>(&decl)) {
            PrintConstDecl(*cnst);
        }
        else if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&decl)) {
            PrintTypeAliasDecl(*alias);
        }
        else if (const auto *extFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
            PrintExternFuncDecl(*extFn);
        }
        else if (const auto *extVar = dynamic_cast<const ExternVarDecl *>(&decl)) {
            PrintExternVarDecl(*extVar);
        }
        else if (const auto *when = dynamic_cast<const WhenDecl *>(&decl)) {
            PrintWhenDecl(*when);
        }
    }

    void PrintWhenDecl(const WhenDecl &d) {
        Pad();
        out << "WhenDecl\n";
        ++indent;
        for (const auto &branch : d.branches) {
            Pad();
            out << (branch.condition ? "Branch\n" : "Else\n");
            ++indent;
            if (branch.condition) {
                Pad();
                out << "Condition\n";
                ++indent;
                PrintExpr(*branch.condition);
                --indent;
            }
            for (const auto &item : branch.items) {
                if (item) {
                    PrintDecl(*item);
                }
            }
            --indent;
        }
        --indent;
    }

    void PrintFuncDecl(const FuncDecl &f) {
        if (f.isNoReturn) {
            Pad();
            out << "#NoReturn()\n";
        }
        Pad();
        if (f.isPublic) {
            out << "pub ";
        }
        if (f.isAsm) {
            out << "asm ";
        }
        out << "FuncDecl '" << f.name << "'";
        // Generic params
        if (!f.typeParams.empty()) {
            out << '<';
            for (std::size_t i = 0; i < f.typeParams.size(); ++i) {
                if (i) {
                    out << ", ";
                }
                out << f.typeParams[i];
            }
            out << '>';
        }
        // Params
        out << " (";
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            if (i) {
                out << ", ";
            }
            const auto &p = f.params[i];
            if (p.isVariadic) {
                out << "...";
                continue;
            }
            out << p.name << ": " << TypeStr(p.type.get());
        }
        out << ')';
        // Return type
        if (f.returnType) {
            out << " -> " << TypeStr(f.returnType->get());
        }
        if (f.isAsm) {
            out << '\n';
            ++indent;
            for (const auto &instr : f.asmBody) {
                Pad();
                if (!instr.labelDef.empty()) {
                    out << instr.labelDef << ":\n";
                    continue;
                }
                out << instr.mnemonic;
                for (std::size_t i = 0; i < instr.operands.size(); ++i) {
                    out << (i == 0 ? " " : ", ");
                    PrintAsmOperand(instr.operands[i]);
                }
                out << '\n';
            }
            --indent;
            return;
        }
        out << (f.body ? "" : " [signature]") << '\n';
        if (f.body) {
            ++indent;
            PrintBlock(*f.body);
            --indent;
        }
    }

    void PrintAsmOperand(const AsmOperand &op) {
        switch (op.kind) {
        case AsmOperand::Kind::Reg:
        case AsmOperand::Kind::Sym:
            out << op.name;
            break;
        case AsmOperand::Kind::Imm:
            out << op.imm;
            break;
        case AsmOperand::Kind::Mem: {
            out << '[';
            bool wrote = false;
            if (!op.memBase.empty()) {
                out << op.memBase;
                wrote = true;
            }
            if (!op.memIndex.empty()) {
                out << (wrote ? " + " : "") << op.memIndex << '*' << op.memScale;
                wrote = true;
            }
            if (!op.memSym.empty()) {
                out << (wrote ? " + " : "") << op.memSym;
                wrote = true;
            }
            if (op.imm != 0 || !wrote) {
                out << (wrote && op.imm >= 0 ? " + " : (wrote ? " - " : "")) << (wrote ? std::abs(op.imm) : op.imm);
            }
            out << ']';
            break;
        }
        case AsmOperand::Kind::None:
            break;
        }
    }

    void PrintStructDecl(const StructDecl &s) {
        Pad();
        if (s.isPublic) {
            out << "pub ";
        }
        out << "StructDecl '" << s.name << "'";
        if (!s.typeParams.empty()) {
            out << '<';
            for (std::size_t i = 0; i < s.typeParams.size(); ++i) {
                if (i) {
                    out << ", ";
                }
                out << s.typeParams[i];
            }
            out << '>';
        }
        out << '\n';
        ++indent;
        for (const auto &f : s.fields) {
            Pad();
            if (f.isPublic) {
                out << "pub ";
            }
            out << "Field '" << f.name << "' : " << TypeStr(f.type.get()) << '\n';
        }
        --indent;
    }

    void PrintEnumDecl(const EnumDecl &e) {
        Pad();
        if (e.isPublic) {
            out << "pub ";
        }
        out << "EnumDecl '" << e.name << "'";
        if (e.baseType) {
            out << " : " << TypeStr(e.baseType.get());
        }
        out << '\n';
        ++indent;
        for (const auto &v : e.variants) {
            Pad();
            out << "Variant '" << v.name << "'";
            if (!v.fields.empty()) {
                out << " (";
                for (std::size_t i = 0; i < v.fields.size(); ++i) {
                    if (i) {
                        out << ", ";
                    }
                    out << TypeStr(v.fields[i].get());
                }
                out << ')';
            }
            if (!v.namedFields.empty()) {
                out << " { ";
                for (std::size_t i = 0; i < v.namedFields.size(); ++i) {
                    if (i) {
                        out << " ";
                    }
                    out << v.namedFields[i].name << ": " << TypeStr(v.namedFields[i].type.get()) << ";";
                }
                out << " }";
            }
            if (v.discriminant) {
                out << " = " << *v.discriminant;
            }
            out << '\n';
        }
        --indent;
    }

    void PrintUnionDecl(const UnionDecl &u) {
        Pad();
        if (u.isPublic) {
            out << "pub ";
        }
        out << "UnionDecl '" << u.name << "'\n";
        ++indent;
        for (const auto &f : u.fields) {
            Pad();
            out << "Field '" << f.name << "' : " << TypeStr(f.type.get()) << '\n';
        }
        --indent;
    }

    void PrintInterfaceDecl(const InterfaceDecl &iface) {
        Pad();
        if (iface.isPublic) {
            out << "pub ";
        }
        out << "InterfaceDecl '" << iface.name << "'\n";
        ++indent;
        for (const auto &m : iface.methods) {
            if (m) {
                PrintFuncDecl(*m);
            }
        }
        --indent;
    }

    void PrintImplDecl(const ImplDecl &impl) {
        Pad();
        out << "ImplDecl ";
        if (impl.interfaceName) {
            out << *impl.interfaceName << " for ";
        }
        out << impl.typeName << '\n';
        ++indent;
        for (const auto &m : impl.methods) {
            if (m) {
                PrintFuncDecl(*m);
            }
        }
        for (const auto &conditional : impl.conditionals) {
            if (conditional) {
                PrintWhenDecl(*conditional);
            }
        }
        --indent;
    }

    void PrintModuleDecl(const ModuleDecl &mod) {
        Pad();
        if (mod.isPublic) {
            out << "pub ";
        }
        out << "ModuleDecl '" << mod.name << "'\n";
        ++indent;
        for (const auto &item : mod.items) {
            if (item) {
                PrintDecl(*item);
            }
        }
        --indent;
    }

    void PrintUseDecl(const UseDecl &u) const {
        Pad();
        out << "ImportDecl '";
        for (std::size_t i = 0; i < u.path.size(); ++i) {
            if (i) {
                out << '.';
            }
            out << u.path[i];
        }
        switch (u.kind) {
        case UseDecl::Kind::Glob:
            out << ".*";
            break;
        case UseDecl::Kind::Multi: {
            out << "::{";
            for (std::size_t i = 0; i < u.names.size(); ++i) {
                if (i) {
                    out << ", ";
                }
                out << u.names[i];
            }
            out << '}';
            break;
        }
        default:
            break;
        }
        out << "'\n";
    }

    void PrintConstDecl(const ConstDecl &c) {
        Pad();
        if (c.isPublic) {
            out << "pub ";
        }
        out << "ConstDecl '" << c.name << "'";
        if (c.type) {
            out << " : " << TypeStr(c.type->get());
        }
        out << '\n';
        ++indent;
        if (c.value) {
            PrintExpr(*c.value);
        }
        --indent;
    }

    void PrintTypeAliasDecl(const TypeAliasDecl &t) const {
        Pad();
        if (t.isPublic) {
            out << "pub ";
        }
        out << "TypeAliasDecl '" << t.name << "' = " << TypeStr(t.type.get()) << '\n';
    }

    void PrintExternFuncDecl(const ExternFuncDecl &f) const {
        if (f.isNoReturn) {
            Pad();
            out << "#NoReturn()\n";
        }
        if (!f.dll.empty()) {
            Pad();
            out << "#Link(\"" << f.dll << "\"";
            if (!f.symbolName.empty()) {
                out << ", \"" << f.symbolName << "\"";
            }
            out << ")\n";
        }
        if (f.callConv != CallingConvention::Default) {
            Pad();
            out << "#Abi(" << ConventionName(f.callConv) << ")\n";
        }
        Pad();
        if (f.isPublic) {
            out << "pub ";
        }
        out << "ExternFuncDecl '" << f.name << "' (";
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            if (i) {
                out << ", ";
            }
            out << f.params[i].name << ": " << TypeStr(f.params[i].type.get());
        }
        if (f.isVariadic) {
            out << (f.params.empty() ? "..." : ", ...");
        }
        out << ')';
        if (f.returnType) {
            out << " -> " << TypeStr(f.returnType->get());
        }
        out << '\n';
    }

    void PrintExternVarDecl(const ExternVarDecl &v) const {
        Pad();
        if (v.isPublic) {
            out << "pub ";
        }
        out << "ExternVarDecl '" << v.name << "' : " << TypeStr(v.type.get()) << '\n';
    }

    // Block
    void PrintBlock(const Block &block) {
        Pad();
        out << "Block [" << block.stmts.size() << " stmt" << (block.stmts.size() == 1 ? "" : "s") << "]\n";
        ++indent;
        for (const auto &stmt : block.stmts) {
            if (stmt) {
                PrintStmt(*stmt);
            }
        }
        --indent;
    }

    // Statements
    void PrintStmt(const Stmt &stmt) {
        if (const auto *let = dynamic_cast<const LetStmt *>(&stmt)) {
            PrintLetStmt(*let);
        }
        else if (const auto *ifStmt = dynamic_cast<const IfStmt *>(&stmt)) {
            PrintIfStmt(*ifStmt);
        }
        else if (const auto *whileStmt = dynamic_cast<const WhileStmt *>(&stmt)) {
            PrintWhileStmt(*whileStmt);
        }
        else if (const auto *forStmt = dynamic_cast<const ForStmt *>(&stmt)) {
            PrintForStmt(*forStmt);
        }
        else if (const auto *matchStmt = dynamic_cast<const MatchStmt *>(&stmt)) {
            PrintMatchStmt(*matchStmt);
        }
        else if (const auto *ret = dynamic_cast<const ReturnStmt *>(&stmt)) {
            PrintReturnStmt(*ret);
        }
        else if (dynamic_cast<const BreakStmt *>(&stmt)) {
            Pad();
            out << "BreakStmt\n";
        }
        else if (dynamic_cast<const ContinueStmt *>(&stmt)) {
            Pad();
            out << "ContinueStmt\n";
        }
        else if (const auto *exprStmt = dynamic_cast<const ExprStmt *>(&stmt)) {
            Pad();
            out << "ExprStmt\n";
            ++indent;
            if (exprStmt->expr) {
                PrintExpr(*exprStmt->expr);
            }
            --indent;
        }
        else if (const auto *declStmt = dynamic_cast<const DeclStmt *>(&stmt)) {
            if (declStmt->decl) {
                PrintDecl(*declStmt->decl);
            }
        }
    }

    void PrintLetStmt(const LetStmt &s) {
        Pad();
        out << "LetStmt '";
        if (s.pattern) {
            out << "<pattern>";
        }
        else {
            out << s.name;
        }
        out << "' (" << (s.isMut ? "var" : "let") << ")";
        if (s.type) {
            out << " : " << TypeStr(s.type->get());
        }
        out << '\n';
        ++indent;
        if (s.pattern) {
            PrintPattern(*s.pattern);
        }
        if (s.init) {
            PrintExpr(*s.init);
        }
        --indent;
    }

    void PrintIfStmt(const IfStmt &s) {
        Pad();
        out << (s.isCompileTime ? "WhenStmt\n" : "IfStmt\n");
        ++indent;

        Pad();
        out << "Condition\n";
        ++indent;
        if (s.condition) {
            PrintExpr(*s.condition);
        }
        --indent;

        Pad();
        out << "Then\n";
        ++indent;
        if (s.thenBlock) {
            PrintBlock(*s.thenBlock);
        }
        --indent;

        for (const auto &elif : s.elseIfs) {
            Pad();
            out << "ElseIf\n";
            ++indent;
            Pad();
            out << "Condition\n";
            ++indent;
            if (elif.condition) {
                PrintExpr(*elif.condition);
            }
            --indent;
            if (elif.block) {
                PrintBlock(*elif.block);
            }
            --indent;
        }

        if (s.elseBlock) {
            Pad();
            out << "Else\n";
            ++indent;
            PrintBlock(*s.elseBlock);
            --indent;
        }
        --indent;
    }

    void PrintWhileStmt(const WhileStmt &s) {
        Pad();
        out << "WhileStmt\n";
        ++indent;
        Pad();
        out << "Condition\n";
        ++indent;
        if (s.condition) {
            PrintExpr(*s.condition);
        }
        --indent;
        if (s.body) {
            PrintBlock(*s.body);
        }
        --indent;
    }

    void PrintForStmt(const ForStmt &s) {
        Pad();
        out << "ForStmt '" << s.variable << "' in\n";
        ++indent;
        if (s.iterable) {
            PrintExpr(*s.iterable);
        }
        if (s.body) {
            PrintBlock(*s.body);
        }
        --indent;
    }

    void PrintMatchStmt(const MatchStmt &s) {
        Pad();
        out << "MatchStmt\n";
        ++indent;
        Pad();
        out << "Subject\n";
        ++indent;
        if (s.subject) {
            PrintExpr(*s.subject);
        }
        --indent;
        for (const auto &arm : s.arms) {
            Pad();
            out << "Arm\n";
            ++indent;
            if (arm.pattern) {
                PrintPattern(*arm.pattern);
            }
            if (arm.body) {
                PrintExpr(*arm.body);
            }
            --indent;
        }
        --indent;
    }

    void PrintReturnStmt(const ReturnStmt &s) {
        Pad();
        out << "ReturnStmt\n";
        if (s.value) {
            ++indent;
            PrintExpr(**s.value);
            --indent;
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
        else if (const auto *sliceExpr = dynamic_cast<const SliceExpr *>(&expr)) {
            Pad();
            out << "SliceExpr [" << sliceExpr->elements.size() << "]\n";
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
                PrintBlock(*blockExpr->block);
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
                PrintPattern(*arm.pattern);
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

    // Patterns
    void PrintPattern(const Pattern &pat) {
        if (dynamic_cast<const WildcardPattern *>(&pat)) {
            Pad();
            out << "WildcardPattern\n";
        }
        else if (const auto *litPat = dynamic_cast<const LiteralPattern *>(&pat)) {
            Pad();
            out << "LiteralPattern '" << litPat->value.text << "'\n";
        }
        else if (const auto *idPat = dynamic_cast<const IdentPattern *>(&pat)) {
            Pad();
            out << "IdentPattern '" << idPat->name << "'\n";
        }
        else if (const auto *rngPat = dynamic_cast<const RangePattern *>(&pat)) {
            Pad();
            out << "RangePattern " << (rngPat->inclusive ? "..." : "..") << '\n';
            ++indent;
            if (rngPat->lo) {
                PrintPattern(*rngPat->lo);
            }
            if (rngPat->hi) {
                PrintPattern(*rngPat->hi);
            }
            --indent;
        }
        else if (const auto *enumPat = dynamic_cast<const EnumPattern *>(&pat)) {
            Pad();
            out << "EnumPattern '";
            for (std::size_t i = 0; i < enumPat->path.size(); ++i) {
                if (i) {
                    out << '.';
                }
                out << enumPat->path[i];
            }
            out << "'";
            if (!enumPat->args.empty()) {
                out << " [" << enumPat->args.size() << " bindings]";
            }
            if (!enumPat->namedArgs.empty()) {
                out << " [" << enumPat->namedArgs.size() << " fields]";
            }
            out << '\n';
            if (!enumPat->args.empty() || !enumPat->namedArgs.empty()) {
                ++indent;
                for (const auto &a : enumPat->args) {
                    if (a) {
                        PrintPattern(*a);
                    }
                }
                for (const auto &a : enumPat->namedArgs) {
                    Pad();
                    out << "." << a.name << ":\n";
                    ++indent;
                    if (a.pattern) {
                        PrintPattern(*a.pattern);
                    }
                    --indent;
                }
                --indent;
            }
        }
        else if (const auto *structPat = dynamic_cast<const StructPattern *>(&pat)) {
            Pad();
            out << "StructPattern '" << structPat->typeName << "'\n";
            ++indent;
            for (const auto &f : structPat->fields) {
                Pad();
                out << "." << f.name << ":\n";
                ++indent;
                if (f.pattern) {
                    PrintPattern(*f.pattern);
                }
                --indent;
            }
            --indent;
        }
        else if (const auto *tuplePat = dynamic_cast<const TuplePattern *>(&pat)) {
            Pad();
            out << "TuplePattern [" << tuplePat->elements.size() << "]\n";
            ++indent;
            for (const auto &e : tuplePat->elements) {
                if (e) {
                    PrintPattern(*e);
                }
            }
            --indent;
        }
        else if (const auto *guardedPat = dynamic_cast<const GuardedPattern *>(&pat)) {
            Pad();
            out << "GuardedPattern\n";
            ++indent;
            if (guardedPat->inner) {
                PrintPattern(*guardedPat->inner);
            }
            Pad();
            out << "Guard\n";
            ++indent;
            if (guardedPat->guard) {
                PrintExpr(*guardedPat->guard);
            }
            --indent;
            --indent;
        }
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

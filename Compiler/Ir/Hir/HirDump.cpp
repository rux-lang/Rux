// Human-readable HIR dump.

#include "Ir/Hir/HirInternal.h"
#include "Ir/Hir/HirPrinter.h"

#include <format>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>

namespace Rux {
// Dump
static std::string PrintPattern(const HirPattern &pat);
static std::string PrintExpr(const HirExpr &expr);

static std::string PrintExpr(const HirExpr &expr) {
    if (auto *e = dynamic_cast<const HirLiteralExpr *>(&expr)) {
        return e->value;
    }
    if (auto *e = dynamic_cast<const HirVarExpr *>(&expr)) {
        return e->name;
    }
    if (dynamic_cast<const HirSelfExpr *>(&expr)) {
        return "self";
    }
    if (auto *e = dynamic_cast<const HirPathExpr *>(&expr)) {
        std::string s;
        for (std::size_t i = 0; i < e->segments.size(); ++i) {
            if (i) {
                s += "::";
            }
            s += e->segments[i];
        }
        return s;
    }
    if (auto *e = dynamic_cast<const HirUnaryExpr *>(&expr)) {
        return std::string(OpStr(e->op)) + PrintExpr(*e->operand);
    }
    if (auto *e = dynamic_cast<const HirPostfixExpr *>(&expr)) {
        return PrintExpr(*e->operand) + (e->op == TokenKind::PlusPlus ? "++" : "--");
    }
    if (auto *e = dynamic_cast<const HirBinaryExpr *>(&expr)) {
        return std::format("({} {} {})", PrintExpr(*e->left), OpStr(e->op), PrintExpr(*e->right));
    }
    if (auto *e = dynamic_cast<const HirAssignExpr *>(&expr)) {
        return std::format("{} {} {}", PrintExpr(*e->target), OpStr(e->op), PrintExpr(*e->value));
    }
    if (auto *e = dynamic_cast<const HirTernaryExpr *>(&expr)) {
        return std::format("{} ? {} : {}", PrintExpr(*e->condition), PrintExpr(*e->thenExpr), PrintExpr(*e->elseExpr));
    }
    if (auto *e = dynamic_cast<const HirRangeExpr *>(&expr)) {
        std::string lo = e->lo ? PrintExpr(*e->lo) : "";
        std::string hi = e->hi ? PrintExpr(*e->hi) : "";
        return lo + (e->inclusive ? "..." : "..") + hi;
    }
    if (auto *e = dynamic_cast<const HirCallExpr *>(&expr)) {
        std::string s = PrintExpr(*e->callee) + "(";
        for (std::size_t i = 0; i < e->args.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintExpr(*e->args[i]);
        }
        return s + ")";
    }
    if (auto *e = dynamic_cast<const HirIndexExpr *>(&expr)) {
        return std::format("{}[{}]", PrintExpr(*e->object), PrintExpr(*e->index));
    }
    if (auto *e = dynamic_cast<const HirFieldExpr *>(&expr)) {
        return std::format("{}.{}", PrintExpr(*e->object), e->field);
    }
    if (auto *e = dynamic_cast<const HirStructInitExpr *>(&expr)) {
        std::string s = e->typeName + " { ";
        for (std::size_t i = 0; i < e->fields.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += e->fields[i].name + ": " + PrintExpr(*e->fields[i].value);
        }
        return s + " }";
    }
    if (auto *e = dynamic_cast<const HirArrayExpr *>(&expr)) {
        std::string s = "[";
        for (std::size_t i = 0; i < e->elements.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintExpr(*e->elements[i]);
        }
        return s + "]";
    }
    if (auto *e = dynamic_cast<const HirTupleExpr *>(&expr)) {
        std::string s = "(";
        for (std::size_t i = 0; i < e->elements.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintExpr(*e->elements[i]);
        }
        return s + ")";
    }
    if (auto *e = dynamic_cast<const HirCastExpr *>(&expr)) {
        return std::format("{} as {}", PrintExpr(*e->operand), e->targetType.ToString());
    }
    if (auto *e = dynamic_cast<const HirIsExpr *>(&expr)) {
        return std::format("{} is {}", PrintExpr(*e->operand), e->checkType.ToString());
    }
    if (auto *e = dynamic_cast<const HirMatchExpr *>(&expr)) {
        std::string s = "match " + PrintExpr(*e->subject) + " { ";
        for (std::size_t i = 0; i < e->arms.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintPattern(*e->arms[i].pattern) + " => " + PrintExpr(*e->arms[i].body);
        }
        return s + " }";
    }
    if (auto *e = dynamic_cast<const HirEnumConstructExpr *>(&expr)) {
        std::string s = "#(";
        for (std::size_t i = 0; i < e->payloads.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintExpr(*e->payloads[i]);
        }
        return s + ")#" + e->discriminant;
    }
    if (dynamic_cast<const HirBlockExpr *>(&expr)) {
        return "{ ... }";
    }
    return "<expr>";
}

static std::string PrintPattern(const HirPattern &pat) {
    if (dynamic_cast<const HirWildcardPattern *>(&pat)) {
        return "_";
    }
    if (auto *p = dynamic_cast<const HirLiteralPattern *>(&pat)) {
        return p->value;
    }
    if (auto *p = dynamic_cast<const HirBindingPattern *>(&pat)) {
        return p->name;
    }
    if (auto *p = dynamic_cast<const HirRangePattern *>(&pat)) {
        std::string lo = p->lo ? PrintPattern(*p->lo) : "";
        std::string hi = p->hi ? PrintPattern(*p->hi) : "";
        return lo + (p->inclusive ? "..." : "..") + hi;
    }
    if (auto *p = dynamic_cast<const HirEnumPattern *>(&pat)) {
        std::string s;
        for (std::size_t i = 0; i < p->path.size(); ++i) {
            if (i) {
                s += "::";
            }
            s += p->path[i];
        }
        if (!p->args.empty()) {
            s += "(";
            for (std::size_t i = 0; i < p->args.size(); ++i) {
                if (i) {
                    s += ", ";
                }
                s += PrintPattern(*p->args[i]);
            }
            s += ")";
        }
        return s;
    }

    if (auto *p = dynamic_cast<const HirStructPattern *>(&pat)) {
        std::string s = p->typeName + " { ";
        for (std::size_t i = 0; i < p->fields.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += p->fields[i].name + ": " + PrintPattern(*p->fields[i].pattern);
        }
        return s + " }";
    }
    if (auto *p = dynamic_cast<const HirTuplePattern *>(&pat)) {
        std::string s = "(";
        for (std::size_t i = 0; i < p->elements.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintPattern(*p->elements[i]);
        }
        return s + ")";
    }
    if (auto *p = dynamic_cast<const HirGuardedPattern *>(&pat)) {
        return PrintPattern(*p->inner) + " if " + PrintExpr(*p->guard);
    }
    return "_";
}

static void DumpBlock(std::ostream &out, const HirBlock &block, const std::string &indent);

static void DumpStmt(std::ostream &out, const HirStmt &stmt, const std::string &indent);

static void DumpBlock(std::ostream &out, const HirBlock &block, const std::string &indent) {
    for (const auto &stmt : block.stmts) {
        DumpStmt(out, *stmt, indent);
    }
}

static void DumpStmt(std::ostream &out, const HirStmt &stmt, const std::string &indent) {
    if (auto *s = dynamic_cast<const HirExprStmt *>(&stmt)) {
        out << indent << PrintExpr(*s->expr) << '\n';
        return;
    }
    if (auto *s = dynamic_cast<const HirLetStmt *>(&stmt)) {
        out << std::format("{}{} {}: {}", indent, s->isMut ? "let mut" : "let",
                           s->pattern ? PrintPattern(*s->pattern) : s->name, s->type.ToString());
        if (s->init) {
            out << " = " << PrintExpr(*s->init);
        }
        out << '\n';
        return;
    }
    if (auto *s = dynamic_cast<const HirIfStmt *>(&stmt)) {
        out << std::format("{}if {}\n", indent, PrintExpr(*s->condition));
        DumpBlock(out, s->thenBlock, indent + "  ");
        for (const auto &elif : s->elseIfs) {
            out << std::format("{}else if {}\n", indent, PrintExpr(*elif.condition));
            DumpBlock(out, elif.block, indent + "  ");
        }
        if (s->elseBlock) {
            out << indent << "else\n";
            DumpBlock(out, *s->elseBlock, indent + "  ");
        }
        return;
    }
    if (auto *s = dynamic_cast<const HirWhileStmt *>(&stmt)) {
        out << std::format("{}while {}\n", indent, PrintExpr(*s->condition));
        DumpBlock(out, s->body, indent + "  ");
        return;
    }
    if (auto *s = dynamic_cast<const HirDoWhileStmt *>(&stmt)) {
        out << std::format("{}do\n", indent);
        DumpBlock(out, s->body, indent + "  ");
        out << std::format("{}while {}\n", indent, PrintExpr(*s->condition));
        return;
    }
    if (auto *s = dynamic_cast<const HirLoopStmt *>(&stmt)) {
        out << std::format("{}loop\n", indent);
        DumpBlock(out, s->body, indent + "  ");
        return;
    }
    if (auto *s = dynamic_cast<const HirForStmt *>(&stmt)) {
        out << std::format("{}for {} in {}\n", indent, s->variable, PrintExpr(*s->iterable));
        DumpBlock(out, s->body, indent + "  ");
        return;
    }
    if (auto *s = dynamic_cast<const HirMatchStmt *>(&stmt)) {
        out << std::format("{}match {}\n", indent, PrintExpr(*s->subject));
        for (const auto &arm : s->arms) {
            out << std::format("{}  {} =>\n", indent, PrintPattern(*arm.pattern));
            out << std::format("{}    {}\n", indent, PrintExpr(*arm.body));
        }
        return;
    }
    if (auto *s = dynamic_cast<const HirReturnStmt *>(&stmt)) {
        if (s->value) {
            out << std::format("{}return {}\n", indent, PrintExpr(**s->value));
        }
        else {
            out << indent << "return\n";
        }
        return;
    }
    if (auto *s = dynamic_cast<const HirBreakStmt *>(&stmt)) {
        out << indent << (s->label.empty() ? "break" : "break " + s->label) << "\n";
        return;
    }
    if (auto *s = dynamic_cast<const HirContinueStmt *>(&stmt)) {
        out << indent << (s->label.empty() ? "continue" : "continue " + s->label) << "\n";
        return;
    }
    if (auto *s = dynamic_cast<const HirLocalDecl *>(&stmt)) {
        out << std::format("{}[local {}]\n", indent, s->description);
        return;
    }
}

static void DumpFuncSignature(std::ostream &out, const HirFunc &f, const std::string &prefix = "") {
    if (f.isNoReturn) {
        out << prefix << "#NoReturn()\n";
    }
    std::string pub = f.isPublic ? "pub " : "";
    std::string asm_ = f.isAsm ? "asm " : "";
    std::string tps;
    if (!f.typeParams.empty()) {
        tps = "<";
        for (std::size_t i = 0; i < f.typeParams.size(); ++i) {
            if (i) {
                tps += ", ";
            }
            tps += f.typeParams[i];
        }
        tps += ">";
    }
    std::string params;
    for (std::size_t i = 0; i < f.params.size(); ++i) {
        if (i) {
            params += ", ";
        }
        if (f.params[i].isVariadic) {
            params += "...";
        }
        else {
            params += f.params[i].name + ": " + f.params[i].type.ToString();
        }
    }
    std::string ret = f.returnType.IsOpaque() ? "" : " -> " + f.returnType.ToString();
    out << std::format("{}{}{}func {}{}{}{}\n", prefix, pub, asm_, f.name, tps,
                       params.empty() ? "()" : "(" + params + ")", ret);
}

bool HirPrinter::Dump(const HirPackage &package, const std::filesystem::path &path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << "=== High-level Intermediate Representation ===\n";
    for (const auto &mod : package.modules) {
        out << '\n';
        out << std::format("Module \"{}\"\n", mod.name);
        out << std::string(std::min<std::size_t>(mod.name.size() + 9, 72), '-') << '\n';
        for (const auto &c : mod.consts) {
            std::string pub = c.isPublic ? "pub " : "";
            out << std::format("\n{}const {}: {} = {}\n", pub, c.name, c.type.ToString(), PrintExpr(*c.value));
        }
        for (const auto &ta : mod.typeAliases) {
            std::string pub = ta.isPublic ? "pub " : "";
            out << std::format("\n{}type {} = {}\n", pub, ta.name, ta.type.ToString());
        }
        for (const auto &ev : mod.externVars) {
            std::string pub = ev.isPublic ? "pub " : "";
            out << std::format("\nextern {}{}: {}\n", pub, ev.name, ev.type.ToString());
        }
        for (const auto &ef : mod.externFuncs) {
            std::string pub = ef.isPublic ? "pub " : "";
            std::string params;
            for (std::size_t i = 0; i < ef.params.size(); ++i) {
                if (i) {
                    params += ", ";
                }
                if (ef.params[i].isVariadic) {
                    params += "...";
                }
                else {
                    params += ef.params[i].name + ": " + ef.params[i].type.ToString();
                }
            }
            if (ef.isVariadic && !ef.params.empty()) {
                params += ", ...";
            }
            std::string ret = ef.returnType.IsOpaque() ? "" : " -> " + ef.returnType.ToString();
            std::string attr;
            if (ef.isNoReturn) {
                attr += "#NoReturn()\n";
            }
            if (!ef.dll.empty()) {
                if (ef.symbolName.empty()) {
                    attr += std::format("#Link(\"{}\")\n", ef.dll);
                }
                else {
                    attr += std::format("#Link(\"{}\", \"{}\")\n", ef.dll, ef.symbolName);
                }
            }
            if (ef.callConv != CallingConvention::Default) {
                attr += std::format("#Abi({})\n", ConventionName(ef.callConv));
            }
            out << std::format("\n{}extern {}func {}({}){}\n", attr, pub, ef.name, params, ret);
        }
        for (const auto &s : mod.structs) {
            std::string pub = s.isPublic ? "pub " : "";
            std::string typeParams;
            if (!s.typeParams.empty()) {
                typeParams = "<";
                for (std::size_t i = 0; i < s.typeParams.size(); ++i) {
                    if (i) {
                        typeParams += ", ";
                    }
                    typeParams += s.typeParams[i];
                }
                typeParams += ">";
            }
            out << std::format("\n{}struct {}{}\n", pub, s.name, typeParams);
            for (const auto &f : s.fields) {
                std::string fpub = f.isPublic ? "pub " : "";
                out << std::format("  {}{}: {}\n", fpub, f.name, f.type.ToString());
            }
        }
        for (const auto &e : mod.enums) {
            std::string pub = e.isPublic ? "pub " : "";
            std::string typeParams;
            if (!e.typeParams.empty()) {
                typeParams = "<";
                for (std::size_t i = 0; i < e.typeParams.size(); ++i) {
                    if (i) {
                        typeParams += ", ";
                    }
                    typeParams += e.typeParams[i];
                }
                typeParams += ">";
            }
            out << std::format("\n{}enum {}{}: {}\n", pub, e.name, typeParams, e.baseType.ToString());
            for (const auto &v : e.variants) {
                if (v.fields.empty()) {
                    out << std::format("  {} = {}\n", v.name, v.discriminant.value_or("0"));
                }
                else {
                    std::string fields;
                    for (std::size_t i = 0; i < v.fields.size(); ++i) {
                        if (i) {
                            fields += ", ";
                        }
                        fields += v.fields[i].ToString();
                    }
                    if (v.discriminant) {
                        out << std::format("  {}({}) = {}\n", v.name, fields, *v.discriminant);
                    }
                    else {
                        out << std::format("  {}({})\n", v.name, fields);
                    }
                }
            }
        }
        for (const auto &u : mod.unions) {
            std::string pub = u.isPublic ? "pub " : "";
            out << std::format("\n{}union {}\n", pub, u.name);
            for (const auto &f : u.fields) {
                out << std::format("  {}: {}\n", f.name, f.type.ToString());
            }
        }
        for (const auto &iface : mod.interfaces) {
            std::string pub = iface.isPublic ? "pub " : "";
            out << std::format("\n{}interface {}\n", pub, iface.name);
            for (const auto &m : iface.methods) {
                std::string params;
                for (std::size_t i = 0; i < m.params.size(); ++i) {
                    if (i) {
                        params += ", ";
                    }
                    if (m.params[i].isVariadic) {
                        params += "...";
                    }
                    else {
                        params += m.params[i].name + ": " + m.params[i].type.ToString();
                    }
                }
                std::string ret = m.returnType.IsOpaque() ? "" : " -> " + m.returnType.ToString();
                out << std::format("  func {}({}){}\n", m.name, params, ret);
            }
        }
        for (const auto &impl : mod.impls) {
            out << '\n';
            if (impl.interfaceName) {
                out << std::format("extend {} for {}\n", impl.typeName, *impl.interfaceName);
            }
            else {
                out << std::format("extend {}\n", impl.typeName);
            }
            for (const auto &m : impl.methods) {
                DumpFuncSignature(out, m, "  ");
                if (m.body) {
                    DumpBlock(out, *m.body, "    ");
                }
            }
        }
        for (const auto &f : mod.funcs) {
            out << '\n';
            DumpFuncSignature(out, f);
            if (f.body) {
                DumpBlock(out, *f.body, "  ");
            }
        }
    }
    return out.good();
}
} // namespace Rux

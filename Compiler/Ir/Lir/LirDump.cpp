// Human-readable LIR dump.

#include "Ir/Lir/LirPrinter.h"

#include <format>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>

namespace Rux {
static std::string_view OpcodeStr(LirOpcode op) {
    switch (op) {
    case LirOpcode::Const:
        return "const";
    case LirOpcode::Alloca:
        return "alloca";
    case LirOpcode::Load:
        return "load";
    case LirOpcode::Store:
        return "store";
    case LirOpcode::Add:
        return "add";
    case LirOpcode::Sub:
        return "sub";
    case LirOpcode::Mul:
        return "mul";
    case LirOpcode::Div:
        return "div";
    case LirOpcode::Mod:
        return "mod";
    case LirOpcode::Pow:
        return "pow";
    case LirOpcode::And:
        return "and";
    case LirOpcode::Or:
        return "or";
    case LirOpcode::Xor:
        return "xor";
    case LirOpcode::Shl:
        return "shl";
    case LirOpcode::Shr:
        return "shr";
    case LirOpcode::Neg:
        return "neg";
    case LirOpcode::Not:
        return "not";
    case LirOpcode::BitNot:
        return "bitnot";
    case LirOpcode::CmpEq:
        return "cmpeq";
    case LirOpcode::CmpNe:
        return "cmpne";
    case LirOpcode::CmpLt:
        return "cmplt";
    case LirOpcode::CmpLe:
        return "cmple";
    case LirOpcode::CmpGt:
        return "cmpgt";
    case LirOpcode::CmpGe:
        return "cmpge";
    case LirOpcode::Cast:
        return "cast";
    case LirOpcode::Call:
        return "call";
    case LirOpcode::CallIndirect:
        return "call_ind";
    case LirOpcode::Assert:
        return "assert";
    case LirOpcode::Panic:
        return "panic";
    case LirOpcode::FieldPtr:
        return "fieldptr";
    case LirOpcode::IndexPtr:
        return "indexptr";
    case LirOpcode::Phi:
        return "phi";
    case LirOpcode::GlobalAddr:
        return "globaladdr";
    case LirOpcode::StringAddr:
        return "stringaddr";
    default:
        return "?";
    }
}

// Dump
static std::string RegStr(LirReg r) {
    return r == LirNoReg ? "<void>" : std::format("%{}", r);
}

static std::string BlockLabel(const LirFunc &fn, std::uint32_t idx) {
    if (idx < fn.blocks.size()) {
        return fn.blocks[idx].label;
    }
    return std::format("bb{}", idx);
}

static void DumpInstr(std::ostream &out, const LirInstr &i, const LirFunc &fn) {
    out << "    ";
    switch (i.op) {
    case LirOpcode::Const:
        out << std::format("{} = const {} {}\n", RegStr(i.dst), i.type.ToString(), i.strArg);
        return;
    case LirOpcode::StringAddr:
        out << std::format("{} = stringaddr {} <{} bytes>\n", RegStr(i.dst), i.type.ToString(), i.strArg.size());
        return;
    case LirOpcode::Alloca:
        out << std::format("{} = alloca {}\n", RegStr(i.dst), i.type.ToString());
        return;
    case LirOpcode::Load:
        if (!i.srcs.empty()) {
            out << std::format("{} = load {} {}\n", RegStr(i.dst), i.type.ToString(), RegStr(i.srcs[0]));
        }
        else {
            out << std::format("{} = load {} {}\n", RegStr(i.dst), i.type.ToString(), i.strArg);
        }
        return;
    case LirOpcode::Store:
        out << std::format("store {} {}, {}\n", i.type.ToString(), !i.srcs.empty() ? RegStr(i.srcs[0]) : "?",
                           i.srcs.size() > 1 ? RegStr(i.srcs[1]) : "?");
        return;
    case LirOpcode::Cast:
        out << std::format("{} = cast {}: {} to {}\n", RegStr(i.dst), i.srcs.empty() ? "?" : RegStr(i.srcs[0]),
                           i.strArg, i.type.ToString());
        return;

    case LirOpcode::Call:
    case LirOpcode::CallIndirect: {
        std::string args;
        const std::size_t first = (i.op == LirOpcode::CallIndirect) ? 1 : 0;
        for (std::size_t k = first; k < i.srcs.size(); ++k) {
            if (k > first) {
                args += ", ";
            }
            args += RegStr(i.srcs[k]);
        }
        if (i.dst == LirNoReg) {
            if (i.op == LirOpcode::Call) {
                out << std::format("call {} @{}({})\n", i.type.ToString(), i.strArg, args);
            }
            else {
                out << std::format("call_ind {} {}({})\n", i.type.ToString(), RegStr(i.srcs[0]), args);
            }
        }
        else {
            if (i.op == LirOpcode::Call) {
                out << std::format("{} = call {} @{}({})\n", RegStr(i.dst), i.type.ToString(), i.strArg, args);
            }
            else {
                out << std::format("{} = call_ind {} {}({})\n", RegStr(i.dst), i.type.ToString(), RegStr(i.srcs[0]),
                                   args);
            }
        }
        return;
    }

    case LirOpcode::Assert:
        out << std::format("assert {}, {}, \"{}\" at {} ({}:{}:{})\n", i.srcs.empty() ? "?" : RegStr(i.srcs[0]),
                           i.srcs.size() > 1 ? RegStr(i.srcs[1]) : "?", i.strArg, i.sourceFunction, i.sourceFile,
                           i.sourceLine, i.sourceColumn);
        return;

    case LirOpcode::Panic:
        out << std::format("panic {}, \"{}\" at {} ({}:{}:{})\n", i.srcs.empty() ? "?" : RegStr(i.srcs[0]), i.strArg,
                           i.sourceFunction, i.sourceFile, i.sourceLine, i.sourceColumn);
        return;

    case LirOpcode::FieldPtr:
        out << std::format("{} = fieldptr {} {}, {}\n", RegStr(i.dst), i.type.ToString(),
                           i.srcs.empty() ? "?" : RegStr(i.srcs[0]), i.strArg);
        return;

    case LirOpcode::IndexPtr:
        out << std::format("{} = indexptr {} {}, {}\n", RegStr(i.dst), i.type.ToString(),
                           !i.srcs.empty() ? RegStr(i.srcs[0]) : "?", i.srcs.size() > 1 ? RegStr(i.srcs[1]) : "?");
        return;

    case LirOpcode::Phi: {
        std::string preds;
        for (std::size_t k = 0; k < i.phiPreds.size(); ++k) {
            if (k) {
                preds += ", ";
            }
            preds += std::format("[{}, {}]", RegStr(i.phiPreds[k].first), BlockLabel(fn, i.phiPreds[k].second));
        }
        out << std::format("{} = phi {} {}\n", RegStr(i.dst), i.type.ToString(), preds);
        return;
    }
    default: {
        // Unary (one src), binary (two srcs), or zero-operand/global addr (zero srcs)
        std::string_view opName = OpcodeStr(i.op);
        if (i.srcs.size() == 1) {
            out << std::format("{} = {} {} {}\n", RegStr(i.dst), opName, i.type.ToString(), RegStr(i.srcs[0]));
        }
        else if (i.srcs.size() >= 2) {
            out << std::format("{} = {} {} {}, {}\n", RegStr(i.dst), opName, i.type.ToString(), RegStr(i.srcs[0]),
                               RegStr(i.srcs[1]));
        }
        else {
            out << std::format("{} = {} {} {}\n", RegStr(i.dst), opName, i.type.ToString(), i.strArg);
        }
        return;
    }
    }
}

static void DumpTerminator(std::ostream &out, const LirTerminator &t, const LirFunc &fn) {
    out << "    ";
    switch (t.kind) {
    case LirTermKind::Jump:
        out << std::format("jmp {}\n", BlockLabel(fn, t.trueTarget));
        return;
    case LirTermKind::Branch:
        out << std::format("br {}, {}, {}\n", RegStr(t.cond), BlockLabel(fn, t.trueTarget),
                           BlockLabel(fn, t.falseTarget));
        return;
    case LirTermKind::Return:
        if (t.retVal) {
            out << std::format("ret {} {}\n", t.retType.ToString(), RegStr(*t.retVal));
        }
        else {
            out << "ret void\n";
        }
        return;
    case LirTermKind::Switch: {
        out << std::format("switch {} {}, default: {}", t.retType.ToString(), RegStr(t.cond),
                           BlockLabel(fn, t.defaultTarget));
        for (const auto &[value, target] : t.cases) {
            out << std::format(", {}: {}", value, BlockLabel(fn, target));
        }
        out << '\n';
        return;
    }
    case LirTermKind::Unreachable:
        out << "unreachable\n";
        return;
    }
}

static void DumpFunc(std::ostream &out, const LirFunc &fn) {
    std::string pub = fn.isPublic ? "pub " : "";
    std::string ext = fn.isExtern ? "extern " : "";
    std::string params;
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
        if (i) {
            params += ", ";
        }
        params += std::format("{}: {}", RegStr(fn.params[i].reg), fn.params[i].type.ToString());
    }
    std::string ret = fn.returnType.IsOpaque() ? "" : " -> " + fn.returnType.ToString();
    if (fn.isNoReturn) {
        out << "\n#NoReturn()";
    }
    out << std::format("\n{}{}func {}({}){}\n", pub, ext, fn.name, params, ret);
    for (const auto &block : fn.blocks) {
        out << std::format("  {}:\n", block.label);
        for (const auto &instr : block.instrs) {
            DumpInstr(out, instr, fn);
        }
        if (block.term) {
            DumpTerminator(out, *block.term, fn);
        }
    }
}

bool LirPrinter::Dump(const LirPackage &package, const std::filesystem::path &path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << "=== Low-level Intermediate Representation ===\n";
    for (const auto &mod : package.modules) {
        out << '\n';
        out << std::format("Module \"{}\"\n", mod.name);
        out << std::string(std::min<std::size_t>(mod.name.size() + 9, 72), '-') << '\n';
        for (const auto &ta : mod.typeAliases) {
            std::string pub = ta.isPublic ? "pub " : "";
            out << std::format("\n{}type {} = {}\n", pub, ta.name, ta.type.ToString());
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
                out << std::format("  {}: {}\n", f.name, f.type.ToString());
            }
        }
        for (const auto &e : mod.enums) {
            std::string pub = e.isPublic ? "pub " : "";
            out << std::format("\n{}enum {}: {}\n", pub, e.name, e.baseType.ToString());
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
        for (const auto &c : mod.consts) {
            std::string pub = c.isPublic ? "pub " : "";
            out << std::format("\n{}const {}: {} = {}\n", pub, c.name, c.type.ToString(), c.value);
        }
        for (const auto &ev : mod.externVars) {
            std::string pub = ev.isPublic ? "pub " : "";
            out << std::format("\nextern {}{}: {}\n", pub, ev.name, ev.type.ToString());
        }
        for (const auto &fn : mod.funcs) {
            DumpFunc(out, fn);
        }
    }

    return out.good();
}
} // namespace Rux

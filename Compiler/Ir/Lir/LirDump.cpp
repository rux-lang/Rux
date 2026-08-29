// Human-readable LIR dump.

#include "Ir/Lir/LirPrinter.h"

#include <format>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>

namespace Rux {
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

static std::string CallMetadata(const LirInstr &instr) {
    std::string metadata;
    if (instr.callConv != CallingConvention::Default) {
        metadata = std::format(" cc={}", ResolvedConventionName(instr.callConv));
    }
    if (instr.isCVariadic) {
        metadata += " c_variadic";
        if (instr.cVariadicFixedParamCount) {
            metadata += std::format(" fixed={}", *instr.cVariadicFixedParamCount);
        }
    }
    return metadata;
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
                out << std::format("call {} @{}({}){}\n", i.type.ToString(), i.strArg, args, CallMetadata(i));
            }
            else {
                out << std::format("call_ind {} {}({})\n", i.type.ToString(), RegStr(i.srcs[0]), args);
            }
        }
        else {
            if (i.op == LirOpcode::Call) {
                out << std::format("{} = call {} @{}({}){}\n", RegStr(i.dst), i.type.ToString(), i.strArg, args,
                                   CallMetadata(i));
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
        std::string_view opName = LirOpcodeName(i.op);
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
    if (fn.isVariadic) {
        params += params.empty() ? "..." : ", ...";
    }
    std::string ret = fn.returnType.IsOpaque() ? "" : " -> " + fn.returnType.ToString();
    std::string convention;
    if (fn.callConv != CallingConvention::Default) {
        convention = std::format(" cc={}", ResolvedConventionName(fn.callConv));
    }
    if (fn.isNoReturn) {
        out << "\n#NoReturn()";
    }
    out << std::format("\n{}{}func {}({}){}{}\n", pub, ext, fn.name, params, ret, convention);
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
            out << std::format("\n{}{} {}{}", pub, CaseTypeKeyword(e.form), e.name, typeParams);
            if (!e.IsVariant()) {
                out << std::format(": {}", e.baseType.ToString());
            }
            out << '\n';
            for (const auto &v : e.variants) {
                if (!e.IsVariant()) {
                    out << std::format("  {} = {}\n", v.name, v.discriminant.value_or("0"));
                }
                else if (v.fields.empty()) {
                    out << std::format("  {}\n", v.name);
                }
                else {
                    std::string fields;
                    for (std::size_t i = 0; i < v.fields.size(); ++i) {
                        if (i) {
                            fields += ", ";
                        }
                        fields += v.fields[i].ToString();
                    }
                    out << std::format("  {}({})\n", v.name, fields);
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

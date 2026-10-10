#include "Optimization/LirInliner.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
bool Scalar(const TypeRef &type) {
    return type.IsInteger() || type.IsFloat() || type.IsBool() || type.IsChar() ||
           type.kind == TypeRef::Kind::Pointer ||
           (type.kind == TypeRef::Kind::Reference && type.inner.size() == 1 &&
            (type.inner[0].IsInteger() || type.inner[0].IsFloat() || type.inner[0].IsBool() || type.inner[0].IsChar()));
}

bool ArgumentMatches(const TypeRef &argument, const TypeRef &parameter) {
    if (argument == parameter) {
        return true;
    }
    // Lowering represents a borrow of a local scalar by its alloca's pointer. Both have the same scalar-address ABI;
    // accepting this representation does not grant no-alias or lifetime assumptions to the cloned loads/stores.
    return parameter.kind == TypeRef::Kind::Reference && Scalar(parameter) && argument.kind == TypeRef::Kind::Pointer &&
           argument.inner == parameter.inner;
}

bool Eligible(const LirFunc &function) {
    if (function.isExtern || function.isAsm || function.isVariadic || function.blocks.size() != 1 ||
        !Scalar(function.returnType) ||
        !std::ranges::all_of(function.params, [](const auto &p) { return Scalar(p.type); })) {
        return false;
    }
    const auto &block = function.blocks[0];
    if (!block.term || block.term->kind != LirTermKind::Return || !block.term->retVal || block.instrs.size() > 24) {
        return false;
    }
    std::unordered_set<LirReg> definitions;
    for (const auto &parameter : function.params) {
        if (!definitions.insert(parameter.reg).second) {
            return false;
        }
    }
    for (const auto &instruction : block.instrs) {
        for (const auto source : instruction.srcs) {
            if (!definitions.contains(source)) {
                return false;
            }
        }
        if (instruction.dst != LirNoReg && !definitions.insert(instruction.dst).second) {
            return false;
        }
        if (instruction.op == LirOpcode::Load && !instruction.strArg.empty()) {
            return false; // Do not transplant module-local data lookup into another module.
        }
        switch (instruction.op) {
        case LirOpcode::Const:
        case LirOpcode::Load:
        case LirOpcode::Store:
        case LirOpcode::Add:
        case LirOpcode::Sub:
        case LirOpcode::Mul:
        case LirOpcode::And:
        case LirOpcode::Or:
        case LirOpcode::Xor:
        case LirOpcode::Shl:
        case LirOpcode::Shr:
        case LirOpcode::Lshr:
        case LirOpcode::RotateRight:
        case LirOpcode::Neg:
        case LirOpcode::Not:
        case LirOpcode::BitNot:
        case LirOpcode::CmpEq:
        case LirOpcode::CmpNe:
        case LirOpcode::CmpLt:
        case LirOpcode::CmpLe:
        case LirOpcode::CmpGt:
        case LirOpcode::CmpGe:
        case LirOpcode::Cast:
        case LirOpcode::FieldPtr:
        case LirOpcode::IndexPtr:
            break;
        default:
            return false; // No calls, recursion, stack identity, atomics, runtime reports or lifetime changes.
        }
    }
    return definitions.contains(*block.term->retVal);
}
} // namespace

std::string_view LirInliner::Name() const noexcept {
    return "lir-leaf-inliner";
}

PassChange LirInliner::Run(LirPackage &package, const PassContext &context) {
    if (context.iteration != 0) {
        return PassChange::None;
    }
    std::unordered_map<std::string, LirFunc> leaves;
    std::unordered_set<std::string> seen, ambiguous;
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            if (!seen.insert(function.name).second) {
                ambiguous.insert(function.name);
            }
            if (Eligible(function)) {
                leaves.emplace(function.name, function);
            }
        }
    }
    for (const auto &name : ambiguous) {
        leaves.erase(name);
    }
    std::size_t packageBudget = 50000;
    bool changed = false;
    for (auto &module : package.modules) {
        for (auto &function : module.funcs) {
            LirReg next = 0;
            std::unordered_map<LirReg, TypeRef> types;
            for (const auto &parameter : function.params) {
                types[parameter.reg] = parameter.type;
                next = std::max(next, parameter.reg + 1);
            }
            for (const auto &block : function.blocks) {
                for (const auto &instruction : block.instrs) {
                    if (instruction.dst != LirNoReg) {
                        types[instruction.dst] = instruction.op == LirOpcode::Alloca
                                                   ? TypeRef::MakePointer(instruction.type)
                                                   : instruction.type;
                        next = std::max(next, instruction.dst + 1);
                    }
                }
            }
            std::size_t budget = 2048;
            for (auto &block : function.blocks) {
                std::vector<LirInstr> instructions;
                for (const auto &call : block.instrs) {
                    const auto found = leaves.find(call.strArg);
                    if (call.op != LirOpcode::Call || found == leaves.end() || call.isCVariadic) {
                        instructions.push_back(call);
                        continue;
                    }
                    const auto &callee = found->second;
                    const auto &body = callee.blocks[0];
                    const auto cost = body.instrs.size() + 1;
                    bool valid = cost <= budget && cost <= packageBudget && call.type == callee.returnType &&
                                 call.srcs.size() == callee.params.size();
                    if (valid) {
                        for (std::size_t i = 0; i < call.srcs.size(); ++i) {
                            valid = valid && types.contains(call.srcs[i]) &&
                                    ArgumentMatches(types.at(call.srcs[i]), callee.params[i].type);
                        }
                    }
                    if (!valid) {
                        instructions.push_back(call);
                        continue;
                    }
                    std::unordered_map<LirReg, LirReg> renamed;
                    for (std::size_t i = 0; i < call.srcs.size(); ++i) {
                        renamed[callee.params[i].reg] = call.srcs[i];
                    }
                    for (const auto &instruction : body.instrs) {
                        if (instruction.dst != LirNoReg) {
                            renamed[instruction.dst] = next++;
                        }
                    }
                    for (auto instruction : body.instrs) {
                        if (instruction.dst != LirNoReg) {
                            instruction.dst = renamed.at(instruction.dst);
                            types[instruction.dst] = instruction.type;
                        }
                        for (auto &source : instruction.srcs) {
                            source = renamed.at(source);
                        }
                        instructions.push_back(std::move(instruction));
                    }
                    if (call.dst != LirNoReg) {
                        // Preserve the call's SSA definition, including the type normalization its return imposed.
                        LirInstr result;
                        result.dst = call.dst;
                        result.type = call.type;
                        result.op = LirOpcode::Cast;
                        result.srcs = {renamed.at(*body.term->retVal)};
                        result.strArg = call.type.ToString();
                        instructions.push_back(std::move(result));
                    }
                    budget -= cost;
                    packageBudget -= cost;
                    changed = true;
                }
                block.instrs = std::move(instructions);
            }
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization

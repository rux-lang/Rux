#include "Optimization/LirInliner.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
using Types = std::unordered_map<LirReg, TypeRef>;

bool Scalar(const TypeRef &type) {
    return type.IsInteger() || type.IsFloat() || type.IsBool() || type.IsChar() ||
           type.kind == TypeRef::Kind::Pointer ||
           (type.kind == TypeRef::Kind::Reference && type.inner.size() == 1 &&
            (type.inner[0].IsInteger() || type.inner[0].IsFloat() || type.inner[0].IsBool() || type.inner[0].IsChar()));
}

bool ArgumentMatches(const TypeRef &argument, const TypeRef &parameter) {
    return argument == parameter || (parameter.kind == TypeRef::Kind::Reference && Scalar(parameter) &&
                                     argument.kind == TypeRef::Kind::Pointer && argument.inner == parameter.inner);
}

std::size_t Cost(const LirFunc &function) {
    std::size_t cost = function.blocks.size();
    for (const auto &block : function.blocks) {
        cost += block.instrs.size();
    }
    return cost;
}

bool Eligible(const LirFunc &function) {
    if (function.isExtern || function.isAsm || function.isVariadic || function.blocks.empty() ||
        function.blocks.size() > 8 || Cost(function) > 48 || !Scalar(function.returnType) ||
        !std::ranges::all_of(function.params, [](const auto &p) { return Scalar(p.type); })) {
        return false;
    }
    std::unordered_set<LirReg> definitions;
    for (const auto &parameter : function.params) {
        if (!definitions.insert(parameter.reg).second) {
            return false;
        }
    }
    bool returns = false;
    for (const auto &block : function.blocks) {
        if (!block.term) {
            return false;
        }
        if (block.term->kind == LirTermKind::Return) {
            if (!block.term->retVal || block.term->retType != function.returnType) {
                return false;
            }
            returns = true;
        }
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg && !definitions.insert(instruction.dst).second) {
                return false;
            }
            if (instruction.op == LirOpcode::Load && !instruction.strArg.empty()) {
                return false; // Module-local data lookup cannot be transplanted into another module.
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
            case LirOpcode::Phi:
            case LirOpcode::Call:
                break;
            default:
                return false; // No stack identity, indirect recursion, atomics or relocated runtime reports.
            }
        }
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            for (const auto source : instruction.srcs) {
                if (!definitions.contains(source)) {
                    return false;
                }
            }
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                if (!definitions.contains(source) || predecessor >= function.blocks.size()) {
                    return false;
                }
            }
        }
        if ((block.term->cond != LirNoReg && !definitions.contains(block.term->cond)) ||
            (block.term->retVal && !definitions.contains(*block.term->retVal))) {
            return false;
        }
    }
    return returns;
}

template <typename Map>
void RemapTargets(LirTerminator &term, Map map) {
    switch (term.kind) {
    case LirTermKind::Jump:
        term.trueTarget = map(term.trueTarget);
        break;
    case LirTermKind::Branch:
        term.trueTarget = map(term.trueTarget);
        term.falseTarget = map(term.falseTarget);
        break;
    case LirTermKind::Switch:
        term.defaultTarget = map(term.defaultTarget);
        for (auto &entry : term.cases) {
            entry.target = map(entry.target);
        }
        break;
    case LirTermKind::Return:
    case LirTermKind::Unreachable:
        break;
    }
}

LirTerminator Jump(const std::uint32_t target) {
    LirTerminator result;
    result.kind = LirTermKind::Jump;
    result.trueTarget = target;
    return result;
}

void Inline(LirFunc &caller, std::uint32_t &blockIndex, std::size_t &position, const LirFunc &callee, LirReg &next,
            Types &types) {
    const auto call = caller.blocks[blockIndex].instrs[position];
    std::unordered_map<LirReg, LirReg> renamed;
    for (std::size_t i = 0; i < call.srcs.size(); ++i) {
        renamed[callee.params[i].reg] = call.srcs[i];
    }
    for (const auto &block : callee.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg) {
                renamed[instruction.dst] = next;
                types[next++] = instruction.type;
            }
        }
    }
    auto clones = callee.blocks;
    for (auto &block : clones) {
        block.label = "inline_" + std::to_string(next) + "_" + block.label;
        for (auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg) {
                instruction.dst = renamed.at(instruction.dst);
            }
            for (auto &source : instruction.srcs) {
                source = renamed.at(source);
            }
            for (auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                source = renamed.at(source);
            }
        }
        if (block.term->cond != LirNoReg) {
            block.term->cond = renamed.at(block.term->cond);
        }
        if (block.term->retVal) {
            block.term->retVal = renamed.at(*block.term->retVal);
        }
    }
    if (clones.size() == 1 && clones[0].term->kind == LirTermKind::Return) {
        auto &instructions = clones[0].instrs;
        const auto returned = *clones[0].term->retVal;
        if (call.dst != LirNoReg && types.at(returned) == call.type) {
            // A return is an SSA rename, not a numeric conversion. In particular, wide software values must not
            // acquire an artificial cast after the software-operation lowering stage has already run.
            const auto replace = [&](LirReg &reg) {
                if (reg == call.dst) {
                    reg = returned;
                }
            };
            for (auto &block : caller.blocks) {
                for (auto &instruction : block.instrs) {
                    for (auto &source : instruction.srcs) {
                        replace(source);
                    }
                    for (auto &[source, predecessor] : instruction.phiPreds) {
                        static_cast<void>(predecessor);
                        replace(source);
                    }
                }
                replace(block.term->cond);
                if (block.term->retVal) {
                    replace(*block.term->retVal);
                }
            }
        }
        else if (call.dst != LirNoReg) {
            LirInstr result;
            result.dst = call.dst;
            result.type = call.type;
            result.op = LirOpcode::Cast;
            result.srcs = {*clones[0].term->retVal};
            result.strArg = types.at(result.srcs[0]).ToString();
            instructions.push_back(std::move(result));
        }
        auto &body = caller.blocks[blockIndex].instrs;
        const auto insertion = body.erase(body.begin() + static_cast<std::ptrdiff_t>(position));
        body.insert(insertion, std::make_move_iterator(instructions.begin()),
                    std::make_move_iterator(instructions.end()));
        position += instructions.size();
        return;
    }
    const auto base = blockIndex + 1;
    const auto continuationIndex = base + static_cast<std::uint32_t>(clones.size());
    const auto shift = static_cast<std::uint32_t>(clones.size() + 1);
    const auto remap = [&](const std::uint32_t old) { return old > blockIndex ? old + shift : old; };
    for (auto &block : caller.blocks) {
        RemapTargets(*block.term, remap);
        for (auto &instruction : block.instrs) {
            for (auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(source);
                predecessor = predecessor == blockIndex ? continuationIndex : remap(predecessor);
            }
        }
    }
    LirBlock continuation;
    continuation.label = "inline_return_" + std::to_string(next);
    continuation.term = std::move(caller.blocks[blockIndex].term);
    LirInstr result;
    result.op = LirOpcode::Phi;
    result.dst = call.dst;
    result.type = call.type;
    for (std::uint32_t i = 0; i < clones.size(); ++i) {
        auto &block = clones[i];
        for (auto &instruction : block.instrs) {
            for (auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(source);
                predecessor += base;
            }
        }
        if (block.term->kind == LirTermKind::Return) {
            auto value = *block.term->retVal;
            if (call.dst != LirNoReg && types.at(value) != call.type) {
                LirInstr conversion;
                conversion.op = LirOpcode::Cast;
                conversion.dst = next++;
                conversion.type = call.type;
                conversion.srcs = {value};
                conversion.strArg = types.at(value).ToString();
                value = conversion.dst;
                types[value] = call.type;
                block.instrs.push_back(std::move(conversion));
            }
            result.phiPreds.emplace_back(value, base + i);
            block.term = Jump(continuationIndex);
        }
        else {
            RemapTargets(*block.term, [base](const auto old) { return old + base; });
        }
    }
    if (call.dst != LirNoReg) {
        continuation.instrs.push_back(std::move(result));
    }
    auto &body = caller.blocks[blockIndex].instrs;
    continuation.instrs.insert(continuation.instrs.end(),
                               std::make_move_iterator(body.begin() + static_cast<std::ptrdiff_t>(position + 1)),
                               std::make_move_iterator(body.end()));
    body.resize(position);
    caller.blocks[blockIndex].term = Jump(base);
    clones.push_back(std::move(continuation));
    caller.blocks.insert(caller.blocks.begin() + base, std::make_move_iterator(clones.begin()),
                         std::make_move_iterator(clones.end()));
    blockIndex = continuationIndex;
    position = call.dst != LirNoReg ? 1 : 0;
}
} // namespace

std::string_view LirInliner::Name() const noexcept {
    return "lir-leaf-inliner"; // Retain the stable identifier used by statistics and developer tooling.
}

PassChange LirInliner::Run(LirPackage &package, const PassContext &context) {
    if (context.iteration != 0) {
        return PassChange::None;
    }
    std::vector<LirFunc *> functions;
    std::unordered_map<std::string, std::size_t> names;
    std::unordered_set<std::string> ambiguous;
    for (auto &module : package.modules) {
        for (auto &function : module.funcs) {
            if (!names.emplace(function.name, functions.size()).second) {
                ambiguous.insert(function.name);
            }
            functions.push_back(&function);
        }
    }
    for (const auto &name : ambiguous) {
        names.erase(name);
    }
    // Deterministic sink removal orders callees before callers. Recursive components and wrappers reaching them never
    // become candidates. Their bodies may still inline independent leaf calls, under the same finite growth budget.
    std::vector<std::vector<std::size_t>> callers(functions.size());
    std::vector<std::size_t> remaining(functions.size());
    for (std::size_t index = 0; index < functions.size(); ++index) {
        std::unordered_set<std::size_t> targets;
        for (const auto &block : functions[index]->blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.op == LirOpcode::Call && names.contains(instruction.strArg)) {
                    targets.insert(names.at(instruction.strArg));
                }
            }
        }
        remaining[index] = targets.size();
        for (const auto target : targets) {
            callers[target].push_back(index);
        }
    }
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
    for (std::size_t i = 0; i < remaining.size(); ++i) {
        if (remaining[i] == 0) {
            ready.push(i);
        }
    }
    std::vector<std::size_t> order;
    while (!ready.empty()) {
        const auto next = ready.top();
        ready.pop();
        order.push_back(next);
        for (const auto caller : callers[next]) {
            if (--remaining[caller] == 0) {
                ready.push(caller);
            }
        }
    }
    for (std::size_t i = 0; i < remaining.size(); ++i) {
        if (remaining[i] != 0) {
            order.push_back(i);
        }
    }
    std::unordered_map<std::string, LirFunc> available;
    std::size_t packageBudget = 50000;
    bool changed = false;
    for (const auto index : order) {
        auto &function = *functions[index];
        LirReg next = 0;
        Types types;
        for (const auto &parameter : function.params) {
            types[parameter.reg] = parameter.type;
            next = std::max(next, parameter.reg + 1);
        }
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.dst != LirNoReg) {
                    types[instruction.dst] =
                        instruction.op == LirOpcode::Alloca ? TypeRef::MakePointer(instruction.type) : instruction.type;
                    next = std::max(next, instruction.dst + 1);
                }
            }
        }
        std::size_t budget = 2048;
        for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
            for (std::size_t position = 0; position < function.blocks[block].instrs.size();) {
                const auto &call = function.blocks[block].instrs[position];
                const auto found = available.find(call.strArg);
                bool valid = call.op == LirOpcode::Call && found != available.end() && !call.isCVariadic;
                if (valid) {
                    const auto &callee = found->second;
                    valid = call.type == callee.returnType && call.srcs.size() == callee.params.size() &&
                            Cost(callee) <= budget && Cost(callee) <= packageBudget &&
                            function.blocks.size() + callee.blocks.size() + 1 <= 4096;
                    for (std::size_t i = 0; valid && i < call.srcs.size(); ++i) {
                        valid = types.contains(call.srcs[i]) &&
                                ArgumentMatches(types.at(call.srcs[i]), callee.params[i].type);
                    }
                }
                if (!valid) {
                    ++position;
                    continue;
                }
                const auto cost = Cost(found->second);
                Inline(function, block, position, found->second, next, types);
                budget -= cost;
                packageBudget -= cost;
                changed = true;
            }
        }
        if (remaining[index] == 0 && !ambiguous.contains(function.name) && Eligible(function)) {
            available.emplace(function.name, function);
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization

#include "Optimization/LirInliner.h"

#include "Optimization/ReferenceTypes.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <optional>
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
    return argument == parameter || (parameter.kind == TypeRef::Kind::Reference &&
                                     argument.kind == TypeRef::Kind::Pointer && argument.inner == parameter.inner);
}

// Recognize semantics from an exact assembly body, never from a function's spelling. Both supported ABIs pass and
// return the first scalar FP value in register zero. Reject labels, extra work, non-native widths and variadic calls.
bool NativeSquareRoot(const LirFunc &function) {
    if (!function.isAsm || function.isExtern || function.isVariadic || function.isNoReturn ||
        function.params.size() != 1 || function.params[0].type != function.returnType || function.asmBody.size() != 2)
        return false;
    const bool single = function.returnType.kind == TypeRef::Kind::Float32;
    if (!single && function.returnType.kind != TypeRef::Kind::Float64)
        return false;
    const auto &operation = function.asmBody[0];
    const auto &ret = function.asmBody[1];
    if (!operation.labelDef.empty() || !ret.labelDef.empty() || ret.mnemonic != "ret" || !ret.operands.empty() ||
        operation.operands.size() != 2 || operation.arch != ret.arch)
        return false;
    std::string_view reg;
    if (operation.arch == Target::Arch::X86_64 && operation.mnemonic == (single ? "sqrtss" : "sqrtsd"))
        reg = "xmm0";
    else if (operation.arch == Target::Arch::AArch64 && operation.mnemonic == "fsqrt")
        reg = single ? "s0" : "d0";
    else
        return false;
    return std::ranges::all_of(operation.operands, [&](const AsmOperand &operand) {
        return operand.kind == AsmOperand::Kind::Reg && operand.name == reg && operand.shift == AsmShiftKind::None &&
               operand.extend == AsmExtendKind::None;
    });
}

std::size_t Cost(const LirFunc &function) {
    std::size_t cost = function.blocks.size();
    for (const auto &block : function.blocks) {
        cost += block.instrs.size();
    }
    return cost;
}

[[nodiscard]] bool Fails(const LirBlock &block) {
    return std::ranges::any_of(block.instrs,
                               [](const LirInstr &instruction) { return instruction.op == LirOpcode::Panic; });
}

/// The size of what a call site executes: a failure report is code that only runs on the way to a trap, so a helper
/// whose checks each add one is still small where it matters.
std::size_t HotCost(const LirFunc &function) {
    std::size_t cost = 0;
    for (const auto &block : function.blocks) {
        if (!Fails(block))
            cost += 1 + block.instrs.size();
    }
    return cost;
}

[[nodiscard]] std::size_t HotBlocks(const LirFunc &function) {
    return static_cast<std::size_t>(
        std::ranges::count_if(function.blocks, [](const LirBlock &block) { return !Fails(block); }));
}

/// How large a callee may be. A call that passes the address of one of the caller's own objects earns the larger
/// limits: once inlined, the object's fields can live in registers instead of memory the callee reads through a
/// pointer.
enum class Size {
    Small,
    Large
};

bool Within(const LirFunc &function, const Size size) {
    const std::size_t scale = size == Size::Small ? 1 : 2;
    return HotBlocks(function) <= 8 * scale && HotCost(function) <= 64 * scale && Cost(function) <= 160 * scale;
}

std::optional<Size> Eligible(const LirFunc &function, const ConcreteReferenceTypes &references) {
    const auto parameterFits = [&](const TypeRef &type) { return Scalar(type) || references.Contains(type); };
    // A structure result is renamed into the caller only from a single block, where no phi has to merge it.
    if (function.isExtern || function.isAsm || function.isVariadic || function.blocks.empty() ||
        !Within(function, Size::Large) ||
        (!Scalar(function.returnType) && !function.returnType.IsOpaque() && function.blocks.size() != 1) ||
        !std::ranges::all_of(function.params, [&](const auto &p) { return parameterFits(p.type); })) {
        return std::nullopt;
    }
    // That block returns the structure itself or the address of an object holding it, which the caller loads.
    if (!Scalar(function.returnType) && !function.returnType.IsOpaque()) {
        const auto &exit = *function.blocks.front().term;
        const auto returned = std::ranges::find_if(function.blocks.front().instrs, [&](const LirInstr &instruction) {
            return exit.retVal && instruction.dst == *exit.retVal;
        });
        // An allocation's own type is the type of the object it holds.
        if (returned == function.blocks.front().instrs.end() || returned->type != function.returnType)
            return std::nullopt;
    }
    std::unordered_set<LirReg> definitions;
    for (const auto &parameter : function.params) {
        if (!definitions.insert(parameter.reg).second) {
            return std::nullopt;
        }
    }
    bool returns = false;
    for (const auto &block : function.blocks) {
        if (!block.term) {
            return std::nullopt;
        }
        if (block.term->kind == LirTermKind::Return) {
            if (block.term->retVal.has_value() == function.returnType.IsOpaque() ||
                block.term->retType != function.returnType) {
                return std::nullopt;
            }
            returns = true;
        }
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg && !definitions.insert(instruction.dst).second) {
                return std::nullopt;
            }
            if (instruction.op == LirOpcode::Load && !instruction.strArg.empty()) {
                return std::nullopt; // Module-local data lookup cannot be transplanted into another module.
            }
            switch (instruction.op) {
            case LirOpcode::Const:
            case LirOpcode::GlobalAddr:
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
            case LirOpcode::Sqrt:
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
            // A division traps at the same point inlined or not. A failure report keeps the source context it
            // carries, its message storage is a fresh allocation the call site owns, and literal text is interned by
            // whichever module emits it.
            case LirOpcode::Div:
            case LirOpcode::Mod:
            case LirOpcode::Panic:
            case LirOpcode::Assert:
            case LirOpcode::StringAddr:
            case LirOpcode::Alloca:
                break;
            default:
                return std::nullopt; // No frame identity, indirect recursion, atomics or thread-local access.
            }
        }
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            for (const auto source : instruction.srcs) {
                if (!definitions.contains(source)) {
                    return std::nullopt;
                }
            }
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                if (!definitions.contains(source) || predecessor >= function.blocks.size()) {
                    return std::nullopt;
                }
            }
        }
        if ((block.term->cond != LirNoReg && !definitions.contains(block.term->cond)) ||
            (block.term->retVal && !definitions.contains(*block.term->retVal))) {
            return std::nullopt;
        }
    }
    if (!returns)
        return std::nullopt;
    return Within(function, Size::Small) ? Size::Small : Size::Large;
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
                types[next++] =
                    instruction.op == LirOpcode::Alloca ? TypeRef::MakePointer(instruction.type) : instruction.type;
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
        const auto returned = clones[0].term->retVal.value_or(LirNoReg);
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
            result.srcs = {*clones[0].term->retVal};
            // A structure result may be returned as the address of the object holding it; the caller receives its
            // contents as they stand at the return.
            if (types.at(result.srcs[0]) == TypeRef::MakePointer(call.type)) {
                result.op = LirOpcode::Load;
            }
            else {
                result.op = LirOpcode::Cast;
                result.strArg = types.at(result.srcs[0]).ToString();
            }
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
            auto value = block.term->retVal.value_or(LirNoReg);
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
            if (call.dst != LirNoReg)
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
    // Later rounds see helpers that earlier rounds' inlining and the folding and block coalescing after them made
    // small enough: a helper calling a helper becomes inlinable one round after its callee was inlined into it.
    if (context.iteration > 2) {
        return PassChange::None;
    }
    std::vector<LirFunc *> functions;
    std::vector<std::size_t> ownerIndex;
    std::unordered_map<std::string, std::size_t> names;
    std::unordered_set<std::string> ambiguous;
    // A generic instantiation is emitted in every module that uses it, so one name can have a definition per module.
    // A call resolves to the definition in its own module first, which is the one it links against anyway.
    std::vector<std::unordered_map<std::string, std::size_t>> moduleNames(package.modules.size());
    const ConcreteReferenceTypes references(package);
    for (std::size_t moduleIndex = 0; moduleIndex < package.modules.size(); ++moduleIndex) {
        auto &module = package.modules[moduleIndex];
        for (auto &function : module.funcs) {
            if (!names.emplace(function.name, functions.size()).second) {
                ambiguous.insert(function.name);
            }
            if (!function.isExtern)
                moduleNames[moduleIndex].emplace(function.name, functions.size());
            functions.push_back(&function);
            ownerIndex.push_back(moduleIndex);
        }
    }
    for (const auto &name : ambiguous) {
        names.erase(name);
    }
    // Inlining stays inside a module: what a module compiles to depends only on its own source and the declarations
    // of the rest of the program, never on another module's bodies, so a module whose source did not change can be
    // reused from an earlier build as it is. Only a call to an assembly square root, whose body is part of its
    // module's interface, is recognized across modules.
    const auto resolve = [&](const std::size_t caller, const std::string &name) -> std::optional<std::size_t> {
        const auto &local = moduleNames[ownerIndex[caller]];
        if (const auto found = local.find(name); found != local.end())
            return found->second;
        return std::nullopt;
    };
    const auto resolveAnywhere = [&](const std::size_t caller, const std::string &name) -> std::optional<std::size_t> {
        if (const auto found = resolve(caller, name))
            return found;
        if (const auto found = names.find(name); found != names.end())
            return found->second;
        return std::nullopt;
    };
    // Deterministic sink removal orders callees before callers. Recursive components and wrappers reaching them never
    // become candidates. Their bodies may still inline independent leaf calls, under the same finite growth budget.
    std::vector<std::vector<std::size_t>> callers(functions.size());
    std::vector<std::size_t> remaining(functions.size());
    for (std::size_t index = 0; index < functions.size(); ++index) {
        std::unordered_set<std::size_t> targets;
        for (const auto &block : functions[index]->blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.op != LirOpcode::Call)
                    continue;
                if (const auto target = resolve(index, instruction.strArg)) {
                    targets.insert(*target);
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
    // Callees are kept by address: a function is finished, and never changed again this round, before it is offered.
    std::unordered_map<std::size_t, const LirFunc *> available;
    std::unordered_map<std::size_t, Size> sizes;
    // The growth budget is a module's own, for the same reason inlining stays inside one.
    std::vector<std::size_t> moduleBudgets(package.modules.size(), 50000);
    bool changed = false;
    for (const auto index : order) {
        auto &function = *functions[index];
        // Register types and the function's own addresses are needed only around a call this round may rewrite, so they
        // are gathered when the first such call turns up, still before anything was inlined into the function.
        LirReg next = 0;
        Types types;
        std::unordered_set<LirReg> local;
        bool prepared = false;
        const auto prepare = [&] {
            if (prepared)
                return;
            prepared = true;
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
            // Addresses of the function's own objects and their fields, as it stood before this round inlined into it.
            for (int pass = 0; pass < 2; ++pass)
                for (const auto &block : function.blocks)
                    for (const auto &instruction : block.instrs)
                        if (instruction.op == LirOpcode::Alloca ||
                            ((instruction.op == LirOpcode::FieldPtr || instruction.op == LirOpcode::IndexPtr) &&
                             !instruction.srcs.empty() && local.contains(instruction.srcs[0])))
                            local.insert(instruction.dst);
        };
        std::size_t budget = 2048;
        bool functionChanged = false;
        for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
            for (std::size_t position = 0; position < function.blocks[block].instrs.size();) {
                auto &call = function.blocks[block].instrs[position];
                const auto target =
                    call.op == LirOpcode::Call ? resolve(index, call.strArg) : std::optional<std::size_t>{};
                const auto rooted =
                    call.op == LirOpcode::Call ? resolveAnywhere(index, call.strArg) : std::optional<std::size_t>{};
                const bool squareRoot = rooted && call.dst != LirNoReg && !call.isCVariadic && !call.isVolatile &&
                                        NativeSquareRoot(*functions[*rooted]) && call.srcs.size() == 1;
                if (squareRoot)
                    prepare();
                if (squareRoot && types.contains(call.srcs[0]) && types.at(call.srcs[0]) == call.type &&
                    call.type == functions[*rooted]->returnType) {
                    call.op = LirOpcode::Sqrt;
                    call.strArg.clear();
                    call.callConv = CallingConvention::Default;
                    functionChanged = true;
                    ++position;
                    continue;
                }
                const auto found = target ? available.find(*target) : available.end();
                bool valid =
                    call.op == LirOpcode::Call && found != available.end() && !call.isCVariadic && !call.isVolatile;
                if (valid) {
                    prepare();
                    const auto &callee = *found->second;
                    valid = call.type == callee.returnType && call.srcs.size() == callee.params.size() &&
                            (!callee.returnType.IsOpaque() || call.dst == LirNoReg) && Cost(callee) <= budget &&
                            Cost(callee) <= moduleBudgets[ownerIndex[index]] &&
                            function.blocks.size() + callee.blocks.size() + 1 <= 4096;
                    for (std::size_t i = 0; valid && i < call.srcs.size(); ++i) {
                        valid = types.contains(call.srcs[i]) &&
                                ArgumentMatches(types.at(call.srcs[i]), callee.params[i].type);
                    }
                    if (valid && sizes.at(*target) == Size::Large)
                        valid =
                            std::ranges::any_of(call.srcs, [&](const LirReg source) { return local.contains(source); });
                }
                if (!valid) {
                    ++position;
                    continue;
                }
                const auto cost = Cost(*found->second);
                Inline(function, block, position, *found->second, next, types);
                budget -= cost;
                moduleBudgets[ownerIndex[index]] -= cost;
                functionChanged = true;
            }
        }
        if (functionChanged) {
            context.NoteChanged(function);
            changed = true;
        }
        if (const auto size = remaining[index] == 0 ? Eligible(function, references) : std::nullopt) {
            available.emplace(index, &function);
            sizes.emplace(index, *size);
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization

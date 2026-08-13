#include "Optimization/LirConstantPropagation.h"

#include "Optimization/ConstantEvaluator.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Rux::Optimization {
namespace {
struct KnownValue {
    std::optional<TypedConstant> constant;
    std::optional<LirReg> copy;
};

using Values = std::unordered_map<LirReg, KnownValue>;

bool SameConstant(const TypedConstant &left, const TypedConstant &right) {
    return left.GetKind() == right.GetKind() && left.Width() == right.Width() && left.RawBits() == right.RawBits();
}

bool SameValue(const KnownValue &left, const KnownValue &right) {
    if (left.constant && right.constant) {
        return SameConstant(*left.constant, *right.constant);
    }
    return left.copy && right.copy && left.copy == right.copy;
}

KnownValue Resolve(const Values &values, const LirReg reg) {
    LirReg current = reg;
    std::unordered_set<LirReg> visited;
    while (visited.insert(current).second) {
        const auto found = values.find(current);
        if (found == values.end()) {
            return KnownValue{std::nullopt, current};
        }
        if (found->second.constant) {
            return found->second;
        }
        if (!found->second.copy || *found->second.copy == current) {
            return KnownValue{std::nullopt, current};
        }
        current = *found->second.copy;
    }
    return KnownValue{std::nullopt, *std::ranges::min_element(visited)};
}

std::optional<TokenKind> UnaryToken(const LirOpcode opcode) {
    switch (opcode) {
    case LirOpcode::Neg:
        return TokenKind::Minus;
    case LirOpcode::Not:
        return TokenKind::Bang;
    case LirOpcode::BitNot:
        return TokenKind::Tilde;
    default:
        return std::nullopt;
    }
}

std::optional<TokenKind> BinaryToken(const LirOpcode opcode) {
    switch (opcode) {
    case LirOpcode::Add:
        return TokenKind::Plus;
    case LirOpcode::Sub:
        return TokenKind::Minus;
    case LirOpcode::Mul:
        return TokenKind::Star;
    case LirOpcode::Div:
        return TokenKind::Slash;
    case LirOpcode::Mod:
        return TokenKind::Percent;
    case LirOpcode::Pow:
        return TokenKind::StarStar;
    case LirOpcode::And:
        return TokenKind::Amp;
    case LirOpcode::Or:
        return TokenKind::Pipe;
    case LirOpcode::Xor:
        return TokenKind::Caret;
    case LirOpcode::Shl:
        return TokenKind::LessLess;
    case LirOpcode::Shr:
        return TokenKind::GreaterGreater;
    case LirOpcode::Lshr:
        return TokenKind::GreaterGreaterGreater;
    case LirOpcode::CmpEq:
        return TokenKind::Equal;
    case LirOpcode::CmpNe:
        return TokenKind::BangEqual;
    case LirOpcode::CmpLt:
        return TokenKind::Less;
    case LirOpcode::CmpLe:
        return TokenKind::LessEqual;
    case LirOpcode::CmpGt:
        return TokenKind::Greater;
    case LirOpcode::CmpGe:
        return TokenKind::GreaterEqual;
    default:
        return std::nullopt;
    }
}

std::optional<KnownValue> Evaluate(const LirInstr &instruction, const Values &values) {
    if (instruction.op == LirOpcode::Const) {
        if (auto value = ParseConstant(instruction.strArg, instruction.type)) {
            return KnownValue{std::move(value), std::nullopt};
        }
        return std::nullopt;
    }
    if (instruction.op == LirOpcode::Phi) {
        std::optional<KnownValue> joined;
        for (const auto &[reg, predecessor] : instruction.phiPreds) {
            static_cast<void>(predecessor);
            KnownValue incoming = Resolve(values, reg);
            if (joined && !SameValue(*joined, incoming)) {
                return std::nullopt;
            }
            joined = std::move(incoming);
        }
        if (joined && joined->copy == instruction.dst) {
            return std::nullopt;
        }
        return joined;
    }
    if (const auto token = UnaryToken(instruction.op); token && instruction.srcs.size() == 1) {
        const KnownValue operand = Resolve(values, instruction.srcs[0]);
        if (operand.constant) {
            if (auto result = EvaluateUnary(*token, *operand.constant)) {
                return KnownValue{std::move(result), std::nullopt};
            }
        }
        return std::nullopt;
    }
    if (const auto token = BinaryToken(instruction.op); token && instruction.srcs.size() == 2) {
        const KnownValue left = Resolve(values, instruction.srcs[0]);
        const KnownValue right = Resolve(values, instruction.srcs[1]);
        if (left.constant && right.constant) {
            if (auto result = EvaluateBinary(*token, *left.constant, *right.constant)) {
                return KnownValue{std::move(result), std::nullopt};
            }
        }
        return std::nullopt;
    }
    if (instruction.op == LirOpcode::Cast && instruction.srcs.size() == 1) {
        const KnownValue source = Resolve(values, instruction.srcs[0]);
        if (source.constant) {
            if (auto result = CastConstant(*source.constant, instruction.type)) {
                return KnownValue{std::move(result), std::nullopt};
            }
        }
    }
    return std::nullopt;
}

Values Analyze(const LirFunc &function) {
    Values values;
    std::size_t definitions = function.params.size();
    for (const auto &block : function.blocks) {
        definitions += std::ranges::count_if(block.instrs,
                                             [](const LirInstr &instruction) { return instruction.dst != LirNoReg; });
    }

    for (std::size_t iteration = 0; iteration <= definitions; ++iteration) {
        bool changed = false;
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.dst == LirNoReg) {
                    continue;
                }
                const auto evaluated = Evaluate(instruction, values);
                const auto previous = values.find(instruction.dst);
                if (evaluated && (previous == values.end() || !SameValue(previous->second, *evaluated))) {
                    values[instruction.dst] = *evaluated;
                    changed = true;
                }
            }
        }
        if (!changed) {
            break;
        }
    }
    return values;
}

bool IsPureUser(const LirOpcode opcode) {
    return UnaryToken(opcode).has_value() || BinaryToken(opcode).has_value() || opcode == LirOpcode::Cast ||
           opcode == LirOpcode::FieldPtr || opcode == LirOpcode::IndexPtr || opcode == LirOpcode::Phi;
}

bool RewriteCopy(LirReg &reg, const Values &values) {
    const KnownValue value = Resolve(values, reg);
    if (!value.copy || *value.copy == reg) {
        return false;
    }
    reg = *value.copy;
    return true;
}

bool RewriteFunction(LirFunc &function, const Values &values) {
    bool changed = false;
    for (auto &block : function.blocks) {
        for (auto &instruction : block.instrs) {
            if (instruction.op != LirOpcode::Const) {
                const auto value = values.find(instruction.dst);
                if (value != values.end() && value->second.constant) {
                    LirInstr constant;
                    constant.dst = instruction.dst;
                    constant.type = value->second.constant->Type();
                    constant.op = LirOpcode::Const;
                    constant.strArg = value->second.constant->ToLiteral();
                    instruction = std::move(constant);
                    changed = true;
                    continue;
                }
            }
            if (!IsPureUser(instruction.op)) {
                continue;
            }
            for (auto &source : instruction.srcs) {
                changed = RewriteCopy(source, values) || changed;
            }
            for (auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                changed = RewriteCopy(source, values) || changed;
            }
        }
        if (block.term) {
            if (block.term->kind == LirTermKind::Branch || block.term->kind == LirTermKind::Switch) {
                changed = RewriteCopy(block.term->cond, values) || changed;
            }
            if (block.term->retVal) {
                changed = RewriteCopy(*block.term->retVal, values) || changed;
            }
        }
    }
    return changed;
}
} // namespace

std::string_view LirConstantPropagation::Name() const noexcept {
    return "lir-constant-copy-propagation";
}

PassChange LirConstantPropagation::Run(LirPackage &package, const PassContext &) {
    bool changed = false;
    for (auto &module : package.modules) {
        for (auto &function : module.funcs) {
            if (!function.isExtern && !function.isAsm) {
                changed = RewriteFunction(function, Analyze(function)) || changed;
            }
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization

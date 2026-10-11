#include "Optimization/LirScalarCombines.h"

#include "Optimization/ConstantEvaluator.h"

#include <algorithm>
#include <bit>
#include <unordered_map>

namespace Rux::Optimization {
namespace {
struct UnsignedRange {
    std::uint64_t minimum;
    std::uint64_t maximum;
};

std::optional<std::uint64_t> UnsignedMaximum(const TypeRef &type) {
    switch (type.kind) {
    case TypeRef::Kind::UInt8:
        return UINT8_MAX;
    case TypeRef::Kind::UInt16:
        return UINT16_MAX;
    case TypeRef::Kind::UInt32:
        return UINT32_MAX;
    case TypeRef::Kind::UInt64:
    case TypeRef::Kind::UInt:
        return UINT64_MAX;
    default:
        return std::nullopt;
    }
}

// Facts describe a value's representation, independent of control flow. Unknown definitions, phis and overflowing
// arithmetic retain their complete unsigned range. Bounded recursion prevents deep expression graphs consuming the
// host stack; caching a conservative result is safe even if a later query could establish a tighter bound.
std::optional<UnsignedRange> RangeOf(const LirReg reg, const std::unordered_map<LirReg, LirInstr> &definitions,
                                     std::unordered_map<LirReg, UnsignedRange> &ranges, const unsigned depth = 0) {
    if (const auto found = ranges.find(reg); found != ranges.end())
        return found->second;
    const auto found = definitions.find(reg);
    if (found == definitions.end())
        return std::nullopt;
    const auto &instruction = found->second;
    const auto maximum = UnsignedMaximum(instruction.type);
    if (!maximum)
        return std::nullopt;
    UnsignedRange result{0, *maximum};
    if (depth >= 48)
        return result;
    if (instruction.op == LirOpcode::Const) {
        if (const auto value = ParseConstant(instruction.strArg, instruction.type))
            result = {value->RawBits(), value->RawBits()};
    }
    else if (instruction.op == LirOpcode::Cast && instruction.srcs.size() == 1) {
        if (const auto source = RangeOf(instruction.srcs[0], definitions, ranges, depth + 1);
            source && source->maximum <= *maximum)
            result = *source;
    }
    else if (instruction.srcs.size() == 2 && instruction.op != LirOpcode::Phi) {
        const auto left = RangeOf(instruction.srcs[0], definitions, ranges, depth + 1);
        const auto right = RangeOf(instruction.srcs[1], definitions, ranges, depth + 1);
        if (left && right) {
            switch (instruction.op) {
            case LirOpcode::And:
                result.maximum = std::min(left->maximum, right->maximum);
                break;
            case LirOpcode::Add:
                if (left->maximum <= *maximum && right->maximum <= *maximum - left->maximum)
                    result = {left->minimum + right->minimum, left->maximum + right->maximum};
                break;
            case LirOpcode::Sub:
                if (left->minimum >= right->maximum && left->maximum <= *maximum)
                    result = {left->minimum - right->maximum, left->maximum - right->minimum};
                break;
            case LirOpcode::Mul:
                if (left->maximum == 0 || right->maximum <= *maximum / left->maximum)
                    result = {left->minimum * right->minimum, left->maximum * right->maximum};
                break;
            case LirOpcode::Div:
                if (right->minimum > 0)
                    result = {left->minimum / right->maximum, left->maximum / right->minimum};
                break;
            case LirOpcode::Mod:
                if (right->minimum > 0)
                    result.maximum = std::min(left->maximum, right->maximum - 1);
                break;
            default:
                break;
            }
        }
    }
    // Hand-built mixed-width fixtures must not create facts outside the result's representation.
    if (result.maximum > *maximum)
        result = {0, *maximum};
    ranges.emplace(reg, result);
    return result;
}

std::optional<bool> CompareRanges(const LirOpcode opcode, const UnsignedRange left, const UnsignedRange right) {
    switch (opcode) {
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
        if (left.maximum < right.minimum || right.maximum < left.minimum)
            return opcode == LirOpcode::CmpNe;
        if (left.minimum == left.maximum && right.minimum == right.maximum)
            return (left.minimum == right.minimum) == (opcode == LirOpcode::CmpEq);
        break;
    case LirOpcode::CmpLt:
        if (left.maximum < right.minimum)
            return true;
        if (left.minimum >= right.maximum)
            return false;
        break;
    case LirOpcode::CmpLe:
        if (left.maximum <= right.minimum)
            return true;
        if (left.minimum > right.maximum)
            return false;
        break;
    case LirOpcode::CmpGt:
        return CompareRanges(LirOpcode::CmpLt, right, left);
    case LirOpcode::CmpGe:
        return CompareRanges(LirOpcode::CmpLe, right, left);
    default:
        break;
    }
    return std::nullopt;
}

bool Combine(LirFunc &function) {
    std::unordered_map<LirReg, LirInstr> definitions;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        definitions[parameter.reg] = {};
        definitions[parameter.reg].op = LirOpcode::Phi; // Unknown runtime input, not a literal.
        definitions[parameter.reg].type = parameter.type;
        next = std::max(next, parameter.reg + 1);
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg) {
                if (!definitions.emplace(instruction.dst, instruction).second) {
                    return false;
                }
                next = std::max(next, instruction.dst + 1);
            }
        }
    }
    const auto constant = [&](const LirReg reg) -> std::optional<std::uint64_t> {
        const auto found = definitions.find(reg);
        if (found == definitions.end() || found->second.op != LirOpcode::Const || !found->second.type.IsInteger()) {
            return std::nullopt;
        }
        const auto value = ParseConstant(found->second.strArg, found->second.type);
        return value && value->Width() <= 64 ? std::optional{value->RawBits()} : std::nullopt;
    };
    bool changed = false;
    std::unordered_map<LirReg, UnsignedRange> ranges;
    for (auto &block : function.blocks) {
        std::vector<LirInstr> output;
        for (auto instruction : block.instrs) {
            if (instruction.type == TypeRef::MakeBool() && instruction.srcs.size() == 2 && !instruction.isVolatile &&
                definitions.contains(instruction.srcs[0]) && definitions.contains(instruction.srcs[1]) &&
                definitions.at(instruction.srcs[0]).type == definitions.at(instruction.srcs[1]).type) {
                const auto left = RangeOf(instruction.srcs[0], definitions, ranges);
                const auto right = RangeOf(instruction.srcs[1], definitions, ranges);
                if (left && right) {
                    if (const auto result = CompareRanges(instruction.op, *left, *right)) {
                        instruction.op = LirOpcode::Const;
                        instruction.srcs.clear();
                        instruction.strArg = *result ? "true" : "false";
                        changed = true;
                    }
                }
            }
            // Keep a deliberately narrow contract: no signed arithmetic, wide integers or floating identities.
            const bool word =
                instruction.type.kind == TypeRef::Kind::UInt32 || instruction.type.kind == TypeRef::Kind::UInt64;
            if (word && instruction.srcs.size() == 2 && instruction.op == LirOpcode::Or) {
                const auto left = definitions.find(instruction.srcs[0]);
                const auto right = definitions.find(instruction.srcs[1]);
                if (left != definitions.end() && right != definitions.end()) {
                    const auto *shiftLeft = &left->second;
                    const auto *shiftRight = &right->second;
                    if (shiftRight->op == LirOpcode::Shl) {
                        std::swap(shiftLeft, shiftRight);
                    }
                    if (shiftLeft->op == LirOpcode::Shl &&
                        (shiftRight->op == LirOpcode::Shr || shiftRight->op == LirOpcode::Lshr) &&
                        shiftLeft->srcs.size() == 2 && shiftRight->srcs.size() == 2 &&
                        shiftLeft->srcs[0] == shiftRight->srcs[0] && shiftLeft->type == instruction.type &&
                        shiftRight->type == instruction.type) {
                        const auto a = constant(shiftLeft->srcs[1]);
                        const auto b = constant(shiftRight->srcs[1]);
                        const auto width = instruction.type.kind == TypeRef::Kind::UInt32 ? 32U : 64U;
                        if (a && b && *a > 0 && *a < width && *b > 0 && *b < width && *a + *b == width) {
                            instruction.op = LirOpcode::RotateRight;
                            instruction.srcs = {shiftLeft->srcs[0]};
                            instruction.strArg = std::to_string(*b);
                            changed = true;
                        }
                    }
                }
            }
            // A logical right shift by c leaves only the low width - c bits; a mask keeping all of them changes
            // nothing, so the and becomes the shift itself. This is what lets a rotation written through a masked
            // shift helper be recognized as one.
            if (word && instruction.srcs.size() == 2 && instruction.op == LirOpcode::And) {
                for (std::size_t side = 0; side < 2; ++side) {
                    const auto shift = definitions.find(instruction.srcs[side]);
                    const auto mask = constant(instruction.srcs[1 - side]);
                    if (shift == definitions.end() || !mask ||
                        (shift->second.op != LirOpcode::Lshr && shift->second.op != LirOpcode::Shr) ||
                        shift->second.type != instruction.type || shift->second.srcs.size() != 2 ||
                        shift->second.isVolatile)
                        continue;
                    const auto count = constant(shift->second.srcs[1]);
                    const auto width = instruction.type.kind == TypeRef::Kind::UInt32 ? 32U : 64U;
                    if (!count || *count == 0 || *count >= width)
                        continue;
                    const std::uint64_t kept = (std::uint64_t{1} << (width - *count)) - 1;
                    if ((*mask & kept) != kept)
                        continue;
                    instruction.op = shift->second.op;
                    instruction.srcs = shift->second.srcs;
                    changed = true;
                    break;
                }
            }
            if (word && instruction.srcs.size() == 2 &&
                (instruction.op == LirOpcode::Div || instruction.op == LirOpcode::Mod)) {
                const auto divisor = constant(instruction.srcs[1]);
                if (divisor && std::has_single_bit(*divisor)) {
                    LirInstr immediate;
                    immediate.op = LirOpcode::Const;
                    immediate.dst = next++;
                    immediate.type = instruction.type;
                    immediate.strArg =
                        std::to_string(instruction.op == LirOpcode::Div ? std::countr_zero(*divisor) : *divisor - 1);
                    instruction.srcs[1] = immediate.dst;
                    instruction.op = instruction.op == LirOpcode::Div ? LirOpcode::Lshr : LirOpcode::And;
                    output.push_back(std::move(immediate));
                    changed = true;
                }
            }
            // Adding two constants in turn adds their sum, which leaves one addition where an unrolled loop stepped
            // its count twice. Unsigned arithmetic wraps the same either way.
            if (instruction.op == LirOpcode::Add && instruction.srcs.size() == 2 && !instruction.isVolatile &&
                instruction.type.IsInteger() && !instruction.type.IsSigned()) {
                const auto inner = definitions.find(instruction.srcs[0]);
                const auto outer = constant(instruction.srcs[1]);
                const auto maximum = UnsignedMaximum(instruction.type);
                if (inner != definitions.end() && outer && maximum && inner->second.op == LirOpcode::Add &&
                    inner->second.type == instruction.type && inner->second.srcs.size() == 2 &&
                    !inner->second.isVolatile) {
                    if (const auto first = constant(inner->second.srcs[1])) {
                        LirInstr sum;
                        sum.op = LirOpcode::Const;
                        sum.dst = next++;
                        sum.type = instruction.type;
                        sum.strArg = std::to_string((*first + *outer) & *maximum);
                        instruction.srcs = {inner->second.srcs[0], sum.dst};
                        output.push_back(std::move(sum));
                        changed = true;
                    }
                }
            }
            output.push_back(std::move(instruction));
        }
        block.instrs = std::move(output);
    }
    return changed;
}
} // namespace

std::string_view LirScalarCombines::Name() const noexcept {
    return "lir-scalar-combines";
}

PassChange LirScalarCombines::Run(LirPackage &package, const PassContext &) {
    bool changed = false;
    for (auto &module : package.modules) {
        for (auto &function : module.funcs) {
            changed = Combine(function) || changed;
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization

#include "Optimization/LirValueNumbering.h"

#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
bool Pure(const LirOpcode op) {
    switch (op) {
    case LirOpcode::Const:
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
        return true;
    default:
        return false;
    }
}

std::string Key(const LirInstr &instruction) {
    const auto type = instruction.type.ToString();
    std::string key = std::to_string(static_cast<unsigned>(instruction.op)) + ":" + std::to_string(type.size()) + ":" +
                      type + ":" + std::to_string(instruction.strArg.size()) + ":" + instruction.strArg;
    for (const auto source : instruction.srcs) {
        key += ":" + std::to_string(source);
    }
    return key;
}

bool Number(LirFunc &function) {
    std::unordered_set<LirReg> definitions;
    for (const auto &parameter : function.params) {
        if (!definitions.insert(parameter.reg).second) {
            return false;
        }
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg && !definitions.insert(instruction.dst).second) {
                return false;
            }
        }
    }
    std::unordered_map<LirReg, LirReg> copies;
    const auto resolve = [&](LirReg reg) {
        for (std::size_t depth = 0; depth < copies.size(); ++depth) {
            const auto found = copies.find(reg);
            if (found == copies.end()) {
                break;
            }
            reg = found->second;
        }
        return reg;
    };
    bool changed = false;
    for (auto &block : function.blocks) {
        // Available values are deliberately block-local, so every replacement dominates its use.
        std::unordered_map<std::string, LirReg> values, loads;
        std::vector<LirInstr> retained;
        for (auto instruction : block.instrs) {
            for (auto &source : instruction.srcs) {
                source = resolve(source);
            }
            const bool load =
                instruction.op == LirOpcode::Load && instruction.srcs.size() == 1 && instruction.strArg.empty() &&
                !instruction.isVolatile &&
                (instruction.type.IsInteger() || instruction.type.IsFloat() || instruction.type.IsBool() ||
                 instruction.type.IsChar() || instruction.type.kind == TypeRef::Kind::Pointer);
            const bool pure = Pure(instruction.op) && !instruction.isVolatile;
            if (instruction.dst != LirNoReg && (pure || load)) {
                auto &available = load ? loads : values;
                const auto [entry, inserted] = available.emplace(Key(instruction), instruction.dst);
                if (!inserted) {
                    copies[instruction.dst] = entry->second;
                    changed = true;
                    continue;
                }
            }
            // No alias promise is made about pointers. Any possible write or ordering effect ends load reuse.
            // Reusing an already executed load never introduces a new speculative access or erases the first trap.
            if (!pure && instruction.op != LirOpcode::Load && instruction.op != LirOpcode::Alloca &&
                instruction.op != LirOpcode::Phi) {
                loads.clear();
                values.clear();
            }
            if (instruction.isVolatile) {
                loads.clear();
                values.clear();
            }
            retained.push_back(std::move(instruction));
        }
        block.instrs = std::move(retained);
    }
    for (auto &block : function.blocks) {
        for (auto &instruction : block.instrs) {
            for (auto &source : instruction.srcs) {
                source = resolve(source);
            }
            for (auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                source = resolve(source);
            }
        }
        if (block.term) {
            block.term->cond = resolve(block.term->cond);
            if (block.term->retVal) {
                block.term->retVal = resolve(*block.term->retVal);
            }
        }
    }
    return changed;
}
} // namespace

std::string_view LirValueNumbering::Name() const noexcept {
    return "lir-local-value-numbering";
}

PassChange LirValueNumbering::Run(LirPackage &package, const PassContext &) {
    bool changed = false;
    for (auto &module : package.modules) {
        for (auto &function : module.funcs) {
            changed = Number(function) || changed;
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization

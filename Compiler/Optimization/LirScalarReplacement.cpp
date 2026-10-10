#include "Optimization/LirScalarReplacement.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
struct Candidate {
    std::map<std::string, TypeRef> fields;
    std::unordered_set<LirReg> addresses;
    bool valid = true;
    bool range = false;
};

bool ReplaceFields(LirFunc &function) {
    std::map<LirReg, Candidate> candidates;
    std::unordered_map<LirReg, std::pair<LirReg, std::string>> fields;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        next = std::max(next, parameter.reg + 1);
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg) {
                next = std::max(next, instruction.dst + 1);
            }
            if (instruction.op == LirOpcode::Alloca && instruction.strArg.empty() &&
                (instruction.type.IsRange() || instruction.type.IsSlice())) {
                candidates[instruction.dst].range = instruction.type.IsRange();
            }
        }
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.op != LirOpcode::FieldPtr || instruction.srcs.size() != 1 ||
                !candidates.contains(instruction.srcs[0])) {
                continue;
            }
            auto &candidate = candidates.at(instruction.srcs[0]);
            const bool knownField = candidate.range ? instruction.strArg == "start" || instruction.strArg == "end"
                                                    : instruction.strArg == "data" || instruction.strArg == "length";
            if (!knownField) {
                candidate.valid = false;
                continue;
            }
            if (instruction.type.kind != TypeRef::Kind::Pointer || instruction.type.inner.size() != 1) {
                candidate.valid = false;
                continue;
            }
            const auto &type = instruction.type.inner[0];
            const auto [field, added] = candidate.fields.emplace(instruction.strArg, type);
            candidate.valid = candidate.valid && (added || field->second == type);
            candidate.addresses.insert(instruction.dst);
            fields.emplace(instruction.dst, std::pair{instruction.srcs[0], instruction.strArg});
        }
    }
    const auto reject = [&](const LirReg reg) {
        if (candidates.contains(reg)) {
            candidates.at(reg).valid = false;
        }
        if (fields.contains(reg)) {
            candidates.at(fields.at(reg).first).valid = false;
        }
    };
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            for (std::size_t index = 0; index < instruction.srcs.size(); ++index) {
                const auto source = instruction.srcs[index];
                if (candidates.contains(source) &&
                    (instruction.op != LirOpcode::FieldPtr || instruction.srcs.size() != 1)) {
                    reject(source);
                }
                if (const auto field = fields.find(source); field != fields.end()) {
                    const auto &type = candidates.at(field->second.first).fields.at(field->second.second);
                    const bool load = instruction.op == LirOpcode::Load && index == 0 && instruction.srcs.size() == 1;
                    const bool store = instruction.op == LirOpcode::Store && index == 1 && instruction.srcs.size() == 2;
                    if ((!load && !store) || instruction.isVolatile || instruction.type != type) {
                        reject(source);
                    }
                }
            }
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                reject(source);
            }
        }
        if (block.term) {
            reject(block.term->cond);
            if (block.term->retVal) {
                reject(*block.term->retVal);
            }
        }
    }
    std::unordered_map<LirReg, LirReg> replacements;
    std::map<LirReg, std::vector<LirInstr>> allocas;
    for (const auto &[reg, candidate] : candidates) {
        if (!candidate.valid || candidate.fields.empty()) {
            continue;
        }
        for (const auto &[name, type] : candidate.fields) {
            LirInstr allocation;
            allocation.op = LirOpcode::Alloca;
            allocation.type = type;
            allocation.dst = next++;
            for (const auto address : candidate.addresses) {
                if (fields.at(address).second == name) {
                    replacements.emplace(address, allocation.dst);
                }
            }
            allocas[reg].push_back(std::move(allocation));
        }
    }
    if (allocas.empty()) {
        return false;
    }
    for (auto &block : function.blocks) {
        std::vector<LirInstr> instructions;
        for (auto &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Alloca && allocas.contains(instruction.dst)) {
                for (const auto &allocation : allocas.at(instruction.dst)) {
                    instructions.push_back(allocation);
                }
                continue;
            }
            if (instruction.op == LirOpcode::FieldPtr && replacements.contains(instruction.dst)) {
                continue;
            }
            for (auto &source : instruction.srcs) {
                if (replacements.contains(source)) {
                    source = replacements.at(source);
                }
            }
            instructions.push_back(std::move(instruction));
        }
        block.instrs = std::move(instructions);
    }
    return true;
}
} // namespace

std::string_view LirScalarReplacement::Name() const noexcept {
    return "lir-scalar-replacement";
}

PassChange LirScalarReplacement::Run(LirPackage &package, const PassContext &) {
    bool changed = false;
    for (auto &module : package.modules) {
        for (auto &function : module.funcs) {
            changed = ReplaceFields(function) || changed;
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization

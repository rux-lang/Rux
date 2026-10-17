#include "Optimization/LirViewParameters.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
bool Snapshot(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.empty())
        return false;
    std::unordered_set<LirReg> candidates;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        next = std::max(next, parameter.reg + 1);
        if (parameter.byAddress)
            candidates.insert(parameter.reg);
    }
    if (candidates.empty())
        return false;
    // Field addresses computed from a candidate, by the field they name.
    std::unordered_map<LirReg, std::pair<LirReg, const LirInstr *>> fields;
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg)
                next = std::max(next, instruction.dst + 1);
            if (instruction.op == LirOpcode::FieldPtr && instruction.srcs.size() == 1 &&
                candidates.contains(instruction.srcs[0]) && instruction.dst != LirNoReg)
                fields.emplace(instruction.dst, std::pair{instruction.srcs[0], &instruction});
        }
    }
    // A candidate stays one only if its address reaches nothing but field reads and call arguments. A callee cannot
    // write through it either: the callee's parameter is a by-value parameter of its own.
    std::unordered_set<LirReg> rejected;
    const auto reject = [&](const LirReg reg) {
        if (candidates.contains(reg))
            rejected.insert(reg);
        if (const auto field = fields.find(reg); field != fields.end())
            rejected.insert(field->second.first);
    };
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            for (std::size_t index = 0; index < instruction.srcs.size(); ++index) {
                const LirReg source = instruction.srcs[index];
                if (candidates.contains(source)) {
                    const bool argument =
                        (instruction.op == LirOpcode::Call) || (instruction.op == LirOpcode::CallIndirect && index > 0);
                    if (!argument && instruction.op != LirOpcode::FieldPtr)
                        reject(source);
                }
                if (fields.contains(source) && (instruction.op != LirOpcode::Load || index != 0 ||
                                                instruction.isVolatile || !instruction.strArg.empty()))
                    reject(source);
            }
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                reject(source);
            }
        }
        if (const auto &term = block.term) {
            reject(term->cond);
            if (term->retVal)
                reject(*term->retVal);
        }
    }
    // One entry load per parameter, field and loaded type.
    std::map<std::tuple<LirReg, std::string, std::string>, LirReg> snapshots;
    std::unordered_map<LirReg, LirReg> replaced;
    std::vector<LirInstr> entry;
    for (auto &block : function.blocks) {
        for (auto &instruction : block.instrs) {
            if (instruction.op != LirOpcode::Load || instruction.srcs.size() != 1 || instruction.dst == LirNoReg)
                continue;
            const auto field = fields.find(instruction.srcs[0]);
            if (field == fields.end() || rejected.contains(field->second.first))
                continue;
            const LirInstr &address = *field->second.second;
            const auto key = std::tuple{field->second.first, address.strArg, instruction.type.ToString()};
            auto snapshot = snapshots.find(key);
            if (snapshot == snapshots.end()) {
                LirInstr fieldAddress = address;
                fieldAddress.dst = next++;
                LirInstr load = instruction;
                load.dst = next++;
                load.srcs = {fieldAddress.dst};
                entry.push_back(std::move(fieldAddress));
                entry.push_back(load);
                snapshot = snapshots.emplace(key, load.dst).first;
            }
            replaced.emplace(instruction.dst, snapshot->second);
        }
    }
    if (replaced.empty())
        return false;
    const auto rename = [&](LirReg &reg) {
        if (const auto found = replaced.find(reg); found != replaced.end())
            reg = found->second;
    };
    for (auto &block : function.blocks) {
        std::erase_if(block.instrs, [&](const LirInstr &instruction) {
            return instruction.dst != LirNoReg && replaced.contains(instruction.dst);
        });
        for (auto &instruction : block.instrs) {
            for (auto &source : instruction.srcs)
                rename(source);
            for (auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                rename(source);
            }
        }
        if (block.term) {
            rename(block.term->cond);
            if (block.term->retVal)
                rename(*block.term->retVal);
        }
    }
    auto &instructions = function.blocks.front().instrs;
    instructions.insert(instructions.begin(), std::make_move_iterator(entry.begin()),
                        std::make_move_iterator(entry.end()));
    return true;
}
} // namespace

std::string_view LirViewParameters::Name() const noexcept {
    return "lir-view-parameters";
}

PassChange LirViewParameters::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    return Snapshot(function) ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization

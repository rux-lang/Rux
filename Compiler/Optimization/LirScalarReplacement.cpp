#include "Optimization/LirScalarReplacement.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
using Fields = std::map<std::string, TypeRef>;
using Shapes = std::unordered_map<std::string, Fields>;

Fields Shape(const TypeRef &type, const Shapes &structs) {
    Fields fields;
    if (type.IsRange()) {
        const auto element = type.inner.empty() ? TypeRef::MakeInt64() : type.inner[0];
        if (type.RangeHasStart())
            fields.emplace("start", element);
        if (type.RangeHasEnd())
            fields.emplace("end", element);
    }
    else if (type.IsSlice() && type.inner.size() == 1) {
        fields.emplace("data", TypeRef::MakePointer(type.inner[0]));
        fields.emplace("length", TypeRef::MakeUInt64());
    }
    else if (type.kind == TypeRef::Kind::Tuple && type.inner.size() <= 16) {
        for (std::size_t i = 0; i < type.inner.size(); ++i)
            fields.emplace(std::to_string(i), type.inner[i]);
    }
    else if (type.kind == TypeRef::Kind::Named && type.inner.empty()) {
        if (const auto found = structs.find(type.name); found != structs.end())
            fields = found->second;
    }
    if (!std::ranges::all_of(fields, [](const auto &field) {
            const auto &value = field.second;
            return value.IsInteger() || value.IsFloat() || value.IsBool() || value.IsChar() ||
                   value.kind == TypeRef::Kind::Pointer || value.kind == TypeRef::Kind::Func;
        })) {
        return {};
    }
    return fields;
}

struct Candidate {
    TypeRef type;
    Fields fields;
    std::unordered_set<LirReg> addresses;
    std::vector<LirReg> dependencies;
    bool valid = true;
};

bool ReplaceFields(LirFunc &function, const Shapes &structs) {
    std::map<LirReg, Candidate> candidates;
    std::unordered_map<LirReg, std::pair<LirReg, std::string>> fields;
    std::unordered_map<LirReg, LirReg> copies;
    LirReg next = 0;
    for (const auto &parameter : function.params)
        next = std::max(next, parameter.reg + 1);
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg)
                next = std::max(next, instruction.dst + 1);
            if (instruction.op == LirOpcode::Alloca && instruction.strArg.empty()) {
                auto shape = Shape(instruction.type, structs);
                if (!shape.empty())
                    candidates.emplace(instruction.dst, Candidate{instruction.type, std::move(shape), {}, {}, true});
            }
        }
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.srcs.size() != 1 || !candidates.contains(instruction.srcs[0]))
                continue;
            auto &candidate = candidates.at(instruction.srcs[0]);
            if (instruction.op == LirOpcode::Load && instruction.type == candidate.type && !instruction.isVolatile &&
                instruction.strArg.empty()) {
                copies.emplace(instruction.dst, instruction.srcs[0]);
            }
            if (instruction.op != LirOpcode::FieldPtr)
                continue;
            const auto field = candidate.fields.find(instruction.strArg);
            if (field == candidate.fields.end() || instruction.type.kind != TypeRef::Kind::Pointer ||
                instruction.type.inner.size() != 1 || instruction.type.inner[0] != field->second) {
                candidate.valid = false;
                continue;
            }
            candidate.addresses.insert(instruction.dst);
            fields.emplace(instruction.dst, std::pair{instruction.srcs[0], instruction.strArg});
        }
    }
    const auto reject = [&](const LirReg reg) {
        if (candidates.contains(reg))
            candidates.at(reg).valid = false;
        if (fields.contains(reg))
            candidates.at(fields.at(reg).first).valid = false;
        if (copies.contains(reg))
            candidates.at(copies.at(reg)).valid = false;
    };
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            bool copyStore = instruction.op == LirOpcode::Store && instruction.srcs.size() == 2 &&
                             !instruction.isVolatile && copies.contains(instruction.srcs[0]) &&
                             candidates.contains(instruction.srcs[1]);
            if (copyStore) {
                const auto source = copies.at(instruction.srcs[0]);
                const auto destination = instruction.srcs[1];
                copyStore = instruction.type == candidates.at(source).type &&
                            instruction.type == candidates.at(destination).type;
                if (copyStore) {
                    candidates.at(source).dependencies.push_back(destination);
                    candidates.at(destination).dependencies.push_back(source);
                }
            }
            for (std::size_t index = 0; index < instruction.srcs.size(); ++index) {
                const auto source = instruction.srcs[index];
                if (candidates.contains(source)) {
                    const bool projection = instruction.op == LirOpcode::FieldPtr && instruction.srcs.size() == 1;
                    const bool copyLoad = instruction.op == LirOpcode::Load && copies.contains(instruction.dst);
                    if (!projection && !copyLoad && !(copyStore && index == 1))
                        reject(source);
                }
                if (copies.contains(source) && !(copyStore && index == 0))
                    reject(source);
                if (const auto field = fields.find(source); field != fields.end()) {
                    const auto &type = candidates.at(field->second.first).fields.at(field->second.second);
                    const bool load = instruction.op == LirOpcode::Load && index == 0 && instruction.srcs.size() == 1 &&
                                      instruction.strArg.empty();
                    const bool store = instruction.op == LirOpcode::Store && index == 1 && instruction.srcs.size() == 2;
                    if ((!load && !store) || instruction.isVolatile || instruction.type != type)
                        reject(source);
                }
            }
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                reject(source);
            }
        }
        if (block.term) {
            reject(block.term->cond);
            if (block.term->retVal)
                reject(*block.term->retVal);
        }
    }
    // Copy-connected objects must be split together. Propagate escapes without a quadratic fixed-point scan.
    std::vector<LirReg> rejected;
    for (const auto &[reg, candidate] : candidates)
        if (!candidate.valid)
            rejected.push_back(reg);
    while (!rejected.empty()) {
        const auto reg = rejected.back();
        rejected.pop_back();
        for (const auto dependency : candidates.at(reg).dependencies) {
            if (candidates.at(dependency).valid) {
                candidates.at(dependency).valid = false;
                rejected.push_back(dependency);
            }
        }
    }
    std::unordered_map<LirReg, LirReg> replacements;
    std::map<LirReg, std::vector<LirInstr>> allocas;
    std::unordered_map<LirReg, std::vector<LirReg>> snapshots;
    for (const auto &[reg, candidate] : candidates) {
        if (!candidate.valid)
            continue;
        for (const auto &[name, type] : candidate.fields) {
            LirInstr allocation;
            allocation.op = LirOpcode::Alloca;
            allocation.type = type;
            allocation.dst = next++;
            for (const auto address : candidate.addresses)
                if (fields.at(address).second == name)
                    replacements.emplace(address, allocation.dst);
            allocas[reg].push_back(std::move(allocation));
        }
    }
    if (allocas.empty())
        return false;
    // Allocate snapshot names before rewriting, since textual block order need not be dominance order.
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Load && copies.contains(instruction.dst) &&
                allocas.contains(copies.at(instruction.dst))) {
                for (std::size_t i = 0; i < allocas.at(copies.at(instruction.dst)).size(); ++i)
                    snapshots[instruction.dst].push_back(next++);
            }
        }
    }
    for (auto &block : function.blocks) {
        std::vector<LirInstr> instructions;
        for (auto &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Alloca && allocas.contains(instruction.dst)) {
                for (const auto &allocation : allocas.at(instruction.dst))
                    instructions.push_back(allocation);
                continue;
            }
            if (instruction.op == LirOpcode::Load && snapshots.contains(instruction.dst)) {
                const auto &storage = allocas.at(copies.at(instruction.dst));
                const auto &values = snapshots.at(instruction.dst);
                for (std::size_t i = 0; i < storage.size(); ++i) {
                    auto load = instruction;
                    load.dst = values[i];
                    load.type = storage[i].type;
                    load.srcs = {storage[i].dst};
                    instructions.push_back(std::move(load));
                }
                continue;
            }
            if (instruction.op == LirOpcode::Store && instruction.srcs.size() == 2 &&
                snapshots.contains(instruction.srcs[0])) {
                const auto &storage = allocas.at(instruction.srcs[1]);
                const auto &values = snapshots.at(instruction.srcs[0]);
                for (std::size_t i = 0; i < storage.size(); ++i) {
                    auto store = instruction;
                    store.type = storage[i].type;
                    store.srcs = {values[i], storage[i].dst};
                    instructions.push_back(std::move(store));
                }
                continue;
            }
            if (instruction.op == LirOpcode::FieldPtr && replacements.contains(instruction.dst))
                continue;
            for (auto &source : instruction.srcs)
                if (replacements.contains(source))
                    source = replacements.at(source);
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

void LirScalarReplacement::Prepare(const LirPackage &package) {
    structs.clear();
    std::unordered_set<std::string> ambiguous;
    for (const auto &module : package.modules) {
        for (const auto &declaration : module.structs) {
            if (!declaration.typeParams.empty() || declaration.fields.size() > 16)
                continue;
            Fields shape;
            for (const auto &field : declaration.fields)
                shape.emplace(field.name, field.type);
            // A generic instance is declared by every module that uses it, always with the same fields, so only a
            // second declaration of a different shape makes a name ambiguous.
            const auto [existing, inserted] = structs.emplace(declaration.name, shape);
            if (!inserted && existing->second != shape)
                ambiguous.insert(declaration.name);
        }
        // Never resolve a coincident short name to a struct when another category owns it.
        for (const auto &declaration : module.unions)
            ambiguous.insert(declaration.name);
        for (const auto &declaration : module.enums)
            ambiguous.insert(declaration.name);
        for (const auto &name : module.interfaceNames)
            ambiguous.insert(name);
        for (const auto &declaration : module.typeAliases)
            ambiguous.insert(declaration.name);
    }
    for (const auto &name : ambiguous)
        structs.erase(name);
}

PassChange LirScalarReplacement::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    return ReplaceFields(function, structs) ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization

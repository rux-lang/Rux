#include "Optimization/LirConstantMemory.h"

#include "Optimization/RegisterTable.h"
#include "Unicode/Utf.h"

#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
std::optional<std::size_t> Length(const LirConstDecl &constant) {
    if (!constant.hasSequenceData || constant.type.kind != TypeRef::Kind::Slice || constant.type.inner.size() != 1 ||
        constant.elementType != constant.type.inner[0])
        return std::nullopt;
    if (!constant.isTextSlice)
        return constant.elements.size();
    switch (constant.elementType.kind) {
    case TypeRef::Kind::Char8:
        return CodeUnitCount(constant.text, 1);
    case TypeRef::Kind::Char16:
        return CodeUnitCount(constant.text, 2);
    case TypeRef::Kind::Char32:
        return CodeUnitCount(constant.text, 4);
    default:
        return std::nullopt;
    }
}

bool Fold(LirFunc &function, const std::unordered_map<std::string, const LirConstDecl *> &constants) {
    // A parameter is defined by no instruction, so it stands in the table with none.
    RegisterTable<const LirInstr *> definitions(RegisterCountHint(function));
    for (const auto &parameter : function.params)
        if (!definitions.Insert(parameter.reg, nullptr))
            return false;
    for (const auto &block : function.blocks)
        for (const auto &instruction : block.instrs)
            if (instruction.dst != LirNoReg && !definitions.Insert(instruction.dst, &instruction))
                return false;
    const auto get = [&](const LirReg reg) -> const LirInstr * {
        const LirInstr *const *found = definitions.Find(reg);
        return found == nullptr ? nullptr : *found;
    };
    bool changed = false;
    for (auto &block : function.blocks) {
        for (auto &instruction : block.instrs) {
            if (instruction.op != LirOpcode::Load || instruction.isVolatile || instruction.srcs.size() != 1 ||
                !instruction.strArg.empty() || instruction.type != TypeRef::MakeUInt64())
                continue;
            const auto *field = get(instruction.srcs[0]);
            if (!field || field->op != LirOpcode::FieldPtr || field->isVolatile || field->srcs.size() != 1 ||
                field->strArg != "length" || field->type != TypeRef::MakePointer(TypeRef::MakeUInt64()))
                continue;
            const auto *address = get(field->srcs[0]);
            if (!address || address->op != LirOpcode::GlobalAddr || address->isVolatile || !address->srcs.empty())
                continue;
            const auto constant = constants.find(address->strArg);
            if (constant == constants.end() || address->type != TypeRef::MakePointer(constant->second->type))
                continue;
            const auto length = Length(*constant->second);
            if (!length)
                continue;
            // Constant headers live in read-only storage. Keep the data address and all unrelated memory accesses.
            LirInstr replacement;
            replacement.op = LirOpcode::Const;
            replacement.dst = instruction.dst;
            replacement.type = instruction.type;
            replacement.strArg = std::to_string(*length);
            instruction = std::move(replacement);
            changed = true;
        }
    }
    return changed;
}
} // namespace

std::string_view LirConstantMemory::Name() const noexcept {
    return "lir-constant-memory";
}

void LirConstantMemory::Prepare(const LirPackage &package) {
    moduleConstants.clear();
    moduleConstants.reserve(package.modules.size());
    for (const auto &module : package.modules) {
        auto &constants = moduleConstants.emplace_back();
        std::unordered_set<std::string> ambiguous;
        for (const auto &constant : module.consts)
            if (!constants.emplace(constant.name, &constant).second)
                ambiguous.insert(constant.name);
        for (const auto &global : module.globals)
            ambiguous.insert(global.name);
        for (const auto &external : module.externVars)
            ambiguous.insert(external.name);
        for (const auto &name : ambiguous)
            constants.erase(name);
    }
}

PassChange LirConstantMemory::RunFunction(LirFunc &function, const std::size_t moduleIndex, const PassContext &) const {
    if (function.isExtern || function.isAsm)
        return PassChange::None;
    // Only a load of a constant's length folds, and a module without constants has none.
    if (moduleConstants[moduleIndex].empty())
        return PassChange::None;
    return Fold(function, moduleConstants[moduleIndex]) ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization

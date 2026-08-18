#include "CodeGen/AArch64/FramePlan.h"

#include "CodeGen/LinearScan.h"

#include <algorithm>
#include <charconv>

namespace Rux {
using namespace Layout;

namespace {
constexpr std::int32_t kFrameRecordSize = 16;
constexpr unsigned kFirstCalleeSaved = 19;
constexpr int kCalleeSavedCount = 10;
constexpr unsigned kFirstCalleeSavedVector = 8;
constexpr int kCalleeSavedVectorCount = 8;
constexpr unsigned kMaxHfaMembers = 4;
} // namespace

class AArch64FramePlanner {
public:
    AArch64FramePlanner(const LirFunc &inputFunc, const LayoutMap &inputLayouts,
                        const std::unordered_set<std::string> &inputInterfaceNames,
                        const std::vector<LirStructDecl> &inputStructDecls, const Target::OS inputTargetOs)
        : func(inputFunc)
        , layouts(inputLayouts)
        , interfaceNames(inputInterfaceNames) {
        plan.targetOs = inputTargetOs;
        for (const auto &declaration : inputStructDecls) {
            structFields[declaration.name] = &declaration;
        }
    }

    [[nodiscard]] AArch64FramePlan Build() {
        PlanRegisterHomes();
        PlanIndirectResult();
        PlanCalleeSaves();
        PlanParameters();
        PlanInstructions();
        PlanWideTemporaries();
        PlanPhiTemporary();
        plan.frameSize = AlignUp(nextOffset, 16);
        return std::move(plan);
    }

private:
    const LirFunc &func;
    const LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    std::unordered_map<std::string, const LirStructDecl *> structFields;
    AArch64FramePlan plan;
    std::int32_t nextOffset = kFrameRecordSize;

    [[nodiscard]] int RuntimeSize(const TypeRef &type) const {
        return RuntimeSizeOf(type, layouts, interfaceNames);
    }

    [[nodiscard]] bool IsAggregate(const TypeRef &type) const {
        if (IsWideInteger(type)) {
            return true;
        }
        if (type.IsRange()) {
            return true;
        }
        switch (type.kind) {
        case TypeRef::Kind::Tuple:
        case TypeRef::Kind::Array:
            return true;
        case TypeRef::Kind::Named: {
            const std::string base = BaseTypeName(type.name);
            return base == "Slice" || interfaceNames.contains(base) || layouts.contains(base) ||
                   (!type.inner.empty() && SizeOf(type) > 8);
        }
        default:
            return false;
        }
    }

    [[nodiscard]] bool IsRegisterValue(const TypeRef &type) const {
        return !IsFloat(type) && !IsAggregate(type) && type.kind != TypeRef::Kind::Str;
    }

    [[nodiscard]] int SlotSize(const TypeRef &type) const {
        const int size = RuntimeSize(type);
        if (size <= 0) {
            return 8;
        }
        return IsAggregate(type) ? AlignUp(size, 8) : size;
    }

    [[nodiscard]] bool CollectFloatMembers(const TypeRef &type, TypeRef::Kind &memberKind, unsigned &count) const {
        if (count > kMaxHfaMembers) {
            return false;
        }
        if (IsFloat(type)) {
            if (count == 0) {
                memberKind = type.kind;
            }
            else if (type.kind != memberKind) {
                return false;
            }
            ++count;
            return true;
        }
        switch (type.kind) {
        case TypeRef::Kind::Tuple:
            for (const auto &element : type.inner) {
                if (!CollectFloatMembers(element, memberKind, count)) {
                    return false;
                }
            }
            return !type.inner.empty();
        case TypeRef::Kind::Array:
            if (type.inner.empty() || !type.arrayLength || *type.arrayLength == 0) {
                return false;
            }
            for (std::uint64_t index = 0; index < *type.arrayLength; ++index) {
                if (!CollectFloatMembers(type.inner[0], memberKind, count)) {
                    return false;
                }
            }
            return true;
        case TypeRef::Kind::Named: {
            const std::string base = BaseTypeName(type.name);
            if (interfaceNames.contains(base)) {
                return false;
            }
            const auto declaration = structFields.find(base);
            if (declaration == structFields.end()) {
                return false;
            }
            for (const auto &field : declaration->second->fields) {
                if (!CollectFloatMembers(field.type, memberKind, count)) {
                    return false;
                }
            }
            return !declaration->second->fields.empty();
        }
        default:
            return false;
        }
    }

    [[nodiscard]] bool ReturnsInMemory(const TypeRef &type) const {
        TypeRef::Kind memberKind = TypeRef::Kind::Float64;
        unsigned memberCount = 0;
        const bool homogeneous =
            CollectFloatMembers(type, memberKind, memberCount) && memberCount > 0 && memberCount <= kMaxHfaMembers;
        return IsAggregate(type) && RuntimeSize(type) > 16 && !homogeneous;
    }

    std::int32_t AllocateRegion(const int byteCount) {
        const int alignment = byteCount > 0 ? std::min(byteCount, 8) : 1;
        nextOffset = AlignUp(nextOffset, alignment);
        const std::int32_t offset = nextOffset;
        nextOffset += byteCount > 0 ? byteCount : 8;
        return offset;
    }

    std::int32_t AllocateSlot(const LirReg reg, const int byteCount) {
        if (const auto found = plan.slotOffsets.find(reg); found != plan.slotOffsets.end()) {
            return found->second;
        }
        const std::int32_t offset = AllocateRegion(byteCount);
        plan.slotOffsets[reg] = offset;
        return offset;
    }

    [[nodiscard]] int AllocaSize(const LirInstr &instruction) const {
        if (instruction.strArg.empty()) {
            const int size = RuntimeSize(instruction.type);
            return size > 0 ? size : 8;
        }
        int count = 0;
        const char *first = instruction.strArg.data();
        std::from_chars(first, first + instruction.strArg.size(), count);
        const TypeRef &elementType = instruction.type.inner.empty() ? instruction.type : instruction.type.inner[0];
        const int elementSize = RuntimeSize(elementType);
        const int bytes = count * (elementSize > 0 ? elementSize : 8);
        return bytes > 0 ? bytes : 8;
    }

    void PlanRegisterHomes() {
        if (func.blocks.size() != 1) {
            return;
        }

        ParamTypeMap parameterTypes;
        for (const auto &parameter : func.params) {
            parameterTypes[parameter.reg] = parameter.type;
        }

        std::vector<LiveInterval> general;
        std::vector<LiveInterval> vector;
        for (const auto &interval : ComputeLiveIntervals(func, parameterTypes)) {
            const int size = RuntimeSize(interval.type);
            if (IsFloat(interval.type)) {
                vector.push_back(interval);
            }
            else if (IsRegisterValue(interval.type) && size > 0 && size <= 8) {
                general.push_back(interval);
            }
        }

        const RegisterAssignment integers = AllocateRegisters(general, kCalleeSavedCount);
        const RegisterAssignment floats = AllocateRegisters(vector, kCalleeSavedVectorCount);
        for (const auto &[reg, index] : integers.physRegs) {
            plan.generalRegisterHomes[reg] = kFirstCalleeSaved + static_cast<unsigned>(index);
        }
        for (const auto &[reg, index] : floats.physRegs) {
            plan.vectorRegisterHomes[reg] = kFirstCalleeSavedVector + static_cast<unsigned>(index);
        }
        for (const int index : integers.usedPhysRegs) {
            plan.savedGeneralRegisters.push_back(kFirstCalleeSaved + static_cast<unsigned>(index));
        }
        for (const int index : floats.usedPhysRegs) {
            plan.savedVectorRegisters.push_back(kFirstCalleeSavedVector + static_cast<unsigned>(index));
        }
    }

    void PlanIndirectResult() {
        if (ReturnsInMemory(func.returnType)) {
            plan.indirectResultOffset = AllocateRegion(8);
        }
    }

    void PlanCalleeSaves() {
        const auto savedCount = static_cast<int>(plan.savedGeneralRegisters.size() + plan.savedVectorRegisters.size());
        if (savedCount > 0) {
            plan.calleeSaveOffset = AllocateRegion(savedCount * 8);
        }
    }

    void PlanParameters() {
        for (const auto &parameter : func.params) {
            plan.registerTypes[parameter.reg] = parameter.type;
            AllocateSlot(parameter.reg, std::max(8, SlotSize(parameter.type)));
        }
    }

    void PlanInstructions() {
        for (std::uint32_t blockIndex = 0; blockIndex < func.blocks.size(); ++blockIndex) {
            for (const auto &instruction : func.blocks[blockIndex].instrs) {
                if (instruction.op == LirOpcode::Phi) {
                    for (const auto &[source, predecessor] : instruction.phiPreds) {
                        plan.phiMoves[predecessor][blockIndex].push_back({instruction.dst, source, instruction.type});
                    }
                    plan.phiTemporarySize = std::max(plan.phiTemporarySize, std::max(8, RuntimeSize(instruction.type)));
                }
                if (instruction.dst == LirNoReg) {
                    continue;
                }
                if (instruction.op == LirOpcode::Alloca) {
                    plan.registerTypes[instruction.dst] = TypeRef::MakePointer(instruction.type);
                    AllocateSlot(instruction.dst, 8);
                    plan.allocaDataOffsets[instruction.dst] = AllocateRegion(AllocaSize(instruction));
                    continue;
                }
                plan.registerTypes[instruction.dst] = instruction.type;
                AllocateSlot(instruction.dst, SlotSize(instruction.type));
            }
        }
    }

    void PlanWideTemporaries() {
        int largest = 0;
        for (const LirBlock &block : func.blocks) {
            for (const LirInstr &instruction : block.instrs) {
                if (IsWideInteger(instruction.type)) {
                    largest = std::max(largest, RuntimeSize(instruction.type));
                }
            }
        }
        if (largest == 0) {
            return;
        }
        plan.wideTemporarySize = largest;
        for (std::int32_t &offset : plan.wideTemporaryOffsets) {
            offset = AllocateRegion(largest);
        }
    }

    void PlanPhiTemporary() {
        if (plan.phiTemporarySize > 0) {
            plan.phiTemporaryOffset = AllocateRegion(plan.phiTemporarySize);
        }
    }
};

AArch64FramePlan PlanAArch64Frame(const LirFunc &func, const LayoutMap &layouts,
                                  const std::unordered_set<std::string> &interfaceNames,
                                  const std::vector<LirStructDecl> &structDecls, const Target::OS targetOs) {
    return AArch64FramePlanner(func, layouts, interfaceNames, structDecls, targetOs).Build();
}
} // namespace Rux

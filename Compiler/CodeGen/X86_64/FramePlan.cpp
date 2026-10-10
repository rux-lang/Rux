#include "CodeGen/X86_64/FramePlan.h"

#include "CodeGen/FrameSlotSharing.h"
#include "CodeGen/LinearScan.h"
#include "CodeGen/ScalarImmediates.h"
#include "Target/CallingConvention.h"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace Rux {
using namespace Layout;

class X86_64FramePlanner {
public:
    X86_64FramePlanner(const LirFunc &inputFunc, const LayoutMap &inputLayouts,
                       const std::unordered_set<std::string> &inputInterfaceNames, const Target::OS inputTargetOs)
        : func(inputFunc)
        , layouts(inputLayouts)
        , interfaceNames(inputInterfaceNames)
        , targetOs(inputTargetOs) {
    }

    [[nodiscard]] X86_64FramePlan Build() {
        ParamTypeMap parameters;
        for (const auto &parameter : func.params) {
            parameters.emplace(parameter.reg, ParameterRegisterType(parameter.type));
        }
        const auto intervals = ComputeLiveIntervals(func, parameters);
        PlanTiedInputs(intervals);
        PlanCalleeSaves(intervals);
        nextOffset = static_cast<std::int32_t>(plan.usedPhysicalRegisters.size() * 8);
        PlanFloatRegisters(intervals);
        PlanHiddenReturn();
        PlanParameters();
        PlanInstructions();
        PlanBranches();
        PlanWideTemporaries();
        PlanPhiTemporary();

        plan.frameSize = AlignUp(nextOffset, 16);
        if (plan.frameSize == 0) {
            plan.frameSize = 16;
        }
        plan.immediateValues = ScalarImmediates(func);
        return std::move(plan);
    }

private:
    const LirFunc &func;
    const LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    Target::OS targetOs;
    X86_64FramePlan plan;
    std::int32_t nextOffset = 0;
    std::unordered_map<LirReg, LirReg> tiedInputs;

    void PlanTiedInputs(const std::vector<LiveInterval> &intervals) {
        std::unordered_map<LirReg, const LiveInterval *> spans;
        for (const auto &interval : intervals)
            spans.emplace(interval.reg, &interval);
        int position = 0;
        for (const auto &block : func.blocks) {
            for (const auto &instruction : block.instrs) {
                const bool floating =
                    instruction.type.kind == TypeRef::Kind::Float32 || instruction.type.kind == TypeRef::Kind::Float64;
                const bool word =
                    instruction.type.kind == TypeRef::Kind::Int64 || instruction.type.kind == TypeRef::Kind::UInt64 ||
                    instruction.type.kind == TypeRef::Kind::Int || instruction.type.kind == TypeRef::Kind::UInt;
                const bool arithmetic = instruction.op == LirOpcode::Add || instruction.op == LirOpcode::Sub ||
                                        instruction.op == LirOpcode::Mul ||
                                        (floating && instruction.op == LirOpcode::Div) ||
                                        (word && (instruction.op == LirOpcode::And || instruction.op == LirOpcode::Or ||
                                                  instruction.op == LirOpcode::Xor));
                if ((floating || word) && arithmetic && !instruction.isVolatile && instruction.srcs.size() == 2 &&
                    spans.contains(instruction.dst) && spans.contains(instruction.srcs[0]) &&
                    spans.at(instruction.dst)->start == position && spans.at(instruction.srcs[0])->end == position &&
                    spans.at(instruction.srcs[0])->type == instruction.type) {
                    tiedInputs.emplace(instruction.dst, instruction.srcs[0]);
                }
                ++position;
            }
            ++position;
        }
    }

    [[nodiscard]] CallingConvention EffectiveConvention() const {
        if (func.callConv == CallingConvention::Default) {
            return PlatformDefaultConvention(targetOs, Target::Arch::X86_64);
        }
        return ResolveCConvention(func.callConv, targetOs, Target::Arch::X86_64);
    }

    [[nodiscard]] int RuntimeSize(const TypeRef &type) const {
        return RuntimeSizeOf(type, layouts, interfaceNames);
    }

    [[nodiscard]] bool IsAggregate(const TypeRef &type) const {
        if (IsWideValue(type)) {
            return true;
        }
        if (type.IsRange()) {
            return true;
        }
        // A slice, text included, is a 16-byte {data, length} view whose shape the runtime fixes rather than a
        // declaration.
        if (type.IsSlice()) {
            return true;
        }
        switch (type.kind) {
        case TypeRef::Kind::Tuple:
        case TypeRef::Kind::Array:
        // A native sum, optional, or fallible is a tag followed by its widest payload.
        case TypeRef::Kind::Sum:
        case TypeRef::Kind::Optional:
        case TypeRef::Kind::Fallible:
            return true;
        case TypeRef::Kind::Named: {
            const std::string base = BaseTypeName(type.name);
            return interfaceNames.contains(base) || layouts.contains(base) || (!type.inner.empty() && SizeOf(type) > 8);
        }
        default:
            return false;
        }
    }

    [[nodiscard]] bool IsWin64ByRefAggregate(const TypeRef &type) const {
        if (!IsAggregate(type)) {
            return false;
        }
        const int size = RuntimeSize(type);
        return size > 0 && size != 1 && size != 2 && size != 4 && size != 8;
    }

    // The frame slot a value occupies, which is not always what the value measures. An aggregate is moved a whole
    // word at a time — spilled from the stack argument slots a caller filled, reloaded into the outgoing area of
    // the next call — so its slot is padded to a whole number of words and those moves stay inside it. Without the
    // padding a twenty-byte struct parameter spilled twenty-four bytes over a twenty-byte slot and overwrote the
    // saved frame pointer sitting just above it. A width no scalar move spells is padded for the same reason.
    [[nodiscard]] int SlotSize(const TypeRef &type) const {
        const int size = RuntimeSize(type);
        if (size <= 0) {
            return 8;
        }
        if (IsAggregate(type)) {
            return AlignUp(size, 8);
        }
        return size == 3 || size == 5 || size == 6 || size == 7 ? 8 : size;
    }

    [[nodiscard]] bool IsSysVMemoryAggregate(const TypeRef &type) const {
        return IsAggregate(type) && RuntimeSize(type) > 16;
    }

    [[nodiscard]] bool IsWin64AddressParameter(const TypeRef &type) const {
        if (type.IsSlice()) {
            return true;
        }
        if (type.kind != TypeRef::Kind::Named) {
            return false;
        }
        const std::string base = BaseTypeName(type.name);
        return interfaceNames.contains(base);
    }

    [[nodiscard]] TypeRef ParameterRegisterType(const TypeRef &type) const {
        return IsWin64AddressParameter(type) ? TypeRef::MakePointer(type) : type;
    }

    // The alignment a value of `type` needs in its frame slot. Frame offsets count down from a sixteen-byte aligned
    // frame pointer, so an offset that is a multiple of this places the value where its `alignof` says it is.
    [[nodiscard]] int StorageAlign(const TypeRef &type) const {
        return std::clamp(RuntimeAlignmentOf(type, layouts, interfaceNames), 1, 16);
    }

    std::int32_t AllocateRegion(const int byteCount, const int storageAlignment = 1) {
        const int alignment = std::max(byteCount > 0 ? std::min(byteCount, 8) : 1, storageAlignment);
        nextOffset = AlignUp(nextOffset, alignment);
        nextOffset += byteCount > 0 ? byteCount : 8;
        return nextOffset;
    }

    std::int32_t AllocateSlot(const LirReg reg, const int byteCount, const int storageAlignment = 1) {
        if (const auto found = plan.slotOffsets.find(reg); found != plan.slotOffsets.end()) {
            return found->second;
        }
        const std::int32_t offset = AllocateRegion(byteCount, storageAlignment);
        plan.slotOffsets[reg] = offset;
        return offset;
    }

    void PlanCalleeSaves(const std::vector<LiveInterval> &intervals) {
        std::vector<LiveInterval> candidates;
        for (const auto &interval : intervals) {
            if (interval.type.IsFloat() || IsAggregate(interval.type) || RuntimeSize(interval.type) > 8) {
                continue;
            }
            candidates.push_back(interval);
        }

        bool extendedWin64 = EffectiveConvention() == CallingConvention::Win64;
        for (const auto &block : func.blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.op == LirOpcode::ThreadLocalAddr || instruction.op == LirOpcode::WriteStandardError)
                    extendedWin64 = false;
                if (instruction.op == LirOpcode::Call || instruction.op == LirOpcode::CallIndirect) {
                    const auto convention =
                        instruction.callConv == CallingConvention::Default
                            ? PlatformDefaultConvention(targetOs, Target::Arch::X86_64)
                            : ResolveCConvention(instruction.callConv, targetOs, Target::Arch::X86_64);
                    extendedWin64 = extendedWin64 && convention == CallingConvention::Win64;
                }
            }
        }
        RegisterAssignment assignment = AllocateRegisters(candidates, extendedWin64 ? 7 : 5, tiedInputs);
        plan.physicalRegisters = std::move(assignment.physRegs);
        plan.usedPhysicalRegisters = std::move(assignment.usedPhysRegs);
    }

    void PlanFloatRegisters(const std::vector<LiveInterval> &intervals) {
        // Wide/software arithmetic still has memory-only operand paths. Keep this first FP allocation phase out of
        // such functions until those paths describe all their operand constraints.
        if (std::ranges::any_of(intervals, [](const LiveInterval &interval) {
                return IsSoftwareFloat(interval.type) || IsWideScalar(interval.type);
            })) {
            return;
        }
        std::vector<int> clobbers;
        int position = 0;
        for (const auto &block : func.blocks) {
            for (const auto &instruction : block.instrs) {
                const auto convention = instruction.callConv == CallingConvention::Default
                                          ? PlatformDefaultConvention(targetOs, Target::Arch::X86_64)
                                          : ResolveCConvention(instruction.callConv, targetOs, Target::Arch::X86_64);
                if (((instruction.op == LirOpcode::Call || instruction.op == LirOpcode::CallIndirect) &&
                     convention != CallingConvention::Win64) ||
                    instruction.op == LirOpcode::ThreadLocalAddr || instruction.op == LirOpcode::WriteStandardError) {
                    clobbers.push_back(position);
                }
                ++position;
            }
            ++position;
        }
        std::vector<LiveInterval> candidates;
        for (const auto &interval : intervals) {
            if (!IsFloat(interval.type) || std::ranges::any_of(clobbers, [&](const int clobber) {
                    return interval.start <= clobber && interval.end >= clobber;
                })) {
                continue;
            }
            candidates.push_back(interval);
        }
        const auto assignment = AllocateRegisters(candidates, 8, tiedInputs);
        for (const auto &[reg, index] : assignment.physRegs) {
            plan.floatRegisters.emplace(reg, index + 8);
        }
        if (EffectiveConvention() == CallingConvention::Win64) {
            for (const auto index : assignment.usedPhysRegs) {
                plan.floatSaves.emplace_back(index + 8, AllocateRegion(16, 16));
            }
        }
    }

    void PlanHiddenReturn() {
        const CallingConvention convention = EffectiveConvention();
        if ((convention == CallingConvention::Win64 && IsWin64ByRefAggregate(func.returnType)) ||
            (convention == CallingConvention::SysV && IsSysVMemoryAggregate(func.returnType))) {
            plan.hiddenReturnOffset = AllocateRegion(8);
        }
    }

    void PlanBranches() {
        std::unordered_map<LirReg, std::size_t> uses;
        for (const auto &block : func.blocks) {
            for (const auto &instruction : block.instrs) {
                for (const auto source : instruction.srcs)
                    ++uses[source];
                for (const auto &[source, predecessor] : instruction.phiPreds) {
                    static_cast<void>(predecessor);
                    ++uses[source];
                }
            }
            if (block.term) {
                ++uses[block.term->cond];
                if (block.term->retVal)
                    ++uses[*block.term->retVal];
            }
        }
        for (const auto &block : func.blocks) {
            if (!block.term || block.term->kind != LirTermKind::Branch || block.instrs.empty())
                continue;
            const auto &comparison = block.instrs.back();
            if (comparison.dst != block.term->cond || comparison.srcs.size() != 2 || uses[comparison.dst] != 1 ||
                comparison.type != TypeRef::MakeBool() || comparison.isVolatile)
                continue;
            const auto left = plan.registerTypes.find(comparison.srcs[0]);
            const auto right = plan.registerTypes.find(comparison.srcs[1]);
            if (left == plan.registerTypes.end() || right == plan.registerTypes.end() ||
                left->second != right->second || !left->second.IsInteger() || RuntimeSize(left->second) > 8)
                continue;
            if (const auto condition = IntegerComparisonCondition(comparison.op, left->second.IsSigned())) {
                plan.branchComparisons.emplace(
                    comparison.dst,
                    X86BranchComparison{comparison.srcs[0], comparison.srcs[1], left->second, *condition});
            }
        }
    }

    void PlanParameters() {
        for (const auto &parameter : func.params) {
            TypeRef registerType = ParameterRegisterType(parameter.type);
            plan.registerTypes[parameter.reg] = registerType;
            AllocateSlot(parameter.reg, std::max(8, SlotSize(registerType)), StorageAlign(registerType));
        }
    }

    [[nodiscard]] int AllocaSize(const LirInstr &instruction) const {
        if (instruction.strArg.empty()) {
            return RuntimeSize(instruction.type);
        }

        int count = 0;
        const std::string_view text = instruction.strArg;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), count);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            count = 0;
        }
        const TypeRef &elementType = instruction.type.inner.empty() ? instruction.type : instruction.type.inner[0];
        const int elementSize = RuntimeSize(elementType);
        return count * (elementSize > 0 ? elementSize : 8);
    }

    void PlanInstructions() {
        // Results that are never live together share a slot; its region is placed where its first member is defined.
        const FrameSlotSharing sharing =
            ShareFrameSlots(func, [&](const LirInstr &instruction) { return SlotSize(instruction.type); });
        std::vector<std::int32_t> sharedOffsets(sharing.slotSizes.size(), -1);
        // The same for the data of allocas only this function can reach.
        const FrameSlotSharing dataSharing = ShareAllocaData(func, [&](const LirInstr &instruction) {
            const int size = AllocaSize(instruction);
            return size > 0 ? size : 8;
        });
        std::vector<std::int32_t> sharedDataOffsets(dataSharing.slotSizes.size(), -1);
        // A shared region is aligned for the strictest of the values that take turns in it.
        std::vector<int> sharedAlignments(sharing.slotSizes.size(), 1);
        std::vector<int> sharedDataAlignments(dataSharing.slotSizes.size(), 1);
        for (const auto &block : func.blocks) {
            for (const auto &instruction : block.instrs) {
                if (const auto shared = sharing.slotOf.find(instruction.dst); shared != sharing.slotOf.end()) {
                    sharedAlignments[shared->second] =
                        std::max(sharedAlignments[shared->second], StorageAlign(instruction.type));
                }
                if (const auto shared = dataSharing.slotOf.find(instruction.dst); shared != dataSharing.slotOf.end()) {
                    sharedDataAlignments[shared->second] =
                        std::max(sharedDataAlignments[shared->second], StorageAlign(instruction.type));
                }
            }
        }
        for (std::uint32_t blockIndex = 0; blockIndex < func.blocks.size(); ++blockIndex) {
            for (const auto &instruction : func.blocks[blockIndex].instrs) {
                if (instruction.op == LirOpcode::Phi) {
                    for (const auto &[source, predecessor] : instruction.phiPreds) {
                        plan.phiMoves[predecessor][blockIndex].push_back({instruction.dst, source, instruction.type});
                    }
                }
                if (instruction.dst == LirNoReg) {
                    continue;
                }

                if (instruction.op == LirOpcode::Alloca) {
                    AllocateSlot(instruction.dst, 8);
                    plan.registerTypes[instruction.dst] = TypeRef::MakePointer(instruction.type);
                    if (const auto shared = dataSharing.slotOf.find(instruction.dst);
                        shared != dataSharing.slotOf.end()) {
                        std::int32_t &offset = sharedDataOffsets[shared->second];
                        if (offset < 0) {
                            offset = AllocateRegion(dataSharing.slotSizes[shared->second],
                                                    sharedDataAlignments[shared->second]);
                        }
                        plan.allocaDataOffsets[instruction.dst] = offset;
                        continue;
                    }
                    const int dataSize = AllocaSize(instruction);
                    plan.allocaDataOffsets[instruction.dst] =
                        AllocateRegion(dataSize > 0 ? dataSize : 8, StorageAlign(instruction.type));
                    continue;
                }

                plan.registerTypes[instruction.dst] = instruction.type;
                if (const auto shared = sharing.slotOf.find(instruction.dst); shared != sharing.slotOf.end()) {
                    std::int32_t &offset = sharedOffsets[shared->second];
                    if (offset < 0) {
                        offset = AllocateRegion(sharing.slotSizes[shared->second], sharedAlignments[shared->second]);
                    }
                    plan.slotOffsets.try_emplace(instruction.dst, offset);
                    continue;
                }
                AllocateSlot(instruction.dst, SlotSize(instruction.type), StorageAlign(instruction.type));
            }
        }
    }

    void PlanWideTemporaries() {
        int largest = 0;
        for (const LirBlock &block : func.blocks) {
            for (const LirInstr &instruction : block.instrs) {
                largest = std::max(largest, WideTemporarySize(instruction, plan.registerTypes));
                // A float32 or float64 remainder runs on the x87 unit (`X87Emitter.cpp`), which loads and stores only
                // memory: it gets the scratch slots an x87 instruction moves a register-held value through.
                if (instruction.op == LirOpcode::Mod && IsFloat(instruction.type)) {
                    largest = std::max(largest, 8);
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
        for (const auto &[fromBlock, destinations] : plan.phiMoves) {
            (void)fromBlock;
            for (const auto &[toBlock, moves] : destinations) {
                (void)toBlock;
                for (const auto &step : ResolvePhiMoves(moves)) {
                    if (step.kind != PhiMoveStep::Kind::SaveDestination) {
                        continue;
                    }
                    // Reserve what the value actually occupies. Clamping to sixteen would under-reserve any wider
                    // aggregate carried through a phi, which is the same overrun that undersized array allocas
                    // produced. AArch64 already sizes this the same way.
                    plan.phiTemporarySize =
                        std::max(plan.phiTemporarySize, std::max(8, AlignUp(RuntimeSize(step.type), 8)));
                }
            }
        }
        if (plan.phiTemporarySize > 0) {
            plan.phiTemporaryOffset = AllocateRegion(plan.phiTemporarySize);
        }
    }
};

X86_64FramePlan PlanX86_64Frame(const LirFunc &func, const LayoutMap &layouts,
                                const std::unordered_set<std::string> &interfaceNames, const Target::OS targetOs) {
    return X86_64FramePlanner(func, layouts, interfaceNames, targetOs).Build();
}
} // namespace Rux

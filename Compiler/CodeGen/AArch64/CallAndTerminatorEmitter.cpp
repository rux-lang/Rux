#include "CodeGen/AArch64/CallAndTerminatorEmitter.h"

#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "CodeGen/PhiMoveResolver.h"

#include <format>
#include <utility>

namespace Rux {
using namespace Layout;

namespace {
constexpr unsigned kTemp = 9;
constexpr unsigned kSrcAddress = 11;
constexpr unsigned kIntegerArgumentRegisters = 8;
constexpr unsigned kIndirectResult = 8;
constexpr unsigned kFpTemp = 16;
constexpr std::uint64_t kNoBranchSite = ~0ULL;

[[nodiscard]] unsigned AccessWidth(const int size) {
    if (size <= 1) {
        return size <= 0 ? 8U : 1U;
    }
    if (size <= 2) {
        return 2;
    }
    return size <= 4 ? 4U : 8U;
}

[[nodiscard]] A64Reg FloatRegister(const TypeRef &type, const unsigned index) {
    return type.kind == TypeRef::Kind::Float32 ? A64::Sn(index) : A64::Dn(index);
}
} // namespace

AArch64CallEmitter::AArch64CallEmitter(A64Enc &encoder, const AArch64FramePlan &framePlan,
                                       AArch64CallPlanner &callPlanner, std::string functionName,
                                       AArch64CallAndTerminatorHooks &hooks)
    : encoder(encoder)
    , framePlan(framePlan)
    , callPlanner(callPlanner)
    , functionName(std::move(functionName))
    , hooks(hooks) {
}

TypeRef AArch64CallEmitter::TypeOfReg(const LirReg reg) const {
    const auto &registerTypes = framePlan.RegisterTypes();
    const auto it = registerTypes.find(reg);
    return it == registerTypes.end() ? TypeRef::MakeInt64() : it->second;
}

void AArch64CallEmitter::EmitWindowsVariadicCallArgs(const std::vector<LirReg> &arguments,
                                                     const std::vector<TypeRef> &types, const CallLayout &layout) {
    const auto loadSlot = [&](const std::size_t index, const std::int32_t sourceOffset, const A64Reg destination) {
        const ArgLocation &location = layout.args[index];
        if (location.byReference) {
            Must(encoder.AddSubLargeImm(destination, A64::Sp, location.copyOffset), "the address of an argument copy");
            return;
        }
        if (hooks.IsAggregate(types[index])) {
            hooks.LoadScalar(destination, A64::Fp,
                             static_cast<std::int64_t>(hooks.Disp(arguments[index])) + sourceOffset, 8, false);
            return;
        }
        if (IsFloat(types[index])) {
            const A64Reg value = FloatRegister(types[index], kFpTemp);
            hooks.LoadFpFromSlot(value, arguments[index]);
            Must(encoder.Fmov(A64::Gpr(destination.code, value.bits), value), "a variadic floating-point argument");
            return;
        }
        hooks.LoadFromSlot(destination, arguments[index], types[index]);
    };

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const ArgLocation &location = layout.args[i];
        if (!location.byReference) {
            continue;
        }
        const A64Reg source = A64::Xn(kSrcAddress);
        hooks.SlotAddress(source, arguments[i]);
        hooks.CopyBlock(A64::Sp, location.copyOffset, source, 0, location.copyBytes, hooks.RuntimeAlign(types[i]) >= 8);
    }

    constexpr std::int32_t registerBytes = static_cast<std::int32_t>(kIntegerArgumentRegisters * 8);
    for (const bool toRegisters : {false, true}) {
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const ArgLocation &location = layout.args[i];
            for (std::int32_t byte = 0; byte < location.bytes; byte += 8) {
                const std::int32_t imaginaryOffset = location.offset + byte;
                if ((imaginaryOffset < registerBytes) != toRegisters) {
                    continue;
                }
                if (toRegisters) {
                    loadSlot(i, byte, A64::Xn(static_cast<unsigned>(imaginaryOffset / 8)));
                }
                else {
                    const A64Reg value = A64::Xn(kTemp);
                    loadSlot(i, byte, value);
                    hooks.StoreScalar(value, A64::Sp, imaginaryOffset - registerBytes, 8);
                }
            }
        }
    }
}

void AArch64CallEmitter::MoveRegisterArgument(const ArgLocation &location, const LirReg reg, const TypeRef &type,
                                              const bool toRegisters) {
    if (location.kind == ArgLocation::Kind::Vector && location.count == 1 && IsFloat(type)) {
        const A64Reg value = A64::Vn(location.first, location.memberBytes * 8U);
        if (toRegisters) {
            hooks.LoadFpFromSlot(value, reg);
        }
        else {
            hooks.StoreFpToSlot(value, reg);
        }
        return;
    }

    const auto displacement = static_cast<std::int64_t>(hooks.Disp(reg));
    if (location.kind == ArgLocation::Kind::Vector) {
        for (unsigned i = 0; i < location.count; ++i) {
            const A64Reg member = A64::Vn(location.first + i, location.memberBytes * 8U);
            const std::int64_t offset = displacement + static_cast<std::int64_t>(i) * location.memberBytes;
            if (toRegisters) {
                hooks.LoadScalar(member, A64::Fp, offset, location.memberBytes, false);
            }
            else {
                hooks.StoreScalar(member, A64::Fp, offset, location.memberBytes);
            }
        }
        return;
    }
    if (location.count > 1) {
        for (unsigned i = 0; i < location.count; ++i) {
            const A64Reg word = A64::Xn(location.first + i);
            const std::int64_t offset = displacement + static_cast<std::int64_t>(i) * 8;
            if (toRegisters) {
                hooks.LoadScalar(word, A64::Fp, offset, 8, false);
            }
            else {
                hooks.StoreScalar(word, A64::Fp, offset, 8);
            }
        }
        return;
    }
    if (toRegisters) {
        const bool appleNarrowInteger = callPlanner.Policy().callerExtendsNarrowIntegers && !hooks.IsAggregate(type) &&
                                        !IsFloat(type) && hooks.RuntimeSize(type) < 4;
        if (appleNarrowInteger) {
            hooks.LoadWidthFromSlot(A64::Xn(location.first), reg, AccessWidth(hooks.RuntimeSize(type)),
                                    type.IsSigned());
            return;
        }
        hooks.LoadFromSlot(A64::Xn(location.first), reg, type);
    }
    else {
        hooks.StoreToSlot(A64::Xn(location.first), reg, type);
    }
}

void AArch64CallEmitter::LoadResultFromAddress(const ArgLocation &location, const A64Reg base, const TypeRef &type) {
    if (location.kind == ArgLocation::Kind::Vector) {
        for (unsigned i = 0; i < location.count; ++i) {
            const A64Reg member = A64::Vn(location.first + i, location.memberBytes * 8U);
            hooks.LoadScalar(member, base, static_cast<std::int64_t>(i) * location.memberBytes, location.memberBytes,
                             false);
        }
        return;
    }
    if (location.count > 1) {
        for (unsigned i = 0; i < location.count; ++i) {
            hooks.LoadScalar(A64::Xn(location.first + i), base, static_cast<std::int64_t>(i) * 8, 8, false);
        }
        return;
    }
    const bool aggregate = hooks.IsAggregate(type);
    hooks.LoadScalar(A64::Xn(location.first), base, 0, aggregate ? 8U : AccessWidth(hooks.RuntimeSize(type)),
                     !aggregate && type.IsSigned());
}

void AArch64CallEmitter::EmitCallArgs(const std::vector<LirReg> &arguments, const std::vector<TypeRef> &types,
                                      const CallLayout &layout) {
    if (layout.windowsVariadic) {
        EmitWindowsVariadicCallArgs(arguments, types, layout);
        return;
    }
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const ArgLocation &location = layout.args[i];
        const bool paired = hooks.RuntimeAlign(types[i]) >= 8;
        if (location.byReference) {
            const A64Reg source = A64::Xn(kSrcAddress);
            hooks.SlotAddress(source, arguments[i]);
            hooks.CopyBlock(A64::Sp, location.copyOffset, source, 0, location.copyBytes, paired);
            if (location.kind == ArgLocation::Kind::Stack) {
                const A64Reg address = A64::Xn(kTemp);
                Must(encoder.AddSubLargeImm(address, A64::Sp, location.copyOffset), "the address of an argument copy");
                hooks.StoreScalar(address, A64::Sp, location.offset, 8);
            }
            continue;
        }
        if (location.kind != ArgLocation::Kind::Stack) {
            continue;
        }
        const int size = hooks.RuntimeSize(types[i]);
        if (hooks.IsAggregate(types[i]) && (size > 8 || callPlanner.Policy().compactStackArguments)) {
            const A64Reg source = A64::Xn(kSrcAddress);
            hooks.SlotAddress(source, arguments[i]);
            hooks.CopyBlock(A64::Sp, location.offset, source, 0, size, paired);
            continue;
        }
        if (IsFloat(types[i])) {
            const A64Reg value = FloatRegister(types[i], kFpTemp);
            hooks.LoadFpFromSlot(value, arguments[i]);
            hooks.StoreScalar(value, A64::Sp, location.offset, value.bits / 8U);
            continue;
        }
        const A64Reg value = A64::Xn(kTemp);
        hooks.LoadFromSlot(value, arguments[i], types[i]);
        const unsigned width = callPlanner.Policy().compactStackArguments ? AccessWidth(size) : 8U;
        hooks.StoreScalar(value, A64::Sp, location.offset, width);
    }
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const ArgLocation &location = layout.args[i];
        if (location.kind == ArgLocation::Kind::Stack) {
            continue;
        }
        if (location.byReference) {
            Must(encoder.AddSubLargeImm(A64::Xn(location.first), A64::Sp, location.copyOffset),
                 "the address of an argument copy");
            continue;
        }
        MoveRegisterArgument(location, arguments[i], types[i], true);
    }
}

void AArch64CallEmitter::EmitCall(const LirInstr &instruction, const std::vector<LirReg> &arguments,
                                  const bool indirect) {
    if (instruction.isCVariadic != instruction.cVariadicFixedParamCount.has_value()) {
        Report(
            std::format("AArch64 code generation reached inconsistent C variadic call metadata in '{}'", functionName));
        return;
    }
    if (instruction.cVariadicFixedParamCount &&
        (indirect || *instruction.cVariadicFixedParamCount > static_cast<std::uint32_t>(arguments.size()))) {
        Report(std::format("AArch64 code generation reached an invalid C variadic fixed-parameter count in '{}'",
                           functionName));
        return;
    }

    std::vector<TypeRef> types;
    types.reserve(arguments.size());
    for (const LirReg argument : arguments) {
        types.push_back(TypeOfReg(argument));
    }
    const CallLayout layout = callPlanner.PlanArguments(types, instruction.cVariadicFixedParamCount);
    const bool keepsResult = instruction.dst != LirNoReg && !instruction.type.IsOpaque();
    const bool indirectResult = keepsResult && callPlanner.ReturnsInMemory(instruction.type);

    if (layout.areaBytes > 0) {
        hooks.OpenStackArea(layout.areaBytes, "the outgoing argument area");
    }
    EmitCallArgs(arguments, types, layout);
    if (indirectResult) {
        hooks.SlotAddress(A64::Xn(kIndirectResult), instruction.dst);
    }
    if (indirect) {
        const A64Reg target = A64::Xn(kTemp);
        hooks.LoadPointer(target, instruction.srcs[0]);
        Must(encoder.Blr(target), "an indirect call");
    }
    else {
        const std::uint32_t callSite = encoder.Size();
        Must(encoder.Bl(0), "a call");
        hooks.AddCallRelocation(callSite, hooks.ResolveCallSymbol(instruction.strArg));
    }
    if (layout.areaBytes > 0) {
        Must(encoder.FrameAdjust(layout.areaBytes), "the outgoing argument area");
    }
    if (keepsResult && !indirectResult) {
        MoveRegisterArgument(callPlanner.PlanResult(instruction.type), instruction.dst, instruction.type, false);
    }
}

bool AArch64CallEmitter::Emit(const LirInstr &instruction) {
    if (instruction.op == LirOpcode::Call) {
        EmitCall(instruction, instruction.srcs, false);
        return true;
    }
    if (instruction.op != LirOpcode::CallIndirect) {
        return false;
    }
    if (instruction.srcs.empty()) {
        Report(std::format("AArch64 code generation reached an indirect call with no callee in '{}'", functionName));
        return true;
    }
    EmitCall(instruction, {instruction.srcs.begin() + 1, instruction.srcs.end()}, true);
    return true;
}

void AArch64CallEmitter::EmitParamSpills(const LirFunc &function) {
    std::vector<TypeRef> types;
    types.reserve(function.params.size());
    for (const auto &parameter : function.params) {
        types.push_back(parameter.type);
    }
    const CallLayout layout = callPlanner.PlanArguments(types);

    for (std::size_t i = 0; i < function.params.size(); ++i) {
        const LirParam &parameter = function.params[i];
        const ArgLocation &location = layout.args[i];
        const int size = hooks.RuntimeSize(parameter.type);
        const bool paired = hooks.RuntimeAlign(parameter.type) >= 8;
        const std::int64_t incoming = static_cast<std::int64_t>(framePlan.FrameSize()) + location.offset;
        if (location.byReference) {
            const A64Reg source = A64::Xn(kSrcAddress);
            if (location.kind == ArgLocation::Kind::Stack) {
                hooks.LoadScalar(source, A64::Fp, incoming, 8, false);
            }
            else {
                Must(encoder.Mov(source, A64::Xn(location.first)), "the address of an argument");
            }
            hooks.CopyBlock(A64::Fp, hooks.Disp(parameter.reg), source, 0, size, paired);
            continue;
        }
        if (location.kind != ArgLocation::Kind::Stack) {
            MoveRegisterArgument(location, parameter.reg, parameter.type, false);
            continue;
        }
        if (hooks.IsAggregate(parameter.type) && size > 8) {
            hooks.CopyBlock(A64::Fp, hooks.Disp(parameter.reg), A64::Fp, incoming, size, paired);
            continue;
        }
        if (IsFloat(parameter.type)) {
            const A64Reg value = FloatRegister(parameter.type, kFpTemp);
            hooks.LoadScalar(value, A64::Fp, incoming, value.bits / 8U, false);
            hooks.StoreFpToSlot(value, parameter.reg);
            continue;
        }
        const A64Reg value = A64::Xn(kTemp);
        hooks.LoadScalar(value, A64::Fp, incoming, AccessWidth(size), parameter.type.IsSigned());
        hooks.StoreToSlot(value, parameter.reg, parameter.type);
    }
}

void AArch64CallEmitter::EmitReturnValue(const LirReg reg, const TypeRef &type) {
    const TypeRef regType = TypeOfReg(reg);
    const bool throughPointer =
        regType.kind == TypeRef::Kind::Pointer && !regType.inner.empty() && regType.inner[0] == type;
    if (framePlan.IndirectResultOffset() != 0) {
        const A64Reg destination = A64::Xn(10);
        hooks.LoadScalar(destination, A64::Fp, framePlan.IndirectResultOffset(), 8, false);
        A64Reg source = A64::Xn(kSrcAddress);
        if (throughPointer) {
            source = hooks.ReadPointerOperand(reg, source);
        }
        else {
            hooks.SlotAddress(source, reg);
        }
        hooks.CopyBlock(destination, 0, source, 0, hooks.RuntimeSize(type), hooks.RuntimeAlign(type) >= 8);
    }
    else if (throughPointer) {
        LoadResultFromAddress(callPlanner.PlanResult(type), hooks.ReadPointerOperand(reg, A64::Xn(kSrcAddress)), type);
    }
    else {
        MoveRegisterArgument(callPlanner.PlanResult(type), reg, type, true);
    }
}

void AArch64CallEmitter::Report(std::string message) {
    hooks.ReportCallAndTerminatorDiagnostic(std::move(message));
}

void AArch64CallEmitter::Must(const A64Status status, const std::string_view what) {
    if (status != A64Status::Ok) {
        Report(std::format("AArch64 code generation could not encode {} in '{}': {}", what, functionName,
                           A64StatusName(status)));
    }
}

AArch64TerminatorEmitter::AArch64TerminatorEmitter(A64Enc &encoder, const AArch64FramePlan &framePlan,
                                                   AArch64CallEmitter &callEmitter, std::string functionName,
                                                   AArch64CallAndTerminatorHooks &hooks)
    : encoder(encoder)
    , framePlan(framePlan)
    , callEmitter(callEmitter)
    , functionName(std::move(functionName))
    , hooks(hooks) {
}

AArch64TerminatorEmitter::ConditionalBranch AArch64TerminatorEmitter::ConditionalBranch::Inverted() const {
    switch (form) {
    case Form::Zero:
        return {Form::NotZero, condition, reg};
    case Form::NotZero:
        return {Form::Zero, condition, reg};
    default:
        return {Form::Condition, A64::InvertCondition(condition), reg};
    }
}

std::uint64_t AArch64TerminatorEmitter::BranchSite(const std::uint32_t block, const unsigned ordinal) {
    return static_cast<std::uint64_t>(block) << 32U | ordinal;
}

AArch64TerminatorEmitter::ConditionalBranch AArch64TerminatorEmitter::OnCondition(const A64Condition condition) {
    return {ConditionalBranch::Form::Condition, condition, {}};
}

AArch64TerminatorEmitter::ConditionalBranch AArch64TerminatorEmitter::OnZero(const A64Reg reg) {
    return {ConditionalBranch::Form::Zero, A64Condition::Eq, reg};
}

void AArch64TerminatorEmitter::BeginFunction() {
    widenedSites.clear();
}

void AArch64TerminatorEmitter::BeginPass(const std::size_t blockCount) {
    jumpPatches.clear();
    blockOffsets.assign(blockCount, 0);
}

void AArch64TerminatorEmitter::MarkBlock(const std::uint32_t blockIndex) {
    if (blockIndex < blockOffsets.size()) {
        blockOffsets[blockIndex] = encoder.Size();
    }
}

bool AArch64TerminatorEmitter::PatchJumps() {
    bool ok = true;
    for (const auto &patch : jumpPatches) {
        if (patch.targetBlock >= blockOffsets.size()) {
            continue;
        }
        const auto target = static_cast<std::int32_t>(blockOffsets[patch.targetBlock]);
        const std::int32_t instructions =
            (target - static_cast<std::int32_t>(patch.patchOffset)) / static_cast<std::int32_t>(A64Enc::InstrSize);
        const std::int32_t limit = 1 << (patch.width - 1);
        if (instructions < -limit || instructions >= limit) {
            ok = false;
            if (patch.site == kNoBranchSite) {
                Report(std::format("AArch64 code generation reached a branch too far to encode in '{}'", functionName));
            }
            else {
                widenedSites.insert(patch.site);
            }
            continue;
        }
        encoder.PatchField(patch.patchOffset, patch.lsb, patch.width, static_cast<std::uint32_t>(instructions));
    }
    return ok;
}

std::size_t AArch64TerminatorEmitter::WidenedSiteCount() const {
    return widenedSites.size();
}

void AArch64TerminatorEmitter::EmitJumpTo(const std::uint32_t targetBlock) {
    const std::uint32_t patchOffset = encoder.Size();
    Must(encoder.B(0), "a branch");
    jumpPatches.push_back({patchOffset, targetBlock, 0, 26, kNoBranchSite});
}

void AArch64TerminatorEmitter::EmitConditionalBranch(const ConditionalBranch &branch, const std::int64_t offset) {
    switch (branch.form) {
    case ConditionalBranch::Form::Zero:
        Must(encoder.Cbz(branch.reg, offset), "a branch on zero");
        break;
    case ConditionalBranch::Form::NotZero:
        Must(encoder.Cbnz(branch.reg, offset), "a branch on a nonzero value");
        break;
    default:
        Must(encoder.BCond(branch.condition, offset), "a conditional branch");
        break;
    }
}

void AArch64TerminatorEmitter::EmitConditionalBranchTo(const ConditionalBranch &branch, const std::uint32_t targetBlock,
                                                       const std::uint64_t site) {
    if (widenedSites.contains(site)) {
        EmitConditionalBranch(branch.Inverted(), 2 * A64Enc::InstrSize);
        EmitJumpTo(targetBlock);
        return;
    }
    const std::uint32_t patchOffset = encoder.Size();
    EmitConditionalBranch(branch, 0);
    jumpPatches.push_back({patchOffset, targetBlock, 5, 19, site});
}

std::uint32_t AArch64TerminatorEmitter::EmitBranchOver(const ConditionalBranch &branch) {
    const std::uint32_t patchOffset = encoder.Size();
    EmitConditionalBranch(branch, 0);
    return patchOffset;
}

std::uint32_t AArch64TerminatorEmitter::EmitBranchOverNonZero(const A64Reg reg) {
    return EmitBranchOver({ConditionalBranch::Form::NotZero, A64Condition::Eq, reg});
}

void AArch64TerminatorEmitter::PatchBranchOver(const std::uint32_t patchOffset) {
    const std::uint32_t instructions = (encoder.Size() - patchOffset) / A64Enc::InstrSize;
    if (instructions >= 1U << 18U) {
        Report(std::format("AArch64 code generation reached a branch too far to encode in '{}'", functionName));
        return;
    }
    encoder.PatchField(patchOffset, 5, 19, instructions);
}

bool AArch64TerminatorEmitter::HasPhiMoves(const std::uint32_t from, const std::uint32_t to) const {
    const auto edges = framePlan.PhiMoves().find(from);
    return edges != framePlan.PhiMoves().end() && edges->second.contains(to);
}

void AArch64TerminatorEmitter::EmitPhiMoves(const std::uint32_t from, const std::uint32_t to) {
    const auto edges = framePlan.PhiMoves().find(from);
    if (edges == framePlan.PhiMoves().end()) {
        return;
    }
    const auto moves = edges->second.find(to);
    if (moves == edges->second.end()) {
        return;
    }
    for (const auto &step : ResolvePhiMoves(moves->second)) {
        if (step.kind == PhiMoveStep::Kind::SaveDestination) {
            hooks.CopyFrameValue(framePlan.PhiTemporaryOffset(), hooks.Disp(step.dst), step.type);
            continue;
        }
        hooks.CopyFrameValue(hooks.Disp(step.dst),
                             step.sourceIsTemporary ? framePlan.PhiTemporaryOffset() : hooks.Disp(step.src), step.type);
    }
}

void AArch64TerminatorEmitter::CompareAgainstCase(const std::string &value) {
    const std::uint64_t bits = ParseIntegerLiteralBits(value).value_or(0);
    if (encoder.SubsImm(A64::Xzr, A64::Xn(kTemp), bits) == A64Status::Ok) {
        return;
    }
    const A64Reg label = A64::Xn(12);
    Must(encoder.LoadImm64(label, bits), "a case label");
    Must(encoder.Cmp(A64::Xn(kTemp), label), "a case comparison");
}

void AArch64TerminatorEmitter::Emit(const std::uint32_t blockIndex, const LirTerminator &terminator) {
    switch (terminator.kind) {
    case LirTermKind::Jump:
        EmitPhiMoves(blockIndex, terminator.trueTarget);
        EmitJumpTo(terminator.trueTarget);
        break;
    case LirTermKind::Branch: {
        const auto &registerTypes = framePlan.RegisterTypes();
        const auto found = registerTypes.find(terminator.cond);
        hooks.LoadFromSlot(A64::Xn(kTemp), terminator.cond,
                           found != registerTypes.end() ? found->second : TypeRef::MakeBool());
        const A64Reg condition = A64::Xn(kTemp);
        if (!HasPhiMoves(blockIndex, terminator.trueTarget) && !HasPhiMoves(blockIndex, terminator.falseTarget)) {
            EmitConditionalBranchTo(OnZero(condition), terminator.falseTarget, BranchSite(blockIndex, 0));
            EmitJumpTo(terminator.trueTarget);
            break;
        }
        const std::uint32_t toFalse = EmitBranchOver(OnZero(condition));
        EmitPhiMoves(blockIndex, terminator.trueTarget);
        EmitJumpTo(terminator.trueTarget);
        PatchBranchOver(toFalse);
        EmitPhiMoves(blockIndex, terminator.falseTarget);
        EmitJumpTo(terminator.falseTarget);
        break;
    }
    case LirTermKind::Return:
        if (terminator.retVal && *terminator.retVal != LirNoReg) {
            callEmitter.EmitReturnValue(*terminator.retVal, terminator.retType);
        }
        hooks.EmitEpilogue();
        break;
    case LirTermKind::Switch: {
        const auto &registerTypes = framePlan.RegisterTypes();
        const auto found = registerTypes.find(terminator.cond);
        hooks.LoadFromSlot(A64::Xn(kTemp), terminator.cond,
                           found != registerTypes.end() ? found->second : TypeRef::MakeInt64());
        unsigned ordinal = 0;
        for (const auto &branch : terminator.cases) {
            CompareAgainstCase(branch.value);
            if (!HasPhiMoves(blockIndex, branch.target)) {
                EmitConditionalBranchTo(OnCondition(A64Condition::Eq), branch.target,
                                        BranchSite(blockIndex, ordinal++));
                continue;
            }
            const std::uint32_t toNext = EmitBranchOver(OnCondition(A64Condition::Ne));
            EmitPhiMoves(blockIndex, branch.target);
            EmitJumpTo(branch.target);
            PatchBranchOver(toNext);
        }
        EmitPhiMoves(blockIndex, terminator.defaultTarget);
        EmitJumpTo(terminator.defaultTarget);
        break;
    }
    case LirTermKind::Unreachable:
        Must(encoder.Udf(0), "an unreachable terminator");
        break;
    }
}

void AArch64TerminatorEmitter::Report(std::string message) {
    hooks.ReportCallAndTerminatorDiagnostic(std::move(message));
}

void AArch64TerminatorEmitter::Must(const A64Status status, const std::string_view what) {
    if (status != A64Status::Ok) {
        Report(std::format("AArch64 code generation could not encode {} in '{}': {}", what, functionName,
                           A64StatusName(status)));
    }
}
} // namespace Rux

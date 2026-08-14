#include "CodeGen/X86_64/CallAndTerminatorEmitter.h"

#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "CodeGen/PhiMoveResolver.h"
#include "CodeGen/X86_64/Encoder.h"
#include "Target/CallingConvention.h"

namespace Rux {
using namespace Layout;

X86_64CallEmitter::X86_64CallEmitter(X64Enc &encoder, const X86_64FramePlan &framePlan, const Target::OS targetOs,
                                     X86_64CallAndTerminatorHooks &hooks)
    : encoder(encoder)
    , framePlan(framePlan)
    , targetOs(targetOs)
    , hooks(hooks) {
}

CallingConvention X86_64CallEmitter::EffectiveConvention(const CallingConvention convention) const {
    return convention == CallingConvention::Default ? PlatformDefaultConvention(targetOs, Target::Arch::X86_64)
                                                    : ResolveCConvention(convention, targetOs, Target::Arch::X86_64);
}

std::int32_t X86_64CallEmitter::Disp(const LirReg reg) const {
    return -framePlan.SlotOffsets().at(reg);
}

bool X86_64CallEmitter::IsWin64ByRefAggregate(const TypeRef &type) const {
    if (!hooks.IsAggregate(type)) {
        return false;
    }
    const int size = hooks.SizeOfRuntime(type);
    return size > 0 && size != 1 && size != 2 && size != 4 && size != 8;
}

bool X86_64CallEmitter::IsSysVMemoryAggregate(const TypeRef &type) const {
    return hooks.IsAggregate(type) && hooks.SizeOfRuntime(type) > 16;
}

bool X86_64CallEmitter::IsPointerToWin64ByRefAggregate(const TypeRef &type) const {
    return type.kind == TypeRef::Kind::Pointer && !type.inner.empty() && IsWin64ByRefAggregate(type.inner[0]);
}

int X86_64CallEmitter::CallFrameSize(const std::vector<LirReg> &arguments, const bool win64,
                                     const int startIndex) const {
    if (win64) {
        const std::size_t count = arguments.size() + static_cast<std::size_t>(startIndex);
        return AlignUp(static_cast<int>(32 + (count > 4 ? count - 4 : 0) * 8), 16);
    }

    int integerIndex = startIndex;
    int floatIndex = 0;
    std::size_t stackArguments = 0;
    for (const LirReg argument : arguments) {
        const auto &types = framePlan.RegisterTypes();
        const TypeRef type = types.contains(argument) ? types.at(argument) : TypeRef::MakeInt64();
        if (IsFloat(type)) {
            floatIndex < 8 ? ++floatIndex : ++stackArguments;
        }
        else if (IsSysVMemoryAggregate(type)) {
            stackArguments += static_cast<std::size_t>(AlignUp(hooks.SizeOfRuntime(type), 8) / 8);
        }
        else if (hooks.IsAggregate(type) && hooks.SizeOfRuntime(type) == 16) {
            if (integerIndex <= 4) {
                integerIndex += 2;
            }
            else {
                stackArguments += 2;
            }
        }
        else {
            integerIndex < 6 ? ++integerIndex : ++stackArguments;
        }
    }
    return AlignUp(static_cast<int>(stackArguments * 8), 16);
}

void X86_64CallEmitter::StoreSysVStackArguments(const std::vector<LirReg> &arguments, const int startIndex) const {
    int integerIndex = startIndex;
    int floatIndex = 0;
    int stackIndex = 0;
    for (const LirReg argument : arguments) {
        const auto &types = framePlan.RegisterTypes();
        const TypeRef type = types.contains(argument) ? types.at(argument) : TypeRef::MakeInt64();
        if (IsSysVMemoryAggregate(type)) {
            for (int offset = 0; offset < AlignUp(hooks.SizeOfRuntime(type), 8); offset += 8) {
                encoder.MovRaxLoad(Disp(argument) + offset);
                encoder.MovRaxStoreRsp(stackIndex++ * 8);
            }
            continue;
        }
        if (hooks.IsAggregate(type) && hooks.SizeOfRuntime(type) == 16) {
            if (integerIndex <= 4) {
                integerIndex += 2;
            }
            else {
                encoder.MovRaxLoad(Disp(argument));
                encoder.MovRaxStoreRsp(stackIndex++ * 8);
                encoder.MovRaxLoad(Disp(argument) + 8);
                encoder.MovRaxStoreRsp(stackIndex++ * 8);
            }
            continue;
        }

        bool onStack = false;
        if (IsFloat(type)) {
            onStack = floatIndex >= 8;
            ++floatIndex;
        }
        else {
            onStack = integerIndex >= 6;
            ++integerIndex;
        }
        if (!onStack) {
            continue;
        }
        hooks.LoadA(argument, type);
        const std::int32_t offset = stackIndex++ * 8;
        if (IsFloat(type)) {
            SizeOf(type) == 4 ? encoder.MovssXmm0StoreRsp(offset) : encoder.MovsdXmm0StoreRsp(offset);
        }
        else {
            encoder.MovRaxStoreRsp(offset);
        }
    }
}

void X86_64CallEmitter::EmitArguments(const std::vector<LirReg> &arguments, const CallingConvention convention,
                                      const int startIndex) const {
    const auto &types = framePlan.RegisterTypes();
    const auto &physicalRegisters = framePlan.PhysicalRegisters();
    if (EffectiveConvention(convention) == CallingConvention::Win64) {
        int index = startIndex;
        for (const LirReg argument : arguments) {
            const TypeRef type = types.contains(argument) ? types.at(argument) : TypeRef::MakeInt64();
            const std::int32_t displacement = Disp(argument);
            if (index >= 4) {
                const std::int32_t stackOffset = 32 + (index - 4) * 8;
                if (IsFloat(type)) {
                    hooks.LoadA(argument, type);
                    SizeOf(type) == 4 ? encoder.MovssXmm0StoreRsp(stackOffset) : encoder.MovsdXmm0StoreRsp(stackOffset);
                }
                else if (IsWin64ByRefAggregate(type)) {
                    encoder.LeaRaxStack(displacement);
                    encoder.MovRaxStoreRsp(stackOffset);
                }
                else {
                    hooks.LoadA(argument, type);
                    encoder.MovRaxStoreRsp(stackOffset);
                }
                ++index;
                continue;
            }
            if (IsFloat(type)) {
                SizeOf(type) == 4 ? encoder.MovssXmmNLoad(index, displacement)
                                  : encoder.MovsdXmmNLoad(index, displacement);
            }
            else if (IsWin64ByRefAggregate(type)) {
                encoder.LeaArgStackWin64(index, displacement);
            }
            else if (IsPointerToWin64ByRefAggregate(type) && !physicalRegisters.contains(argument)) {
                encoder.MovArgLoadWin64(index, displacement);
            }
            else {
                hooks.LoadA(argument, type);
                encoder.MovArgWin64Rax(index);
            }
            ++index;
        }
        return;
    }

    int integerIndex = startIndex;
    int floatIndex = 0;
    for (const LirReg argument : arguments) {
        const TypeRef type = types.contains(argument) ? types.at(argument) : TypeRef::MakeInt64();
        const std::int32_t displacement = Disp(argument);
        if (IsFloat(type)) {
            if (floatIndex < 8) {
                SizeOf(type) == 4 ? encoder.MovssXmmNLoad(floatIndex, displacement)
                                  : encoder.MovsdXmmNLoad(floatIndex, displacement);
                ++floatIndex;
            }
        }
        else if (hooks.IsAggregate(type) && hooks.SizeOfRuntime(type) == 16) {
            if (integerIndex <= 4) {
                encoder.MovRaxLoad(displacement);
                encoder.MovArgRax(integerIndex++);
                encoder.MovRaxLoad(displacement + 8);
                encoder.MovArgRax(integerIndex++);
            }
        }
        else if (integerIndex < 6) {
            hooks.LoadA(argument, type);
            encoder.MovArgRax(integerIndex++);
        }
    }
    // System V AMD64 requires AL to name the used vector-register count for
    // variadic calls; the register is harmless scratch for fixed calls.
    encoder.MovEaxImm32(floatIndex);
}

void X86_64CallEmitter::StoreReturnValue(const LirReg destination, const TypeRef &type) const {
    if (hooks.SizeOfRuntime(type) == 16) {
        encoder.MovRaxStore(Disp(destination));
        encoder.Byte(0x48);
        encoder.Byte(0x89);
        encoder.Byte(0x95);
        encoder.Dword(static_cast<std::uint32_t>(Disp(destination) + 8)); // mov [rbp+disp], rdx
    }
    else {
        hooks.StoreA(destination, type);
    }
}

bool X86_64CallEmitter::EmitBitCast(const LirInstr &instruction) const {
    if (instruction.srcs.size() != 1) {
        return false;
    }
    if (instruction.strArg == "FloatBits64") {
        encoder.MovsdXmm0Load(Disp(instruction.srcs[0]));
        encoder.Byte(0x66);
        encoder.Byte(0x48);
        encoder.Byte(0x0F);
        encoder.Byte(0x7E);
        encoder.Byte(0xC0); // movq rax, xmm0
    }
    else if (instruction.strArg == "FloatFromBits64") {
        hooks.LoadA(instruction.srcs[0], TypeRef::MakeInt64());
        encoder.Byte(0x66);
        encoder.Byte(0x48);
        encoder.Byte(0x0F);
        encoder.Byte(0x6E);
        encoder.Byte(0xC0); // movq xmm0, rax
    }
    else if (instruction.strArg == "FloatBits32") {
        encoder.MovssXmm0Load(Disp(instruction.srcs[0]));
        encoder.Byte(0x66);
        encoder.Byte(0x0F);
        encoder.Byte(0x7E);
        encoder.Byte(0xC0); // movd eax, xmm0
    }
    else if (instruction.strArg == "FloatFromBits32") {
        hooks.LoadA(instruction.srcs[0], TypeRef::MakeInt32());
        encoder.Byte(0x66);
        encoder.Byte(0x0F);
        encoder.Byte(0x6E);
        encoder.Byte(0xC0); // movd xmm0, eax
    }
    else {
        return false;
    }
    if (instruction.dst != LirNoReg && !instruction.type.IsOpaque()) {
        StoreReturnValue(instruction.dst, instruction.type);
    }
    return true;
}

void X86_64CallEmitter::EmitPreparedCall(const LirInstr &instruction, const std::vector<LirReg> &arguments,
                                         const bool indirect) const {
    const bool win64 = EffectiveConvention(instruction.callConv) == CallingConvention::Win64;
    const bool hiddenReturn = instruction.dst != LirNoReg && (win64 ? IsWin64ByRefAggregate(instruction.type)
                                                                    : IsSysVMemoryAggregate(instruction.type));
    const int startIndex = hiddenReturn ? 1 : 0;
    const int frameSize = CallFrameSize(arguments, win64, startIndex);
    if (frameSize > 0) {
        encoder.SubRspImm32(frameSize);
    }
    if (hiddenReturn) {
        if (win64) {
            encoder.LeaArgStackWin64(0, Disp(instruction.dst));
        }
        else {
            encoder.LeaRaxStack(Disp(instruction.dst));
            encoder.MovArgRax(0);
        }
    }
    if (!win64) {
        StoreSysVStackArguments(arguments, startIndex);
    }
    EmitArguments(arguments, instruction.callConv, startIndex);

    if (indirect) {
        const LirReg callee = instruction.srcs.front();
        if (const auto physical = framePlan.PhysicalRegisters().find(callee);
            physical != framePlan.PhysicalRegisters().end()) {
            encoder.MovR10PhysReg(physical->second);
        }
        else {
            encoder.MovR10Load(Disp(callee));
        }
        encoder.CallR10();
    }
    else {
        std::uint32_t relocationOffset;
        encoder.Call(relocationOffset);
        hooks.AddTextRelocation(relocationOffset, hooks.ResolveCallSymbol(instruction.strArg));
    }
    if (frameSize > 0) {
        encoder.AddRspImm32(frameSize);
    }
    if (instruction.dst != LirNoReg && !instruction.type.IsOpaque() && !hiddenReturn) {
        StoreReturnValue(instruction.dst, instruction.type);
    }
}

bool X86_64CallEmitter::Emit(const LirInstr &instruction) {
    if (instruction.op == LirOpcode::Call) {
        if (!EmitBitCast(instruction)) {
            EmitPreparedCall(instruction, instruction.srcs, false);
        }
        return true;
    }
    if (instruction.op != LirOpcode::CallIndirect) {
        return false;
    }
    if (!instruction.srcs.empty()) {
        const std::vector<LirReg> arguments(instruction.srcs.begin() + 1, instruction.srcs.end());
        EmitPreparedCall(instruction, arguments, true);
    }
    return true;
}

X86_64TerminatorEmitter::X86_64TerminatorEmitter(X64Enc &encoder, const X86_64FramePlan &framePlan,
                                                 X86_64CallAndTerminatorHooks &hooks)
    : encoder(encoder)
    , framePlan(framePlan)
    , hooks(hooks) {
}

void X86_64TerminatorEmitter::Begin(const std::size_t blockCount) {
    blockOffsets.assign(blockCount, 0);
    jumpPatches.clear();
}

void X86_64TerminatorEmitter::MarkBlock(const std::uint32_t blockIndex) {
    blockOffsets.at(blockIndex) = encoder.Size();
}

std::int32_t X86_64TerminatorEmitter::Disp(const LirReg reg) const {
    return -framePlan.SlotOffsets().at(reg);
}

bool X86_64TerminatorEmitter::HasPhiMoves(const std::uint32_t fromBlock, const std::uint32_t toBlock) const {
    const auto from = framePlan.PhiMoves().find(fromBlock);
    return from != framePlan.PhiMoves().end() && from->second.contains(toBlock);
}

void X86_64TerminatorEmitter::EmitPhiMoves(const std::uint32_t fromBlock, const std::uint32_t toBlock) {
    const auto from = framePlan.PhiMoves().find(fromBlock);
    if (from == framePlan.PhiMoves().end()) {
        return;
    }
    const auto edge = from->second.find(toBlock);
    if (edge == from->second.end()) {
        return;
    }
    std::vector<PhiMove> moves;
    moves.reserve(edge->second.size());
    for (const auto &move : edge->second) {
        moves.push_back({move.dst, move.src, move.type});
    }
    const std::int32_t temporary = -framePlan.PhiTemporaryOffset();
    for (const PhiMoveStep &step : ResolvePhiMoves(std::move(moves))) {
        const int size = hooks.SizeOfRuntime(step.type);
        if (step.kind == PhiMoveStep::Kind::SaveDestination) {
            hooks.LoadA(step.dst, step.type);
            if (size == 16) {
                encoder.MovRaxStore(temporary);
                encoder.Byte(0x48);
                encoder.Byte(0x89);
                encoder.Byte(0x95);
                encoder.Dword(static_cast<std::uint32_t>(temporary + 8));
            }
            else if (IsFloat(step.type)) {
                step.type.kind == TypeRef::Kind::Float32 ? encoder.MovssXmm0Store(temporary)
                                                         : encoder.MovsdXmm0Store(temporary);
            }
            else if (const int scalarSize = size > 0 ? size : 8; scalarSize == 8) {
                encoder.MovRaxStore(temporary);
            }
            else if (scalarSize == 4) {
                encoder.MovEaxStore(temporary);
            }
            else if (scalarSize == 2) {
                encoder.MovAxStore(temporary);
            }
            else {
                encoder.MovAlStore(temporary);
            }
            continue;
        }
        if (!step.sourceIsTemporary) {
            hooks.LoadA(step.src, step.type);
            hooks.StoreA(step.dst, step.type);
            continue;
        }
        if (size == 16) {
            encoder.MovRaxLoad(temporary);
            encoder.MovR10Load(temporary + 8);
            encoder.Byte(0x4C);
            encoder.Byte(0x89);
            encoder.Byte(0xD2); // mov rdx, r10
        }
        else if (IsFloat(step.type)) {
            step.type.kind == TypeRef::Kind::Float32 ? encoder.MovssXmm0Load(temporary)
                                                     : encoder.MovsdXmm0Load(temporary);
        }
        else if (const int scalarSize = size > 0 ? size : 8; scalarSize == 8) {
            encoder.MovRaxLoad(temporary);
        }
        else if (step.type.IsSigned()) {
            if (scalarSize == 4) {
                encoder.MovsxdRaxDword(temporary);
            }
            else if (scalarSize == 2) {
                encoder.MovsxRaxWord(temporary);
            }
            else {
                encoder.MovsxRaxByte(temporary);
            }
        }
        else if (scalarSize == 4) {
            encoder.MovEaxLoad(temporary);
        }
        else if (scalarSize == 2) {
            encoder.MovzxRaxWord(temporary);
        }
        else {
            encoder.MovzxRaxByte(temporary);
        }
        hooks.StoreA(step.dst, step.type);
    }
}

void X86_64TerminatorEmitter::LoadReturnValue(const LirReg reg, const TypeRef &type) const {
    const int size = hooks.SizeOfRuntime(type);
    if (hooks.IsRegPointerTo(reg, type) && (size == 1 || size == 2 || size == 4 || size == 8 || size == 16)) {
        if (const auto physical = framePlan.PhysicalRegisters().find(reg);
            physical != framePlan.PhysicalRegisters().end()) {
            encoder.MovR10PhysReg(physical->second);
        }
        else {
            encoder.MovR10Load(Disp(reg));
        }
        if (size == 16) {
            encoder.Byte(0x49);
            encoder.Byte(0x8B);
            encoder.Byte(0x02);
            encoder.Byte(0x49);
            encoder.Byte(0x8B);
            encoder.Byte(0x52);
            encoder.Byte(0x08);
        }
        else if (size == 8) {
            encoder.Byte(0x49);
            encoder.Byte(0x8B);
            encoder.Byte(0x82);
            encoder.Dword(0);
        }
        else if (size == 4) {
            encoder.Byte(0x41);
            encoder.Byte(0x8B);
            encoder.Byte(0x82);
            encoder.Dword(0);
        }
        else if (size == 2) {
            encoder.Byte(0x41);
            encoder.Byte(0x0F);
            encoder.Byte(0xB7);
            encoder.Byte(0x82);
            encoder.Dword(0);
        }
        else {
            encoder.Byte(0x41);
            encoder.Byte(0x0F);
            encoder.Byte(0xB6);
            encoder.Byte(0x82);
            encoder.Dword(0);
        }
        return;
    }
    if (size == 16) {
        encoder.MovRaxLoad(Disp(reg));
        encoder.MovR10Load(Disp(reg) + 8);
        encoder.Byte(0x4C);
        encoder.Byte(0x89);
        encoder.Byte(0xD2); // mov rdx, r10
    }
    else {
        hooks.LoadA(reg, type);
    }
}

void X86_64TerminatorEmitter::Emit(const std::uint32_t blockIndex, const LirTerminator &terminator) {
    const auto &types = framePlan.RegisterTypes();
    switch (terminator.kind) {
    case LirTermKind::Jump: {
        EmitPhiMoves(blockIndex, terminator.trueTarget);
        std::uint32_t patch;
        encoder.Jmp(patch);
        jumpPatches.push_back({patch, terminator.trueTarget});
        break;
    }
    case LirTermKind::Branch: {
        hooks.LoadA(terminator.cond, types.contains(terminator.cond) ? types.at(terminator.cond) : TypeRef::MakeBool());
        encoder.TestRaxRax();
        const bool truePhi = HasPhiMoves(blockIndex, terminator.trueTarget);
        const bool falsePhi = HasPhiMoves(blockIndex, terminator.falseTarget);
        if (!truePhi && !falsePhi) {
            std::uint32_t falsePatch;
            encoder.Jz(falsePatch);
            jumpPatches.push_back({falsePatch, terminator.falseTarget});
            std::uint32_t truePatch;
            encoder.Jmp(truePatch);
            jumpPatches.push_back({truePatch, terminator.trueTarget});
        }
        else {
            std::uint32_t falseTrampoline;
            encoder.Jz(falseTrampoline);
            EmitPhiMoves(blockIndex, terminator.trueTarget);
            std::uint32_t truePatch;
            encoder.Jmp(truePatch);
            jumpPatches.push_back({truePatch, terminator.trueTarget});
            const auto here = static_cast<std::int32_t>(encoder.Size());
            encoder.Patch32(falseTrampoline, here - static_cast<std::int32_t>(falseTrampoline + 4));
            EmitPhiMoves(blockIndex, terminator.falseTarget);
            std::uint32_t falsePatch;
            encoder.Jmp(falsePatch);
            jumpPatches.push_back({falsePatch, terminator.falseTarget});
        }
        break;
    }
    case LirTermKind::Return:
        if (terminator.retVal && *terminator.retVal != LirNoReg) {
            framePlan.HiddenReturnOffset() != 0 ? hooks.StoreHiddenReturnValue(*terminator.retVal, terminator.retType)
                                                : LoadReturnValue(*terminator.retVal, terminator.retType);
        }
        if (const auto &saved = framePlan.UsedPhysicalRegisters(); !saved.empty()) {
            const std::int32_t remainder = framePlan.FrameSize() - static_cast<std::int32_t>(saved.size() * 8);
            if (remainder > 0) {
                encoder.AddRspImm32(remainder);
            }
            for (auto reg = saved.rbegin(); reg != saved.rend(); ++reg) {
                encoder.PopReg(*reg);
            }
            encoder.PopRbp();
        }
        else {
            encoder.Leave();
        }
        encoder.Ret();
        break;
    case LirTermKind::Switch:
        hooks.LoadA(terminator.cond,
                    types.contains(terminator.cond) ? types.at(terminator.cond) : TypeRef::MakeInt64());
        for (const auto &caseValue : terminator.cases) {
            encoder.CmpRaxImm32(static_cast<std::int32_t>(ParseIntegerLiteralBits(caseValue.value).value_or(0)));
            std::uint32_t patch;
            encoder.Je(patch);
            jumpPatches.push_back({patch, caseValue.target});
        }
        EmitPhiMoves(blockIndex, terminator.defaultTarget);
        {
            std::uint32_t patch;
            encoder.Jmp(patch);
            jumpPatches.push_back({patch, terminator.defaultTarget});
        }
        break;
    case LirTermKind::Unreachable:
        encoder.Ud2();
        break;
    }
}

void X86_64TerminatorEmitter::PatchJumps() {
    for (const JumpPatch &patch : jumpPatches) {
        const auto target = static_cast<std::int32_t>(blockOffsets.at(patch.targetBlock));
        encoder.Patch32(patch.patchOffset, target - static_cast<std::int32_t>(patch.patchOffset + 4));
    }
    jumpPatches.clear();
}
} // namespace Rux

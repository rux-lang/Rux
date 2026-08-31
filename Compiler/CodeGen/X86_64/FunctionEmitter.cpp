#include "CodeGen/X86_64/FunctionEmitter.h"

#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "CodeGen/X86_64/Encoder.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace Rux {
using namespace Layout;

X86_64FunctionEmitter::X86_64FunctionEmitter(X64Enc &encoder, const X86_64FramePlan &framePlan,
                                             const LayoutMap &layouts,
                                             const std::unordered_set<std::string> &interfaceNames,
                                             X86_64FunctionEmitterHooks &hooks)
    : encoder(encoder)
    , framePlan(framePlan)
    , layouts(layouts)
    , interfaceNames(interfaceNames)
    , hooks(hooks) {
}

std::int32_t X86_64FunctionEmitter::Disp(const LirReg reg) const {
    return -framePlan.SlotOffsets().at(reg);
}

int X86_64FunctionEmitter::SizeOfRuntime(const TypeRef &type) const {
    return RuntimeSizeOf(type, layouts, interfaceNames);
}

bool X86_64FunctionEmitter::IsAggregate(const TypeRef &type) const {
    if (IsWideInteger(type) || (IsSoftwareFloat(type) && SizeOfRuntime(type) > 8)) {
        return true;
    }
    if (type.IsRange()) {
        return true;
    }
    // A string is a 16-byte {data, length} view, exactly the shape a slice has, so it is
    // classified and placed the way a slice is.
    if (type.IsString()) {
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

bool X86_64FunctionEmitter::IsRegPointerTo(const LirReg reg, const TypeRef &pointee) const {
    const auto &registerTypes = framePlan.RegisterTypes();
    const auto type = registerTypes.find(reg);
    return type != registerTypes.end() && type->second.kind == TypeRef::Kind::Pointer && !type->second.inner.empty() &&
           type->second.inner[0] == pointee;
}

int X86_64FunctionEmitter::FieldOffset(const LirReg base, const std::string &fieldName) const {
    const auto &registerTypes = framePlan.RegisterTypes();
    const auto type = registerTypes.find(base);
    if (type == registerTypes.end()) {
        return 0;
    }
    return FieldOffsetOf(type->second, fieldName, layouts, interfaceNames);
}

bool X86_64FunctionEmitter::EmitWideArithmetic(const LirInstr &instruction) {
    const auto &types = framePlan.RegisterTypes();
    const TypeRef operandType = !instruction.srcs.empty() && types.contains(instruction.srcs[0])
                                  ? types.at(instruction.srcs[0])
                                  : instruction.type;
    if (EmitWideSoftwareFloatNegation(instruction)) {
        return true;
    }
    if (instruction.dst == LirNoReg || (!IsWideInteger(operandType) && !IsWideInteger(instruction.type))) {
        return false;
    }
    const TypeRef wideType = IsWideInteger(operandType) ? operandType : instruction.type;
    const int size = SizeOf(wideType);
    const int words = size / 8;
    const std::int32_t destination = Disp(instruction.dst);
    const auto source = [&](const std::size_t index) { return Disp(instruction.srcs.at(index)); };

    switch (instruction.op) {
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor:
        for (int word = 0; word < words; ++word) {
            encoder.MovRaxLoad(source(0) + word * 8);
            encoder.MovR10Load(source(1) + word * 8);
            if (instruction.op == LirOpcode::Add) {
                if (word == 0) {
                    encoder.AddRaxR10();
                }
                else {
                    encoder.Byte(0x4C);
                    encoder.Byte(0x11);
                    encoder.Byte(0xD0); // adc rax, r10
                }
            }
            else if (instruction.op == LirOpcode::Sub) {
                if (word == 0) {
                    encoder.SubRaxR10();
                }
                else {
                    encoder.Byte(0x4C);
                    encoder.Byte(0x19);
                    encoder.Byte(0xD0); // sbb rax, r10
                }
            }
            else if (instruction.op == LirOpcode::And) {
                encoder.AndRaxR10();
            }
            else if (instruction.op == LirOpcode::Or) {
                encoder.OrRaxR10();
            }
            else {
                encoder.XorRaxR10();
            }
            encoder.MovRaxStore(destination + word * 8);
        }
        return true;
    case LirOpcode::Mul:
        MultiplyWide(source(0), source(1), destination, size);
        return true;
    case LirOpcode::Div:
    case LirOpcode::Mod: {
        const std::int32_t remainder = -framePlan.WideTemporaryOffset(0);
        const std::int32_t dividend = -framePlan.WideTemporaryOffset(1);
        const std::int32_t divisor = -framePlan.WideTemporaryOffset(2);

        encoder.MovEaxImm32(0);
        for (int word = 0; word < words; ++word) {
            encoder.MovR10Load(source(1) + word * 8);
            encoder.OrRaxR10();
        }
        encoder.TestRaxRax();
        const std::uint32_t nonZeroDivisor = JumpIf(0x85);
        encoder.Ud2();
        PatchHere(nonZeroDivisor);

        if (wideType.IsSigned()) {
            std::vector<std::uint32_t> notOverflow;
            encoder.MovRaxImm64(std::numeric_limits<std::int64_t>::min());
            encoder.MovR10Load(source(0) + size - 8);
            encoder.CmpRaxR10();
            notOverflow.push_back(JumpIf(0x85));
            encoder.MovEaxImm32(0);
            for (int word = 0; word < words - 1; ++word) {
                encoder.MovR10Load(source(0) + word * 8);
                encoder.CmpRaxR10();
                notOverflow.push_back(JumpIf(0x85));
            }
            encoder.MovRaxImm64(-1);
            for (int word = 0; word < words; ++word) {
                encoder.MovR10Load(source(1) + word * 8);
                encoder.CmpRaxR10();
                notOverflow.push_back(JumpIf(0x85));
            }
            encoder.Ud2();
            for (const std::uint32_t patch : notOverflow) {
                PatchHere(patch);
            }
        }

        CopyWide(source(0), dividend, size);
        CopyWide(source(1), divisor, size);
        if (wideType.IsSigned()) {
            encoder.MovRaxLoad(dividend + size - 8);
            encoder.TestRaxRax();
            const std::uint32_t dividendPositive = JumpIf(0x89);
            NegateWide(dividend, size);
            PatchHere(dividendPositive);
            encoder.MovRaxLoad(divisor + size - 8);
            encoder.TestRaxRax();
            const std::uint32_t divisorPositive = JumpIf(0x89);
            NegateWide(divisor, size);
            PatchHere(divisorPositive);
        }

        ZeroWide(destination, size);
        ZeroWide(remainder, size);
        encoder.Byte(0x48);
        encoder.Byte(0xC7);
        encoder.Byte(0xC1); // mov rcx, width
        encoder.Dword(static_cast<std::uint32_t>(size * 8));
        const std::uint32_t divideLoop = encoder.Size();

        for (int word = 0; word < words; ++word) {
            encoder.Byte(0x48);
            encoder.Byte(0xD1);
            encoder.Byte(word == 0 ? 0xA5 : 0x95); // shl/rcl quotient
            encoder.Dword(static_cast<std::uint32_t>(destination + word * 8));
        }
        for (int word = 0; word < words; ++word) {
            encoder.Byte(0x48);
            encoder.Byte(0xD1);
            encoder.Byte(word == 0 ? 0xA5 : 0x95); // shl/rcl dividend, leaving its top bit in carry
            encoder.Dword(static_cast<std::uint32_t>(dividend + word * 8));
        }
        for (int word = 0; word < words; ++word) {
            encoder.Byte(0x48);
            encoder.Byte(0xD1);
            encoder.Byte(0x95); // rcl remainder, consuming the dividend bit
            encoder.Dword(static_cast<std::uint32_t>(remainder + word * 8));
        }

        std::vector<std::uint32_t> remainderLess;
        std::vector<std::uint32_t> remainderGreater;
        for (int word = words - 1; word >= 0; --word) {
            encoder.MovRaxLoad(remainder + word * 8);
            encoder.MovR10Load(divisor + word * 8);
            encoder.CmpRaxR10();
            remainderLess.push_back(JumpIf(0x82));
            remainderGreater.push_back(JumpIf(0x87));
        }
        for (const std::uint32_t patch : remainderGreater) {
            PatchHere(patch);
        }
        for (int word = 0; word < words; ++word) {
            encoder.MovRaxLoad(remainder + word * 8);
            encoder.MovR10Load(divisor + word * 8);
            if (word == 0) {
                encoder.SubRaxR10();
            }
            else {
                encoder.Byte(0x4C);
                encoder.Byte(0x19);
                encoder.Byte(0xD0); // sbb rax, r10
            }
            encoder.MovRaxStore(remainder + word * 8);
        }
        encoder.Byte(0x48);
        encoder.Byte(0x83);
        encoder.Byte(0x8D); // or qword [rbp + quotient], 1
        encoder.Dword(static_cast<std::uint32_t>(destination));
        encoder.Byte(0x01);
        for (const std::uint32_t patch : remainderLess) {
            PatchHere(patch);
        }
        encoder.Byte(0x48);
        encoder.Byte(0xFF);
        encoder.Byte(0xC9); // dec rcx
        encoder.Byte(0x0F);
        encoder.Byte(0x85);
        const std::uint32_t divideRepeat = encoder.Size();
        encoder.Dword(0);
        encoder.Patch32(divideRepeat,
                        static_cast<std::int32_t>(divideLoop) - static_cast<std::int32_t>(divideRepeat + 4));

        if (instruction.op == LirOpcode::Mod) {
            CopyWide(remainder, destination, size);
            if (wideType.IsSigned()) {
                encoder.MovRaxLoad(source(0) + size - 8);
                encoder.TestRaxRax();
                const std::uint32_t positive = JumpIf(0x89);
                NegateWide(destination, size);
                PatchHere(positive);
            }
        }
        else if (wideType.IsSigned()) {
            encoder.MovRaxLoad(source(0) + size - 8);
            encoder.MovR10Load(source(1) + size - 8);
            encoder.XorRaxR10();
            encoder.TestRaxRax();
            const std::uint32_t positive = JumpIf(0x89);
            NegateWide(destination, size);
            PatchHere(positive);
        }
        return true;
    }
    case LirOpcode::Neg:
        CopyWide(source(0), destination, size);
        NegateWide(destination, size);
        return true;
    case LirOpcode::BitNot:
        for (int word = 0; word < words; ++word) {
            encoder.MovRaxLoad(source(0) + word * 8);
            encoder.NotRax();
            encoder.MovRaxStore(destination + word * 8);
        }
        return true;
    case LirOpcode::Not: {
        encoder.MovEaxImm32(0);
        for (int word = 0; word < words; ++word) {
            encoder.MovR10Load(source(0) + word * 8);
            encoder.OrRaxR10();
        }
        encoder.TestRaxRax();
        encoder.SeteAl();
        encoder.MovzxRaxAl();
        hooks.StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr: {
        CopyWide(source(0), destination, size);
        if (const auto physical = framePlan.PhysicalRegisters().find(instruction.srcs[1]);
            physical != framePlan.PhysicalRegisters().end()) {
            encoder.MovR11PhysReg(physical->second);
            encoder.MovRcxR11();
        }
        else {
            encoder.MovRcxLoad(source(1));
        }
        encoder.Byte(0x48);
        encoder.Byte(0x81);
        encoder.Byte(0xE1); // and rcx, width - 1
        encoder.Dword(static_cast<std::uint32_t>(size * 8 - 1));
        encoder.Byte(0x48);
        encoder.Byte(0x85);
        encoder.Byte(0xC9); // test rcx, rcx
        const std::uint32_t done = JumpIf(0x84);
        const std::uint32_t loop = encoder.Size();
        if (instruction.op == LirOpcode::Shl) {
            for (int word = 0; word < words; ++word) {
                encoder.Byte(0x48);
                encoder.Byte(0xD1);
                encoder.Byte(word == 0 ? 0xA5 : 0x95); // shl/rcl qword [rbp + disp32], 1
                encoder.Dword(static_cast<std::uint32_t>(destination + word * 8));
            }
        }
        else {
            for (int word = words - 1; word >= 0; --word) {
                encoder.Byte(0x48);
                encoder.Byte(0xD1);
                const bool top = word == words - 1;
                const std::uint8_t modRm =
                    top ? (instruction.op == LirOpcode::Shr && wideType.IsSigned() ? 0xBD : 0xAD) : 0x9D;
                encoder.Byte(modRm); // sar/shr/rcr qword [rbp + disp32], 1
                encoder.Dword(static_cast<std::uint32_t>(destination + word * 8));
            }
        }
        encoder.Byte(0x48);
        encoder.Byte(0xFF);
        encoder.Byte(0xC9); // dec rcx
        encoder.Byte(0x0F);
        encoder.Byte(0x85);
        const std::uint32_t repeat = encoder.Size();
        encoder.Dword(0);
        encoder.Patch32(repeat, static_cast<std::int32_t>(loop) - static_cast<std::int32_t>(repeat + 4));
        PatchHere(done);
        return true;
    }
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe: {
        encoder.MovEaxImm32(instruction.op == LirOpcode::CmpEq ? 1 : 0);
        std::vector<std::uint32_t> differences;
        for (int word = 0; word < words; ++word) {
            encoder.MovR10Load(source(0) + word * 8);
            encoder.MovRaxLoad(source(1) + word * 8);
            encoder.CmpRaxR10();
            differences.push_back(JumpIf(0x85)); // jne different
        }
        encoder.MovEaxImm32(instruction.op == LirOpcode::CmpEq ? 1 : 0);
        std::uint32_t done;
        encoder.Jmp(done);
        for (const std::uint32_t difference : differences) {
            PatchHere(difference);
        }
        encoder.MovEaxImm32(instruction.op == LirOpcode::CmpEq ? 0 : 1);
        PatchHere(done);
        hooks.StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe: {
        std::vector<std::uint32_t> less;
        std::vector<std::uint32_t> greater;
        for (int word = words - 1; word >= 0; --word) {
            encoder.MovRaxLoad(source(0) + word * 8);
            encoder.MovR10Load(source(1) + word * 8);
            encoder.CmpRaxR10();
            const bool signedWord = word == words - 1 && wideType.IsSigned();
            less.push_back(JumpIf(signedWord ? 0x8C : 0x82));    // jl/jb
            greater.push_back(JumpIf(signedWord ? 0x8F : 0x87)); // jg/ja
        }
        const bool equalResult = instruction.op == LirOpcode::CmpLe || instruction.op == LirOpcode::CmpGe;
        encoder.MovEaxImm32(equalResult ? 1 : 0);
        std::uint32_t equalDone;
        encoder.Jmp(equalDone);
        for (const std::uint32_t patch : less) {
            PatchHere(patch);
        }
        const bool lessResult = instruction.op == LirOpcode::CmpLt || instruction.op == LirOpcode::CmpLe;
        encoder.MovEaxImm32(lessResult ? 1 : 0);
        std::uint32_t lessDone;
        encoder.Jmp(lessDone);
        for (const std::uint32_t patch : greater) {
            PatchHere(patch);
        }
        const bool greaterResult = instruction.op == LirOpcode::CmpGt || instruction.op == LirOpcode::CmpGe;
        encoder.MovEaxImm32(greaterResult ? 1 : 0);
        PatchHere(equalDone);
        PatchHere(lessDone);
        hooks.StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::Cast: {
        const TypeRef &destinationType = instruction.type;
        if (destinationType.IsBool()) {
            encoder.MovEaxImm32(0);
            for (int word = 0; word < words; ++word) {
                encoder.MovR10Load(source(0) + word * 8);
                encoder.OrRaxR10();
            }
            encoder.TestRaxRax();
            encoder.SetneAl();
            encoder.MovzxRaxAl();
            hooks.StoreA(instruction.dst, destinationType);
            return true;
        }
        if (IsWideInteger(destinationType)) {
            const int destinationSize = SizeOf(destinationType);
            if (!IsWideInteger(operandType)) {
                hooks.LoadA(instruction.srcs[0], operandType);
                encoder.MovRaxStore(destination);
                if (operandType.IsSigned()) {
                    encoder.Byte(0x48);
                    encoder.Byte(0xC1);
                    encoder.Byte(0xF8);
                    encoder.Byte(0x3F); // sar rax, 63
                }
                else {
                    encoder.MovEaxImm32(0);
                }
                for (int offset = 8; offset < destinationSize; offset += 8) {
                    encoder.MovRaxStore(destination + offset);
                }
                return true;
            }
            const int copiedSize = std::min(size, destinationSize);
            CopyWide(source(0), destination, copiedSize);
            if (destinationSize > copiedSize) {
                encoder.MovEaxImm32(0);
                if (wideType.IsSigned()) {
                    encoder.MovR10Load(source(0) + size - 8);
                    encoder.Byte(0x4C);
                    encoder.Byte(0x89);
                    encoder.Byte(0xD0); // mov rax, r10
                    encoder.Byte(0x48);
                    encoder.Byte(0xC1);
                    encoder.Byte(0xF8);
                    encoder.Byte(0x3F); // sar rax, 63
                }
                for (int offset = copiedSize; offset < destinationSize; offset += 8) {
                    encoder.MovRaxStore(destination + offset);
                }
            }
            return true;
        }
        encoder.MovRaxLoad(source(0));
        hooks.StoreA(instruction.dst, destinationType);
        return true;
    }
    default:
        return false;
    }
}

bool X86_64FunctionEmitter::EmitArithmetic(const LirInstr &instruction) {
    if (EmitWideArithmetic(instruction)) {
        return true;
    }
    const auto &registerTypes = framePlan.RegisterTypes();
    const auto &physicalRegisters = framePlan.PhysicalRegisters();
    switch (instruction.op) {
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor: {
        const TypeRef &type = instruction.type;
        hooks.LoadA(instruction.srcs[0], type);
        hooks.LoadB(instruction.srcs[1], type);
        if (IsFloat(type)) {
            const bool float32 = type.kind == TypeRef::Kind::Float32;
            if (instruction.op == LirOpcode::Add) {
                float32 ? encoder.AddssXmm01() : encoder.AddsdXmm01();
            }
            else if (instruction.op == LirOpcode::Sub) {
                float32 ? encoder.SubssXmm01() : encoder.SubsdXmm01();
            }
            else {
                // Bitwise operations are not produced for floating values;
                // preserve the historical fallback if malformed LIR reaches
                // instruction selection.
                float32 ? encoder.AddssXmm01() : encoder.AddsdXmm01();
            }
        }
        else if (instruction.op == LirOpcode::Add) {
            encoder.AddRaxR10();
        }
        else if (instruction.op == LirOpcode::Sub) {
            encoder.SubRaxR10();
        }
        else if (instruction.op == LirOpcode::And) {
            encoder.AndRaxR10();
        }
        else if (instruction.op == LirOpcode::Or) {
            encoder.OrRaxR10();
        }
        else {
            encoder.XorRaxR10();
        }
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Mul: {
        const TypeRef &type = instruction.type;
        hooks.LoadA(instruction.srcs[0], type);
        hooks.LoadB(instruction.srcs[1], type);
        if (IsFloat(type)) {
            type.kind == TypeRef::Kind::Float32 ? encoder.MulssXmm01() : encoder.MulsdXmm01();
        }
        else {
            encoder.ImulRaxR10();
        }
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Div:
    case LirOpcode::Mod: {
        const TypeRef &type = instruction.type;
        hooks.LoadA(instruction.srcs[0], type);
        hooks.LoadB(instruction.srcs[1], type);
        if (IsFloat(type)) {
            if (instruction.op == LirOpcode::Div) {
                type.kind == TypeRef::Kind::Float32 ? encoder.DivssXmm01() : encoder.DivsdXmm01();
            }
            else {
                type.kind == TypeRef::Kind::Float32 ? encoder.FmodssXmm01() : encoder.FmodsdXmm01();
            }
        }
        else {
            if (type.IsSigned()) {
                encoder.Cqo();
                encoder.IdivR10();
            }
            else {
                encoder.XorRdxRdx();
                encoder.DivR10();
            }
            if (instruction.op == LirOpcode::Mod) {
                encoder.MovRaxRdx();
            }
        }
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr: {
        const TypeRef &type = instruction.type;
        hooks.LoadA(instruction.srcs[0], instruction.op == LirOpcode::Lshr ? UnsignedIntegerType(type) : type);
        if (const auto physical = physicalRegisters.find(instruction.srcs[1]); physical != physicalRegisters.end()) {
            encoder.MovR11PhysReg(physical->second);
        }
        else {
            encoder.MovR11Load(Disp(instruction.srcs[1]));
        }
        encoder.MovRcxR11();
        const bool shiftRight = instruction.op == LirOpcode::Shr || instruction.op == LirOpcode::Lshr;
        if (shiftRight && type.IsSigned() && instruction.op != LirOpcode::Lshr) {
            encoder.SarRaxCl();
        }
        else if (shiftRight) {
            encoder.ShrRaxCl();
        }
        else {
            encoder.ShlRaxCl();
        }
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Neg: {
        const TypeRef &type = instruction.type;
        hooks.LoadA(instruction.srcs[0], type);
        if (IsFloat(type)) {
            const bool float32 = type.kind == TypeRef::Kind::Float32;
            const std::uint32_t maskSymbol = hooks.InternFloatSignMask(float32);
            std::uint32_t relocationOffset;
            float32 ? encoder.MovssXmm1Rip(relocationOffset) : encoder.MovsdXmm1Rip(relocationOffset);
            hooks.AddTextRelocation(relocationOffset, maskSymbol);
            float32 ? encoder.XorpsXmm01() : encoder.XorpdXmm01();
        }
        else {
            encoder.NegRax();
        }
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Not:
        hooks.LoadA(instruction.srcs[0], instruction.type);
        encoder.TestRaxRax();
        encoder.SeteAl();
        encoder.MovzxRaxAl();
        hooks.StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    case LirOpcode::BitNot:
        hooks.LoadA(instruction.srcs[0], instruction.type);
        if (instruction.type.IsBool()) {
            // Bool bitwise NOT is logical NOT so its stored value remains
            // canonical (zero or one).
            encoder.XorRaxImmediate(1);
        }
        else {
            encoder.NotRax();
        }
        hooks.StoreA(instruction.dst, instruction.type);
        return true;
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe: {
        const TypeRef &lhsType =
            registerTypes.contains(instruction.srcs[0]) ? registerTypes.at(instruction.srcs[0]) : instruction.type;
        hooks.LoadA(instruction.srcs[0], lhsType);
        hooks.LoadB(instruction.srcs[1], lhsType);
        if (IsFloat(lhsType)) {
            lhsType.kind == TypeRef::Kind::Float32 ? encoder.UcomissXmm01() : encoder.UcomisdXmm01();
            switch (instruction.op) {
            case LirOpcode::CmpEq:
                encoder.SeteAl();
                encoder.SetnpDl();
                encoder.AndAlDl();
                break;
            case LirOpcode::CmpNe:
                encoder.SetneAl();
                encoder.SetpDl();
                encoder.OrAlDl();
                break;
            case LirOpcode::CmpLt:
                encoder.SetbAl();
                encoder.SetnpDl();
                encoder.AndAlDl();
                break;
            case LirOpcode::CmpLe:
                encoder.SetbeAl();
                encoder.SetnpDl();
                encoder.AndAlDl();
                break;
            case LirOpcode::CmpGt:
                encoder.SetaAl();
                encoder.SetnpDl();
                encoder.AndAlDl();
                break;
            case LirOpcode::CmpGe:
                encoder.SetaeAl();
                encoder.SetnpDl();
                encoder.AndAlDl();
                break;
            default:
                break;
            }
        }
        else {
            encoder.CmpRaxR10();
            const bool signedComparison = lhsType.IsSigned();
            switch (instruction.op) {
            case LirOpcode::CmpEq:
                encoder.SeteAl();
                break;
            case LirOpcode::CmpNe:
                encoder.SetneAl();
                break;
            case LirOpcode::CmpLt:
                signedComparison ? encoder.SetlAl() : encoder.SetbAl();
                break;
            case LirOpcode::CmpLe:
                signedComparison ? encoder.SetleAl() : encoder.SetbeAl();
                break;
            case LirOpcode::CmpGt:
                signedComparison ? encoder.SetgAl() : encoder.SetaAl();
                break;
            default:
                signedComparison ? encoder.SetgeAl() : encoder.SetaeAl();
                break;
            }
        }
        encoder.MovzxRaxAl();
        hooks.StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::Cast: {
        const TypeRef &destinationType = instruction.type;
        const TypeRef sourceType =
            registerTypes.contains(instruction.srcs[0]) ? registerTypes.at(instruction.srcs[0]) : destinationType;
        hooks.LoadA(instruction.srcs[0], sourceType);
        const bool sourceFloat = IsFloat(sourceType);
        const bool destinationFloat = IsFloat(destinationType);
        if (sourceFloat && !destinationFloat) {
            sourceType.kind == TypeRef::Kind::Float32 ? encoder.CvttsssiRaxXmm0() : encoder.CvttsdsiRaxXmm0();
        }
        else if (!sourceFloat && destinationFloat) {
            destinationType.kind == TypeRef::Kind::Float32 ? encoder.Cvtsi2ssXmm0Rax() : encoder.Cvtsi2sdXmm0Rax();
        }
        else if (sourceFloat && destinationFloat) {
            if (sourceType.kind == TypeRef::Kind::Float32 && destinationType.kind == TypeRef::Kind::Float64) {
                encoder.CvtsssdXmm0();
            }
            else if (sourceType.kind == TypeRef::Kind::Float64 && destinationType.kind == TypeRef::Kind::Float32) {
                encoder.CvtsdssXmm0();
            }
        }
        hooks.StoreA(instruction.dst, destinationType);
        return true;
    }
    default:
        return false;
    }
}

void X86_64FunctionEmitter::LoadChunkFromR10(const std::int32_t offset, const int size) const {
    if (size == 8) {
        encoder.Byte(0x49);
        encoder.Byte(0x8B);
        encoder.Byte(0x82); // mov rax, [r10 + disp32]
    }
    else if (size == 4) {
        encoder.Byte(0x41);
        encoder.Byte(0x8B);
        encoder.Byte(0x82); // mov eax, [r10 + disp32]
    }
    else if (size == 2) {
        encoder.Byte(0x41);
        encoder.Byte(0x0F);
        encoder.Byte(0xB7);
        encoder.Byte(0x82); // movzx eax, word [r10 + disp32]
    }
    else {
        encoder.Byte(0x41);
        encoder.Byte(0x0F);
        encoder.Byte(0xB6);
        encoder.Byte(0x82); // movzx eax, byte [r10 + disp32]
    }
    encoder.Dword(static_cast<std::uint32_t>(offset));
}

void X86_64FunctionEmitter::StoreChunkToR11(const std::int32_t offset, const int size) const {
    if (size == 8) {
        encoder.Byte(0x49);
        encoder.Byte(0x89);
        encoder.Byte(0x83); // mov [r11 + disp32], rax
    }
    else if (size == 4) {
        encoder.Byte(0x41);
        encoder.Byte(0x89);
        encoder.Byte(0x83); // mov [r11 + disp32], eax
    }
    else if (size == 2) {
        encoder.Byte(0x66);
        encoder.Byte(0x41);
        encoder.Byte(0x89);
        encoder.Byte(0x83); // mov [r11 + disp32], ax
    }
    else {
        encoder.Byte(0x41);
        encoder.Byte(0x88);
        encoder.Byte(0x83); // mov [r11 + disp32], al
    }
    encoder.Dword(static_cast<std::uint32_t>(offset));
}

void X86_64FunctionEmitter::CopyAggregateFromR10ToStack(const std::int32_t destinationDisp, const int size) const {
    std::int32_t offset = 0;
    for (const int chunkSize : {8, 4, 2, 1}) {
        while (offset + chunkSize <= size) {
            LoadChunkFromR10(offset, chunkSize);
            if (chunkSize == 8) {
                encoder.MovRaxStore(destinationDisp + offset);
            }
            else if (chunkSize == 4) {
                encoder.MovEaxStore(destinationDisp + offset);
            }
            else if (chunkSize == 2) {
                encoder.MovAxStore(destinationDisp + offset);
            }
            else {
                encoder.MovAlStore(destinationDisp + offset);
            }
            offset += chunkSize;
        }
    }
}

void X86_64FunctionEmitter::CopyAggregateFromR10ToR11(const int size) const {
    std::int32_t offset = 0;
    for (const int chunkSize : {8, 4, 2, 1}) {
        while (offset + chunkSize <= size) {
            LoadChunkFromR10(offset, chunkSize);
            StoreChunkToR11(offset, chunkSize);
            offset += chunkSize;
        }
    }
}

bool X86_64FunctionEmitter::EmitMemory(const LirInstr &instruction) {
    const auto &registerTypes = framePlan.RegisterTypes();
    const auto &physicalRegisters = framePlan.PhysicalRegisters();
    switch (instruction.op) {
    case LirOpcode::Const: {
        if (instruction.dst == LirNoReg) {
            return true;
        }
        const TypeRef &type = instruction.type;
        const int size = SizeOf(type);
        if (type.kind == TypeRef::Kind::Float32) {
            const std::uint32_t symbol = hooks.InternFloat32Literal(instruction.strArg);
            std::uint32_t relocationOffset;
            encoder.MovssXmm0Rip(relocationOffset);
            hooks.AddTextRelocation(relocationOffset, symbol);
            encoder.MovssXmm0Store(Disp(instruction.dst));
        }
        else if (type.kind == TypeRef::Kind::Float64) {
            const std::uint32_t symbol = hooks.InternFloat64Literal(instruction.strArg);
            std::uint32_t relocationOffset;
            encoder.MovsdXmm0Rip(relocationOffset);
            hooks.AddTextRelocation(relocationOffset, symbol);
            encoder.MovsdXmm0Store(Disp(instruction.dst));
        }
        else if (IsSoftwareFloat(type)) {
            EmitSoftwareFloatConstant(instruction);
        }
        else if (type.IsBool()) {
            encoder.MovEaxImm32((instruction.strArg == "true" || instruction.strArg == "1") ? 1 : 0);
            hooks.StoreA(instruction.dst, type);
        }
        else if (IsWideInteger(type)) {
            const std::string &literal = instruction.strArg.empty() ? "0" : instruction.strArg;
            const auto parts = SplitIntegerLiteral(literal);
            WideInteger value = WideInteger::Zero(static_cast<std::uint32_t>(size * 8));
            if (parts) {
                if (const auto magnitude =
                        WideInteger::Parse(parts->digits, parts->base, static_cast<std::uint32_t>(size * 8))) {
                    value = parts->negative ? magnitude->Negated() : *magnitude;
                }
            }
            for (int offset = 0; offset < size; offset += 8) {
                const std::uint64_t word = value.Word64(static_cast<std::size_t>(offset / 8));
                if (word <= 0x7FFF'FFFF) {
                    encoder.MovEaxImm32(static_cast<std::int32_t>(word));
                }
                else {
                    encoder.MovRaxImm64(static_cast<std::int64_t>(word));
                }
                encoder.MovRaxStore(Disp(instruction.dst) + offset);
            }
        }
        else {
            const std::string &value = instruction.strArg.empty() ? "0" : instruction.strArg;
            const std::uint64_t bits = ParseIntegerLiteralBits(value).value_or(0);
            if (bits <= 0x7FFF'FFFF) {
                encoder.MovEaxImm32(static_cast<std::int32_t>(bits));
            }
            else {
                encoder.MovRaxImm64(static_cast<std::int64_t>(bits));
            }
            hooks.StoreA(instruction.dst, size > 0 ? type : TypeRef::MakeInt64());
        }
        return true;
    }
    case LirOpcode::Alloca: {
        const std::int32_t dataOffset = framePlan.AllocaDataOffsets().at(instruction.dst);
        encoder.LeaRaxStack(-dataOffset);
        hooks.StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    case LirOpcode::Load: {
        const TypeRef &type = instruction.type;
        const int size = SizeOfRuntime(type);
        const int runtimeSize = SizeOfRuntime(type);
        // Nothing to read either, and reading a word for it could reach past the end of what was allocated.
        if (IsAggregate(type) && runtimeSize == 0) {
            return true;
        }
        if (!instruction.strArg.empty()) {
            const std::uint32_t symbol = hooks.ResolveNamedDataSymbol(instruction.strArg);
            std::uint32_t relocationOffset;
            encoder.MovRaxRip(relocationOffset);
            hooks.AddTextRelocation(relocationOffset, symbol);
        }
        else {
            const LirReg pointer = instruction.srcs[0];
            if (const auto physical = physicalRegisters.find(pointer); physical != physicalRegisters.end()) {
                encoder.MovR10PhysReg(physical->second);
            }
            else {
                encoder.MovR10Load(Disp(pointer));
            }
            if (IsAggregate(type) && (runtimeSize > 8 || (runtimeSize > 0 && runtimeSize != 1 && runtimeSize != 2 &&
                                                          runtimeSize != 4 && runtimeSize != 8))) {
                // Larger than a register, or an odd width no scalar move spells (3, 5, 6, 7): copied in chunks,
                // because a rounded-up load would read past the object and a rounded-down one would lose its tail.
                CopyAggregateFromR10ToStack(Disp(instruction.dst), runtimeSize);
                return true;
            }
            if (IsFloat(type)) {
                if (size == 4) {
                    encoder.Byte(0xF3);
                    encoder.Byte(0x41);
                    encoder.Byte(0x0F);
                    encoder.Byte(0x10);
                    encoder.Byte(0x02); // movss xmm0, [r10]
                }
                else {
                    encoder.Byte(0xF2);
                    encoder.Byte(0x41);
                    encoder.Byte(0x0F);
                    encoder.Byte(0x10);
                    encoder.Byte(0x02); // movsd xmm0, [r10]
                }
                hooks.StoreA(instruction.dst, type);
                return true;
            }
            if (size == 8 || size == 0) {
                encoder.Byte(0x49);
                encoder.Byte(0x8B);
                encoder.Byte(0x02); // mov rax, [r10]
            }
            else if (type.IsSigned()) {
                if (size == 4) {
                    encoder.Byte(0x49);
                    encoder.Byte(0x63);
                    encoder.Byte(0x02); // movsxd rax, [r10]
                }
                else if (size == 2) {
                    encoder.Byte(0x49);
                    encoder.Byte(0x0F);
                    encoder.Byte(0xBF);
                    encoder.Byte(0x02); // movsx rax, word [r10]
                }
                else {
                    encoder.Byte(0x49);
                    encoder.Byte(0x0F);
                    encoder.Byte(0xBE);
                    encoder.Byte(0x02); // movsx rax, byte [r10]
                }
            }
            else if (size == 4) {
                encoder.Byte(0x41);
                encoder.Byte(0x8B);
                encoder.Byte(0x02); // mov eax, [r10]
            }
            else if (size == 2) {
                encoder.Byte(0x49);
                encoder.Byte(0x0F);
                encoder.Byte(0xB7);
                encoder.Byte(0x02); // movzx rax, word [r10]
            }
            else {
                encoder.Byte(0x49);
                encoder.Byte(0x0F);
                encoder.Byte(0xB6);
                encoder.Byte(0x02); // movzx rax, byte [r10]
            }
        }
        hooks.StoreA(instruction.dst, size > 0 ? type : TypeRef::MakeInt64());
        return true;
    }
    case LirOpcode::Store: {
        const LirReg value = instruction.srcs[0];
        const LirReg pointer = instruction.srcs[1];
        const TypeRef &type = instruction.type;
        const int size = SizeOfRuntime(type);
        const int runtimeSize = SizeOfRuntime(type);
        // A value occupying no bytes has nothing to move, and the eight-byte fallback below is for a width the back
        // end could not work out, which is a different thing. Storing a word for a zero-sized field wrote over
        // whatever followed it -- for a node whose value sits between its key and its links, that is the links.
        if (IsAggregate(type) && runtimeSize == 0) {
            return true;
        }
        if (const auto physical = physicalRegisters.find(pointer); physical != physicalRegisters.end()) {
            encoder.MovR11PhysReg(physical->second);
        }
        else {
            encoder.MovR11Load(Disp(pointer));
        }
        if (IsAggregate(type) && (runtimeSize > 8 || (runtimeSize > 0 && runtimeSize != 1 && runtimeSize != 2 &&
                                                      runtimeSize != 4 && runtimeSize != 8))) {
            // The same odd widths on the way out: chunked, so nothing beyond the object is touched.
            if (IsRegPointerTo(value, type)) {
                if (const auto physical = physicalRegisters.find(value); physical != physicalRegisters.end()) {
                    encoder.MovR10PhysReg(physical->second);
                }
                else {
                    encoder.MovR10Load(Disp(value));
                }
            }
            else {
                encoder.Byte(0x4C);
                encoder.Byte(0x8D);
                encoder.Byte(0x95);
                encoder.Dword(static_cast<std::uint32_t>(Disp(value))); // lea r10, [rbp + disp32]
            }
            CopyAggregateFromR10ToR11(runtimeSize);
            return true;
        }
        hooks.LoadA(value, type);
        if (IsFloat(type)) {
            if (size == 4) {
                encoder.Byte(0xF3);
                encoder.Byte(0x41);
                encoder.Byte(0x0F);
                encoder.Byte(0x11);
                encoder.Byte(0x03); // movss [r11], xmm0
            }
            else {
                encoder.Byte(0xF2);
                encoder.Byte(0x41);
                encoder.Byte(0x0F);
                encoder.Byte(0x11);
                encoder.Byte(0x03); // movsd [r11], xmm0
            }
        }
        else {
            const int storeSize = size > 0 ? size : 8;
            if (storeSize == 8) {
                encoder.Byte(0x49);
                encoder.Byte(0x89);
                encoder.Byte(0x03); // mov [r11], rax
            }
            else if (storeSize == 4) {
                encoder.Byte(0x41);
                encoder.Byte(0x89);
                encoder.Byte(0x03); // mov [r11], eax
            }
            else if (storeSize == 2) {
                encoder.Byte(0x66);
                encoder.Byte(0x41);
                encoder.Byte(0x89);
                encoder.Byte(0x03); // mov [r11], ax
            }
            else {
                encoder.Byte(0x41);
                encoder.Byte(0x88);
                encoder.Byte(0x03); // mov [r11], al
            }
        }
        return true;
    }
    case LirOpcode::GlobalAddr: {
        const std::uint32_t symbol = hooks.ResolveGlobalSymbol(instruction.strArg);
        std::uint32_t relocationOffset;
        encoder.LeaRaxRip(relocationOffset);
        hooks.AddTextRelocation(relocationOffset, symbol);
        hooks.StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    case LirOpcode::StringAddr: {
        const TypeRef elementType = instruction.type.inner.empty() ? TypeRef::MakeChar8() : instruction.type.inner[0];
        const std::uint32_t symbol =
            hooks.InternStringLiteral(EncodeStringLiteral(instruction.strArg, SizeOfRuntime(elementType)));
        std::uint32_t relocationOffset;
        encoder.LeaRaxRip(relocationOffset);
        hooks.AddTextRelocation(relocationOffset, symbol);
        hooks.StoreA(instruction.dst, instruction.type);
        return true;
    }
    case LirOpcode::FieldPtr: {
        const LirReg base = instruction.srcs[0];
        hooks.LoadA(base, registerTypes.at(base));
        const int offset = FieldOffset(base, instruction.strArg);
        if (offset != 0) {
            encoder.LeaRaxRaxDisp(offset);
        }
        hooks.StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    case LirOpcode::IndexPtr: {
        const LirReg base = instruction.srcs[0];
        const LirReg index = instruction.srcs[1];
        int elementSize = instruction.type.kind == TypeRef::Kind::Pointer && !instruction.type.inner.empty()
                            ? SizeOfRuntime(instruction.type.inner[0])
                            : 8;
        if (elementSize < 1) {
            elementSize = 1;
        }
        hooks.LoadA(base, registerTypes.at(base));
        hooks.LoadB(index, registerTypes.at(index));
        encoder.ImulR11R10Imm32(elementSize);
        encoder.AddRaxR11();
        hooks.StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    default:
        return false;
    }
}
} // namespace Rux

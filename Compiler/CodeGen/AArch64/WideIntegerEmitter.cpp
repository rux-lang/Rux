#include "CodeGen/AArch64/FunctionEmitter.h"
#include "CodeGen/IntegerLiteral.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace Rux {
using namespace Layout;

namespace {
constexpr unsigned kLeft = 9;
constexpr unsigned kRight = 10;
constexpr unsigned kLow = 11;
constexpr unsigned kHigh = 12;
constexpr unsigned kValue = 13;
constexpr unsigned kCarry = 14;
constexpr unsigned kCount = 15;
} // namespace

void AArch64FunctionEmitter::LoadWideWord(const A64Reg destination, const LirReg value, const int word) {
    hooks.LoadScalar(destination, A64::Fp, static_cast<std::int64_t>(Disp(value)) + word * 8, 8, false);
}

void AArch64FunctionEmitter::LoadWideTemporaryWord(const A64Reg destination, const std::size_t temporary,
                                                   const int word) {
    hooks.LoadScalar(destination, A64::Fp,
                     static_cast<std::int64_t>(framePlan.WideTemporaryOffset(temporary)) + word * 8, 8, false);
}

void AArch64FunctionEmitter::StoreWideWord(const A64Reg value, const LirReg destination, const int word) {
    hooks.StoreScalar(value, A64::Fp, static_cast<std::int64_t>(Disp(destination)) + word * 8, 8);
}

void AArch64FunctionEmitter::StoreWideTemporaryWord(const A64Reg value, const std::size_t temporary, const int word) {
    hooks.StoreScalar(value, A64::Fp, static_cast<std::int64_t>(framePlan.WideTemporaryOffset(temporary)) + word * 8,
                      8);
}

std::uint32_t AArch64FunctionEmitter::BranchIf(const A64Condition condition) {
    const std::uint32_t site = encoder.Size();
    Must(encoder.BCond(condition, 0), "a wide-integer branch");
    return site;
}

std::uint32_t AArch64FunctionEmitter::Branch() {
    const std::uint32_t site = encoder.Size();
    Must(encoder.B(0), "a wide-integer branch");
    return site;
}

void AArch64FunctionEmitter::PatchConditionalBranch(const std::uint32_t site) {
    encoder.PatchField(site, 5, 19, (encoder.Size() - site) / A64Enc::InstrSize);
}

void AArch64FunctionEmitter::PatchBranch(const std::uint32_t site) {
    encoder.PatchField(site, 0, 26, (encoder.Size() - site) / A64Enc::InstrSize);
}

void AArch64FunctionEmitter::ZeroWide(const std::int32_t destination, const int size) {
    Must(encoder.LoadImm64(A64::Xn(kValue), 0), "a wide-integer zero");
    for (int offset = 0; offset < size; offset += 8) {
        hooks.StoreScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(destination) + offset, 8);
    }
}

void AArch64FunctionEmitter::CopyWide(const std::int32_t source, const std::int32_t destination, const int size) {
    if (source == destination) {
        return;
    }
    for (int offset = 0; offset < size; offset += 8) {
        hooks.LoadScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(source) + offset, 8, false);
        hooks.StoreScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(destination) + offset, 8);
    }
}

void AArch64FunctionEmitter::NegateWide(const std::int32_t value, const int size) {
    for (int offset = 0; offset < size; offset += 8) {
        hooks.LoadScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(value) + offset, 8, false);
        Must(offset == 0 ? encoder.Negs(A64::Xn(kValue), A64::Xn(kValue))
                         : encoder.Sbcs(A64::Xn(kValue), A64::Xzr, A64::Xn(kValue)),
             "a wide-integer negation");
        hooks.StoreScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(value) + offset, 8);
    }
}

void AArch64FunctionEmitter::MultiplyWide(const std::int32_t left, const std::int32_t right,
                                          const std::int32_t destination, const int size) {
    ZeroWide(destination, size);
    const int words = size / 8;
    for (int leftWord = 0; leftWord < words; ++leftWord) {
        Must(encoder.LoadImm64(A64::Xn(kCarry), 0), "a wide-integer multiplication carry");
        for (int rightWord = 0; rightWord < words - leftWord; ++rightWord) {
            hooks.LoadScalar(A64::Xn(kLeft), A64::Fp, static_cast<std::int64_t>(left) + leftWord * 8, 8, false);
            hooks.LoadScalar(A64::Xn(kRight), A64::Fp, static_cast<std::int64_t>(right) + rightWord * 8, 8, false);
            hooks.LoadScalar(A64::Xn(kValue), A64::Fp,
                             static_cast<std::int64_t>(destination) + (leftWord + rightWord) * 8, 8, false);
            Must(encoder.Mul(A64::Xn(kLow), A64::Xn(kLeft), A64::Xn(kRight)), "a wide-integer multiplication");
            Must(encoder.Umulh(A64::Xn(kHigh), A64::Xn(kLeft), A64::Xn(kRight)), "a wide-integer multiplication");
            Must(encoder.Adds(A64::Xn(kLow), A64::Xn(kLow), A64::Xn(kValue)), "a wide-integer multiplication");
            Must(encoder.Adc(A64::Xn(kHigh), A64::Xn(kHigh), A64::Xzr), "a wide-integer multiplication");
            Must(encoder.Adds(A64::Xn(kLow), A64::Xn(kLow), A64::Xn(kCarry)), "a wide-integer multiplication");
            Must(encoder.Adc(A64::Xn(kHigh), A64::Xn(kHigh), A64::Xzr), "a wide-integer multiplication");
            hooks.StoreScalar(A64::Xn(kLow), A64::Fp,
                              static_cast<std::int64_t>(destination) + (leftWord + rightWord) * 8, 8);
            Must(encoder.Mov(A64::Xn(kCarry), A64::Xn(kHigh)), "a wide-integer multiplication carry");
        }
    }
}

bool AArch64FunctionEmitter::EmitWideConstant(const LirInstr &instruction) {
    if (!IsWideInteger(instruction.type)) {
        return false;
    }
    const int size = RuntimeSize(instruction.type);
    const std::string &literal = instruction.strArg.empty() ? "0" : instruction.strArg;
    const auto parts = SplitIntegerLiteral(literal);
    WideInteger value = WideInteger::Zero(static_cast<std::uint32_t>(size * 8));
    if (parts) {
        if (const auto magnitude =
                WideInteger::Parse(parts->digits, parts->base, static_cast<std::uint32_t>(size * 8))) {
            value = parts->negative ? magnitude->Negated() : *magnitude;
        }
    }
    for (int word = 0; word < size / 8; ++word) {
        Must(encoder.LoadImm64(A64::Xn(kValue), value.Word64(static_cast<std::size_t>(word))),
             "a wide-integer constant");
        StoreWideWord(A64::Xn(kValue), instruction.dst, word);
    }
    return true;
}

bool AArch64FunctionEmitter::EmitWideArithmetic(const LirInstr &instruction) {
    if (EmitSoftwareFloatNegation(instruction)) {
        return true;
    }
    const TypeRef operandType = instruction.srcs.empty() ? instruction.type : TypeOfReg(instruction.srcs[0]);
    if (instruction.dst == LirNoReg || (!IsWideInteger(operandType) && !IsWideInteger(instruction.type))) {
        return false;
    }
    const TypeRef wideType = IsWideInteger(operandType) ? operandType : instruction.type;
    const int size = RuntimeSize(wideType);
    const int words = size / 8;
    const std::int32_t destination = instruction.dst == LirNoReg ? 0 : Disp(instruction.dst);
    const auto source = [&](const std::size_t index) { return Disp(instruction.srcs.at(index)); };

    switch (instruction.op) {
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor:
        for (int word = 0; word < words; ++word) {
            LoadWideWord(A64::Xn(kLeft), instruction.srcs[0], word);
            LoadWideWord(A64::Xn(kRight), instruction.srcs[1], word);
            A64Status status = A64Status::Ok;
            if (instruction.op == LirOpcode::Add) {
                status = word == 0 ? encoder.Adds(A64::Xn(kValue), A64::Xn(kLeft), A64::Xn(kRight))
                                   : encoder.Adcs(A64::Xn(kValue), A64::Xn(kLeft), A64::Xn(kRight));
            }
            else if (instruction.op == LirOpcode::Sub) {
                status = word == 0 ? encoder.Subs(A64::Xn(kValue), A64::Xn(kLeft), A64::Xn(kRight))
                                   : encoder.Sbcs(A64::Xn(kValue), A64::Xn(kLeft), A64::Xn(kRight));
            }
            else if (instruction.op == LirOpcode::And) {
                status = encoder.And(A64::Xn(kValue), A64::Xn(kLeft), A64::Xn(kRight));
            }
            else if (instruction.op == LirOpcode::Or) {
                status = encoder.Orr(A64::Xn(kValue), A64::Xn(kLeft), A64::Xn(kRight));
            }
            else {
                status = encoder.Eor(A64::Xn(kValue), A64::Xn(kLeft), A64::Xn(kRight));
            }
            Must(status, "a wide-integer operation");
            StoreWideWord(A64::Xn(kValue), instruction.dst, word);
        }
        return true;
    case LirOpcode::Mul:
        MultiplyWide(source(0), source(1), destination, size);
        return true;
    case LirOpcode::Div:
    case LirOpcode::Mod: {
        const std::int32_t remainder = framePlan.WideTemporaryOffset(0);
        const std::int32_t dividend = framePlan.WideTemporaryOffset(1);
        const std::int32_t divisor = framePlan.WideTemporaryOffset(2);

        Must(encoder.LoadImm64(A64::Xn(kValue), 0), "a wide-integer divisor test");
        for (int word = 0; word < words; ++word) {
            LoadWideWord(A64::Xn(kRight), instruction.srcs[1], word);
            Must(encoder.Orr(A64::Xn(kValue), A64::Xn(kValue), A64::Xn(kRight)), "a wide-integer divisor test");
        }
        Must(encoder.Cmp(A64::Xn(kValue), A64::Xzr), "a wide-integer divisor test");
        const std::uint32_t nonZero = BranchIf(A64Condition::Ne);
        Must(encoder.Brk(0), "a wide-integer division-by-zero trap");
        PatchConditionalBranch(nonZero);

        if (wideType.IsSigned()) {
            std::vector<std::uint32_t> notOverflow;
            LoadWideWord(A64::Xn(kLeft), instruction.srcs[0], words - 1);
            Must(encoder.LoadImm64(A64::Xn(kRight), std::numeric_limits<std::int64_t>::min()),
                 "the minimum wide integer");
            Must(encoder.Cmp(A64::Xn(kLeft), A64::Xn(kRight)), "a wide-integer overflow test");
            notOverflow.push_back(BranchIf(A64Condition::Ne));
            Must(encoder.LoadImm64(A64::Xn(kRight), 0), "a wide-integer overflow test");
            for (int word = 0; word < words - 1; ++word) {
                LoadWideWord(A64::Xn(kLeft), instruction.srcs[0], word);
                Must(encoder.Cmp(A64::Xn(kLeft), A64::Xn(kRight)), "a wide-integer overflow test");
                notOverflow.push_back(BranchIf(A64Condition::Ne));
            }
            Must(encoder.LoadImm64(A64::Xn(kRight), std::numeric_limits<std::uint64_t>::max()), "negative one");
            for (int word = 0; word < words; ++word) {
                LoadWideWord(A64::Xn(kLeft), instruction.srcs[1], word);
                Must(encoder.Cmp(A64::Xn(kLeft), A64::Xn(kRight)), "a wide-integer overflow test");
                notOverflow.push_back(BranchIf(A64Condition::Ne));
            }
            Must(encoder.Brk(0), "a wide-integer division-overflow trap");
            for (const std::uint32_t site : notOverflow) {
                PatchConditionalBranch(site);
            }
        }

        CopyWide(source(0), dividend, size);
        CopyWide(source(1), divisor, size);
        if (wideType.IsSigned()) {
            hooks.LoadScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(dividend) + size - 8, 8, false);
            Must(encoder.Cmp(A64::Xn(kValue), A64::Xzr), "a wide-integer sign test");
            const std::uint32_t dividendPositive = BranchIf(A64Condition::Pl);
            NegateWide(dividend, size);
            PatchConditionalBranch(dividendPositive);
            hooks.LoadScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(divisor) + size - 8, 8, false);
            Must(encoder.Cmp(A64::Xn(kValue), A64::Xzr), "a wide-integer sign test");
            const std::uint32_t divisorPositive = BranchIf(A64Condition::Pl);
            NegateWide(divisor, size);
            PatchConditionalBranch(divisorPositive);
        }

        ZeroWide(destination, size);
        ZeroWide(remainder, size);
        Must(encoder.LoadImm64(A64::Xn(kCount), static_cast<std::uint64_t>(size) * 8), "a wide-integer division count");
        const auto shiftLeft = [&](const std::int32_t value) {
            for (int word = words - 1; word > 0; --word) {
                hooks.LoadScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(value) + word * 8, 8, false);
                hooks.LoadScalar(A64::Xn(kRight), A64::Fp, static_cast<std::int64_t>(value) + (word - 1) * 8, 8, false);
                Must(encoder.Extr(A64::Xn(kValue), A64::Xn(kValue), A64::Xn(kRight), 63),
                     "a wide-integer division shift");
                hooks.StoreScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(value) + word * 8, 8);
            }
            hooks.LoadScalar(A64::Xn(kValue), A64::Fp, value, 8, false);
            Must(encoder.Lsl(A64::Xn(kValue), A64::Xn(kValue), 1), "a wide-integer division shift");
            hooks.StoreScalar(A64::Xn(kValue), A64::Fp, value, 8);
        };

        const std::uint32_t divideLoop = encoder.Size();
        shiftLeft(destination);
        hooks.LoadScalar(A64::Xn(kCarry), A64::Fp, static_cast<std::int64_t>(dividend) + size - 8, 8, false);
        Must(encoder.Lsr(A64::Xn(kCarry), A64::Xn(kCarry), 63), "a wide-integer dividend bit");
        shiftLeft(dividend);
        shiftLeft(remainder);
        hooks.LoadScalar(A64::Xn(kValue), A64::Fp, remainder, 8, false);
        Must(encoder.Orr(A64::Xn(kValue), A64::Xn(kValue), A64::Xn(kCarry)), "a wide-integer remainder bit");
        hooks.StoreScalar(A64::Xn(kValue), A64::Fp, remainder, 8);

        std::vector<std::uint32_t> remainderLess;
        std::vector<std::uint32_t> remainderGreater;
        for (int word = words - 1; word >= 0; --word) {
            hooks.LoadScalar(A64::Xn(kLeft), A64::Fp, static_cast<std::int64_t>(remainder) + word * 8, 8, false);
            hooks.LoadScalar(A64::Xn(kRight), A64::Fp, static_cast<std::int64_t>(divisor) + word * 8, 8, false);
            Must(encoder.Cmp(A64::Xn(kLeft), A64::Xn(kRight)), "a wide-integer division comparison");
            remainderLess.push_back(BranchIf(A64Condition::Cc));
            remainderGreater.push_back(BranchIf(A64Condition::Hi));
        }
        for (const std::uint32_t site : remainderGreater) {
            PatchConditionalBranch(site);
        }
        for (int word = 0; word < words; ++word) {
            hooks.LoadScalar(A64::Xn(kLeft), A64::Fp, static_cast<std::int64_t>(remainder) + word * 8, 8, false);
            hooks.LoadScalar(A64::Xn(kRight), A64::Fp, static_cast<std::int64_t>(divisor) + word * 8, 8, false);
            Must(word == 0 ? encoder.Subs(A64::Xn(kValue), A64::Xn(kLeft), A64::Xn(kRight))
                           : encoder.Sbcs(A64::Xn(kValue), A64::Xn(kLeft), A64::Xn(kRight)),
                 "a wide-integer division subtraction");
            hooks.StoreScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(remainder) + word * 8, 8);
        }
        hooks.LoadScalar(A64::Xn(kValue), A64::Fp, destination, 8, false);
        Must(encoder.OrrImm(A64::Xn(kValue), A64::Xn(kValue), 1), "a wide-integer quotient bit");
        hooks.StoreScalar(A64::Xn(kValue), A64::Fp, destination, 8);
        for (const std::uint32_t site : remainderLess) {
            PatchConditionalBranch(site);
        }
        Must(encoder.SubImm(A64::Xn(kCount), A64::Xn(kCount), 1), "a wide-integer division count");
        Must(encoder.Cbnz(A64::Xn(kCount), static_cast<std::int64_t>(divideLoop) - encoder.Size()),
             "a wide-integer division loop");

        if (instruction.op == LirOpcode::Mod) {
            CopyWide(remainder, destination, size);
            if (wideType.IsSigned()) {
                LoadWideWord(A64::Xn(kValue), instruction.srcs[0], words - 1);
                Must(encoder.Cmp(A64::Xn(kValue), A64::Xzr), "a wide-integer remainder sign");
                const std::uint32_t positive = BranchIf(A64Condition::Pl);
                NegateWide(destination, size);
                PatchConditionalBranch(positive);
            }
        }
        else if (wideType.IsSigned()) {
            LoadWideWord(A64::Xn(kLeft), instruction.srcs[0], words - 1);
            LoadWideWord(A64::Xn(kRight), instruction.srcs[1], words - 1);
            Must(encoder.Eor(A64::Xn(kValue), A64::Xn(kLeft), A64::Xn(kRight)), "a wide-integer quotient sign");
            Must(encoder.Cmp(A64::Xn(kValue), A64::Xzr), "a wide-integer quotient sign");
            const std::uint32_t positive = BranchIf(A64Condition::Pl);
            NegateWide(destination, size);
            PatchConditionalBranch(positive);
        }
        return true;
    }
    case LirOpcode::Neg:
        CopyWide(source(0), destination, size);
        NegateWide(destination, size);
        return true;
    case LirOpcode::BitNot:
        for (int word = 0; word < words; ++word) {
            LoadWideWord(A64::Xn(kValue), instruction.srcs[0], word);
            Must(encoder.Mvn(A64::Xn(kValue), A64::Xn(kValue)), "a wide-integer complement");
            StoreWideWord(A64::Xn(kValue), instruction.dst, word);
        }
        return true;
    case LirOpcode::Not: {
        Must(encoder.LoadImm64(A64::Xn(kValue), 0), "a wide-integer truth test");
        for (int word = 0; word < words; ++word) {
            LoadWideWord(A64::Xn(kRight), instruction.srcs[0], word);
            Must(encoder.Orr(A64::Xn(kValue), A64::Xn(kValue), A64::Xn(kRight)), "a wide-integer truth test");
        }
        Must(encoder.Cmp(A64::Xn(kValue), A64::Xzr), "a wide-integer truth test");
        Must(encoder.Cset(A64::Xn(kValue), A64Condition::Eq), "a wide-integer truth test");
        hooks.StoreToSlot(A64::Xn(kValue), instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr: {
        CopyWide(source(0), destination, size);
        const A64Reg amount = hooks.ReadOperand(instruction.srcs[1], TypeOfReg(instruction.srcs[1]), A64::Xn(kCount));
        Must(encoder.AndImm(A64::Xn(kCount), amount, static_cast<std::uint64_t>(size * 8 - 1)),
             "a wide-integer shift count");
        Must(encoder.Cmp(A64::Xn(kCount), A64::Xzr), "a wide-integer shift count");
        const std::uint32_t done = BranchIf(A64Condition::Eq);
        const std::uint32_t loop = encoder.Size();
        if (instruction.op == LirOpcode::Shl) {
            for (int word = words - 1; word > 0; --word) {
                hooks.LoadScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(destination) + word * 8, 8, false);
                hooks.LoadScalar(A64::Xn(kRight), A64::Fp, static_cast<std::int64_t>(destination) + (word - 1) * 8, 8,
                                 false);
                Must(encoder.Extr(A64::Xn(kValue), A64::Xn(kValue), A64::Xn(kRight), 63), "a wide-integer left shift");
                hooks.StoreScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(destination) + word * 8, 8);
            }
            hooks.LoadScalar(A64::Xn(kValue), A64::Fp, destination, 8, false);
            Must(encoder.Lsl(A64::Xn(kValue), A64::Xn(kValue), 1), "a wide-integer left shift");
            hooks.StoreScalar(A64::Xn(kValue), A64::Fp, destination, 8);
        }
        else {
            for (int word = 0; word < words - 1; ++word) {
                hooks.LoadScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(destination) + word * 8, 8, false);
                hooks.LoadScalar(A64::Xn(kRight), A64::Fp, static_cast<std::int64_t>(destination) + (word + 1) * 8, 8,
                                 false);
                Must(encoder.Extr(A64::Xn(kValue), A64::Xn(kRight), A64::Xn(kValue), 1), "a wide-integer right shift");
                hooks.StoreScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(destination) + word * 8, 8);
            }
            hooks.LoadScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(destination) + (words - 1) * 8, 8,
                             false);
            Must(instruction.op == LirOpcode::Shr && wideType.IsSigned()
                     ? encoder.Asr(A64::Xn(kValue), A64::Xn(kValue), 1)
                     : encoder.Lsr(A64::Xn(kValue), A64::Xn(kValue), 1),
                 "a wide-integer right shift");
            hooks.StoreScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(destination) + (words - 1) * 8, 8);
        }
        Must(encoder.SubImm(A64::Xn(kCount), A64::Xn(kCount), 1), "a wide-integer shift count");
        Must(encoder.Cbnz(A64::Xn(kCount), static_cast<std::int64_t>(loop) - encoder.Size()),
             "a wide-integer shift loop");
        PatchConditionalBranch(done);
        return true;
    }
    default:
        break;
    }

    if (instruction.op == LirOpcode::CmpEq || instruction.op == LirOpcode::CmpNe) {
        std::vector<std::uint32_t> differences;
        for (int word = 0; word < words; ++word) {
            LoadWideWord(A64::Xn(kLeft), instruction.srcs[0], word);
            LoadWideWord(A64::Xn(kRight), instruction.srcs[1], word);
            Must(encoder.Cmp(A64::Xn(kLeft), A64::Xn(kRight)), "a wide-integer comparison");
            differences.push_back(BranchIf(A64Condition::Ne));
        }
        Must(encoder.LoadImm64(A64::Xn(kValue), instruction.op == LirOpcode::CmpEq ? 1 : 0),
             "a wide-integer comparison result");
        const std::uint32_t done = Branch();
        for (const std::uint32_t difference : differences) {
            PatchConditionalBranch(difference);
        }
        Must(encoder.LoadImm64(A64::Xn(kValue), instruction.op == LirOpcode::CmpEq ? 0 : 1),
             "a wide-integer comparison result");
        PatchBranch(done);
        hooks.StoreToSlot(A64::Xn(kValue), instruction.dst, TypeRef::MakeBool());
        return true;
    }

    if (instruction.op == LirOpcode::CmpLt || instruction.op == LirOpcode::CmpLe ||
        instruction.op == LirOpcode::CmpGt || instruction.op == LirOpcode::CmpGe) {
        std::vector<std::uint32_t> less;
        std::vector<std::uint32_t> greater;
        for (int word = words - 1; word >= 0; --word) {
            LoadWideWord(A64::Xn(kLeft), instruction.srcs[0], word);
            LoadWideWord(A64::Xn(kRight), instruction.srcs[1], word);
            Must(encoder.Cmp(A64::Xn(kLeft), A64::Xn(kRight)), "a wide-integer comparison");
            const bool signedWord = word == words - 1 && wideType.IsSigned();
            less.push_back(BranchIf(signedWord ? A64Condition::Lt : A64Condition::Cc));
            greater.push_back(BranchIf(signedWord ? A64Condition::Gt : A64Condition::Hi));
        }
        const bool equalResult = instruction.op == LirOpcode::CmpLe || instruction.op == LirOpcode::CmpGe;
        Must(encoder.LoadImm64(A64::Xn(kValue), equalResult ? 1 : 0), "a wide-integer comparison result");
        const std::uint32_t equalDone = Branch();
        for (const std::uint32_t site : less) {
            PatchConditionalBranch(site);
        }
        const bool lessResult = instruction.op == LirOpcode::CmpLt || instruction.op == LirOpcode::CmpLe;
        Must(encoder.LoadImm64(A64::Xn(kValue), lessResult ? 1 : 0), "a wide-integer comparison result");
        const std::uint32_t lessDone = Branch();
        for (const std::uint32_t site : greater) {
            PatchConditionalBranch(site);
        }
        const bool greaterResult = instruction.op == LirOpcode::CmpGt || instruction.op == LirOpcode::CmpGe;
        Must(encoder.LoadImm64(A64::Xn(kValue), greaterResult ? 1 : 0), "a wide-integer comparison result");
        PatchBranch(equalDone);
        PatchBranch(lessDone);
        hooks.StoreToSlot(A64::Xn(kValue), instruction.dst, TypeRef::MakeBool());
        return true;
    }

    if (instruction.op == LirOpcode::Cast) {
        const TypeRef &destinationType = instruction.type;
        if (destinationType.IsBool()) {
            Must(encoder.LoadImm64(A64::Xn(kValue), 0), "a wide-integer truth conversion");
            for (int word = 0; word < words; ++word) {
                LoadWideWord(A64::Xn(kRight), instruction.srcs[0], word);
                Must(encoder.Orr(A64::Xn(kValue), A64::Xn(kValue), A64::Xn(kRight)), "a wide-integer truth conversion");
            }
            Must(encoder.Cmp(A64::Xn(kValue), A64::Xzr), "a wide-integer truth conversion");
            Must(encoder.Cset(A64::Xn(kValue), A64Condition::Ne), "a wide-integer truth conversion");
            hooks.StoreToSlot(A64::Xn(kValue), instruction.dst, destinationType);
            return true;
        }
        if (IsWideInteger(destinationType)) {
            const int destinationSize = RuntimeSize(destinationType);
            if (!IsWideInteger(operandType)) {
                const A64Reg value = hooks.ReadOperand(instruction.srcs[0], operandType, A64::Xn(kValue));
                hooks.StoreScalar(value, A64::Fp, destination, 8);
                if (operandType.IsSigned()) {
                    Must(encoder.Asr(A64::Xn(kValue), value, 63), "a wide-integer sign extension");
                }
                else {
                    Must(encoder.LoadImm64(A64::Xn(kValue), 0), "a wide-integer zero extension");
                }
                for (int offset = 8; offset < destinationSize; offset += 8) {
                    hooks.StoreScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(destination) + offset, 8);
                }
                return true;
            }
            const int copied = std::min(size, destinationSize);
            CopyWide(source(0), destination, copied);
            if (destinationSize > copied) {
                hooks.LoadScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(source(0)) + size - 8, 8, false);
                if (wideType.IsSigned()) {
                    Must(encoder.Asr(A64::Xn(kValue), A64::Xn(kValue), 63), "a wide-integer sign extension");
                }
                else {
                    Must(encoder.LoadImm64(A64::Xn(kValue), 0), "a wide-integer zero extension");
                }
                for (int offset = copied; offset < destinationSize; offset += 8) {
                    hooks.StoreScalar(A64::Xn(kValue), A64::Fp, static_cast<std::int64_t>(destination) + offset, 8);
                }
            }
            return true;
        }
        LoadWideWord(A64::Xn(kValue), instruction.srcs[0], 0);
        hooks.StoreToSlot(A64::Xn(kValue), instruction.dst, destinationType);
        return true;
    }

    return false;
}
} // namespace Rux

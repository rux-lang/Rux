#include "Lowering/HirToLir/CheckedLirBuilder.h"

#include <format>
#include <utility>

namespace Rux {
CheckedLirBuilder::CheckedLirBuilder(LirFunc &inputFunction)
    : function(&inputFunction) {
}

LirReg CheckedLirBuilder::AllocateRegister() {
    return nextRegister++;
}

LirReg CheckedLirBuilder::DefineParameter() {
    const LirReg reg = AllocateRegister();
    definedRegisters.insert(reg);
    return reg;
}

CheckedLirBuilder::BlockIndex CheckedLirBuilder::CreateBlock(std::string label) {
    if (label.empty()) {
        label = std::format("bb{}", function->blocks.size());
    }
    function->blocks.push_back({std::move(label), {}, std::nullopt});
    return static_cast<BlockIndex>(function->blocks.size() - 1);
}

bool CheckedLirBuilder::SelectBlock(const BlockIndex index) {
    if (index >= function->blocks.size()) {
        failureReason = "block index is out of range";
        return false;
    }
    failureReason.clear();
    currentBlock = index;
    return true;
}

bool CheckedLirBuilder::IsTerminated() const noexcept {
    return currentBlock && function->blocks[*currentBlock].term.has_value();
}

CheckedLirBuilder::BlockIndex CheckedLirBuilder::CurrentBlock() const noexcept {
    if (!currentBlock) {
        std::unreachable();
    }
    return *currentBlock;
}

LirFunc &CheckedLirBuilder::Function() const noexcept {
    return *function;
}

std::string_view CheckedLirBuilder::FailureReason() const noexcept {
    return failureReason;
}

bool CheckedLirBuilder::IsDefined(const LirReg reg) const noexcept {
    return reg != LirNoReg && definedRegisters.contains(reg);
}

bool CheckedLirBuilder::HasValidSources(const LirInstr &instruction) const noexcept {
    for (const LirReg source : instruction.srcs) {
        if (!IsDefined(source)) {
            return false;
        }
    }
    for (const auto &[source, predecessor] : instruction.phiPreds) {
        static_cast<void>(predecessor);
        if (!IsDefined(source)) {
            return false;
        }
    }
    return true;
}

bool CheckedLirBuilder::HasValidSources(const LirTerminator &terminator) const noexcept {
    switch (terminator.kind) {
    case LirTermKind::Jump:
    case LirTermKind::Unreachable:
        return true;
    case LirTermKind::Branch:
    case LirTermKind::Switch:
        return IsDefined(terminator.cond);
    case LirTermKind::Return:
        return !terminator.retVal || IsDefined(*terminator.retVal);
    }
    std::unreachable();
}

bool CheckedLirBuilder::HasValidTargets(const LirTerminator &terminator) const noexcept {
    const auto valid = [this](const BlockIndex target) { return target < function->blocks.size(); };
    switch (terminator.kind) {
    case LirTermKind::Jump:
        return valid(terminator.trueTarget);
    case LirTermKind::Branch:
        return valid(terminator.trueTarget) && valid(terminator.falseTarget);
    case LirTermKind::Switch:
        if (!valid(terminator.defaultTarget)) {
            return false;
        }
        for (const auto &switchCase : terminator.cases) {
            if (!valid(switchCase.target)) {
                return false;
            }
        }
        return true;
    case LirTermKind::Return:
    case LirTermKind::Unreachable:
        return true;
    }
    std::unreachable();
}

bool CheckedLirBuilder::Insert(LirInstr instruction) {
    if (!currentBlock) {
        failureReason = "no current block is selected";
        return false;
    }
    if (IsTerminated()) {
        failureReason = "the current block already has a terminator";
        return false;
    }
    if (!HasValidSources(instruction)) {
        failureReason = "the instruction uses an undefined register";
        return false;
    }
    if (instruction.dst != LirNoReg &&
        (instruction.dst >= nextRegister || definedRegisters.contains(instruction.dst))) {
        failureReason = "the instruction defines an invalid or previously defined register";
        return false;
    }
    if (instruction.dst != LirNoReg) {
        definedRegisters.insert(instruction.dst);
    }
    function->blocks[*currentBlock].instrs.push_back(std::move(instruction));
    failureReason.clear();
    return true;
}

bool CheckedLirBuilder::Terminate(LirTerminator terminator) {
    if (!currentBlock) {
        failureReason = "no current block is selected";
        return false;
    }
    if (IsTerminated()) {
        failureReason = "the current block already has a terminator";
        return false;
    }
    if (!HasValidSources(terminator)) {
        failureReason = "the terminator uses an undefined register";
        return false;
    }
    if (!HasValidTargets(terminator)) {
        failureReason = "the terminator targets an invalid block";
        return false;
    }
    function->blocks[*currentBlock].term = std::move(terminator);
    failureReason.clear();
    return true;
}

std::optional<LirOpcode> CheckedLirBuilder::BinaryOpcode(const TokenKind op) noexcept {
    using TK = TokenKind;
    switch (op) {
    case TK::Plus:
        return LirOpcode::Add;
    case TK::Minus:
        return LirOpcode::Sub;
    case TK::Star:
        return LirOpcode::Mul;
    case TK::Slash:
        return LirOpcode::Div;
    case TK::Percent:
        return LirOpcode::Mod;
    case TK::StarStar:
        return LirOpcode::Pow;
    case TK::Amp:
    case TK::AmpAmp:
        return LirOpcode::And;
    case TK::Pipe:
    case TK::PipePipe:
        return LirOpcode::Or;
    case TK::Caret:
        return LirOpcode::Xor;
    case TK::LessLess:
        return LirOpcode::Shl;
    case TK::GreaterGreater:
        return LirOpcode::Shr;
    case TK::GreaterGreaterGreater:
        return LirOpcode::Lshr;
    case TK::Equal:
        return LirOpcode::CmpEq;
    case TK::BangEqual:
        return LirOpcode::CmpNe;
    case TK::Less:
        return LirOpcode::CmpLt;
    case TK::LessEqual:
        return LirOpcode::CmpLe;
    case TK::Greater:
        return LirOpcode::CmpGt;
    case TK::GreaterEqual:
        return LirOpcode::CmpGe;
    default:
        return std::nullopt;
    }
}

std::optional<LirOpcode> CheckedLirBuilder::CompoundOpcode(const TokenKind op) noexcept {
    using TK = TokenKind;
    switch (op) {
    case TK::PlusAssign:
        return LirOpcode::Add;
    case TK::MinusAssign:
        return LirOpcode::Sub;
    case TK::StarAssign:
        return LirOpcode::Mul;
    case TK::SlashAssign:
        return LirOpcode::Div;
    case TK::PercentAssign:
        return LirOpcode::Mod;
    case TK::AmpAssign:
        return LirOpcode::And;
    case TK::PipeAssign:
        return LirOpcode::Or;
    case TK::CaretAssign:
        return LirOpcode::Xor;
    case TK::LessLessAssign:
        return LirOpcode::Shl;
    case TK::GreaterGreaterAssign:
        return LirOpcode::Shr;
    case TK::GreaterGreaterGreaterAssign:
        return LirOpcode::Lshr;
    default:
        return std::nullopt;
    }
}
} // namespace Rux

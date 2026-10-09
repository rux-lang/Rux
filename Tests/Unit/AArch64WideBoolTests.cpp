// AArch64 code generation for bool128, bool256 and bool512.
//
// This host cannot execute AArch64 code, so each case runs the emitted function through a small interpreter of the
// instructions a frame and the wide-scalar paths use, then reads back the value it returned. Memory starts filled with
// a non-zero pattern, so a word the code never wrote cannot pass for one it zeroed.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/Layout.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <doctest.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

namespace {
constexpr std::uint64_t kReturnAddress = 0xFFFF'0000'0000'0000ULL;
constexpr std::size_t kStackBytes = 1U << 16U;
constexpr std::uint64_t kResultAddress = 64;
constexpr std::uint8_t kUnwritten = 0xA5;

[[nodiscard]] std::uint64_t Field(const std::uint32_t word, const unsigned low, const unsigned count) {
    return (word >> low) & ((1ULL << count) - 1);
}

[[nodiscard]] std::int64_t SignedField(const std::uint32_t word, const unsigned low, const unsigned count) {
    const std::uint64_t sign = 1ULL << (count - 1);
    return static_cast<std::int64_t>((Field(word, low, count) ^ sign) - sign);
}

[[nodiscard]] std::uint64_t LowMask(const unsigned bits) {
    return bits >= 64 ? ~0ULL : (1ULL << bits) - 1;
}

// DecodeBitMasks from the Arm ARM, for the logical-immediate forms.
[[nodiscard]] std::uint64_t BitMask(const unsigned n, const unsigned immr, const unsigned imms, const unsigned width) {
    const auto length = static_cast<unsigned>(std::bit_width((n << 6U) | (~imms & 0x3FU))) - 1;
    const unsigned size = 1U << length;
    const unsigned rotate = immr & (size - 1);
    std::uint64_t element = LowMask((imms & (size - 1)) + 1);
    if (rotate != 0) {
        element = ((element >> rotate) | (element << (size - rotate))) & LowMask(size);
    }
    std::uint64_t mask = 0;
    for (unsigned bit = 0; bit < width; bit += size) {
        mask |= element << bit;
    }
    return mask;
}

/// Just enough of AArch64 to run what these cases emit: no calls, no vector registers, and a trap or an instruction it
/// does not know ends the run with a failure that names the word. A result returned in memory goes to the bytes at
/// kResultAddress, below the stack.
class Machine {
public:
    explicit Machine(std::vector<std::uint32_t> code)
        : code(std::move(code))
        , memory(kStackBytes, kUnwritten) {
        registers[8] = kResultAddress;
        registers[30] = kReturnAddress;
    }

    /// Runs to the return to the caller, and says whether it got there.
    [[nodiscard]] bool Run() {
        for (int steps = 0; steps < 100000; ++steps) {
            if (!Step()) {
                return failure.empty();
            }
        }
        failure = "the step limit was reached";
        return false;
    }

    [[nodiscard]] const std::string &Failure() const {
        return failure;
    }

    [[nodiscard]] std::uint64_t Register(const unsigned reg) const {
        return registers.at(reg);
    }

    [[nodiscard]] std::uint64_t Load(const std::uint64_t address, const unsigned bytes) {
        if (address + bytes > memory.size()) {
            failure = "an access outside the stack";
            return 0;
        }
        std::uint64_t value = 0;
        for (unsigned index = 0; index < bytes; ++index) {
            value |= static_cast<std::uint64_t>(memory[address + index]) << (8 * index);
        }
        return value;
    }

private:
    std::vector<std::uint32_t> code;
    std::vector<std::uint8_t> memory;
    std::array<std::uint64_t, 31> registers{};
    std::uint64_t sp = kStackBytes;
    std::size_t pc = 0;
    bool n = false;
    bool z = false;
    bool c = false;
    bool v = false;
    std::string failure;

    // Register 31 is SP in an address or an immediate add, and the zero register everywhere else.
    [[nodiscard]] std::uint64_t Read(const unsigned reg, const bool stackForm = false) const {
        return reg == 31 ? (stackForm ? sp : 0) : registers[reg];
    }

    void Write(const unsigned reg, const std::uint64_t value, const unsigned width, const bool stackForm = false) {
        const std::uint64_t masked = value & LowMask(width);
        if (reg != 31) {
            registers[reg] = masked;
        }
        else if (stackForm) {
            sp = masked;
        }
    }

    void Store(const std::uint64_t address, const std::uint64_t value, const unsigned bytes) {
        if (address + bytes > memory.size()) {
            failure = "an access outside the stack";
            return;
        }
        for (unsigned index = 0; index < bytes; ++index) {
            memory[address + index] = static_cast<std::uint8_t>(value >> (8 * index));
        }
    }

    [[nodiscard]] std::uint64_t AddWithCarry(std::uint64_t left, std::uint64_t right, const bool carry,
                                             const unsigned width, const bool setFlags) {
        left &= LowMask(width);
        right &= LowMask(width);
        const std::uint64_t result = (left + right + (carry ? 1 : 0)) & LowMask(width);
        if (setFlags) {
            const std::uint64_t top = 1ULL << (width - 1);
            n = (result & top) != 0;
            z = result == 0;
            c = width == 64 ? result < left || (carry && result == left)
                            : ((left + right + (carry ? 1 : 0)) >> 32) != 0;
            v = ((left ^ result) & (right ^ result) & top) != 0;
        }
        return result;
    }

    [[nodiscard]] bool Holds(const unsigned condition) const {
        bool result = false;
        switch (condition >> 1U) {
        case 0:
            result = z;
            break;
        case 1:
            result = c;
            break;
        case 2:
            result = n;
            break;
        case 3:
            result = v;
            break;
        case 4:
            result = c && !z;
            break;
        case 5:
            result = n == v;
            break;
        case 6:
            result = !z && n == v;
            break;
        default:
            return true;
        }
        return (condition & 1U) != 0 ? !result : result;
    }

    [[nodiscard]] static std::uint64_t Shifted(const std::uint64_t value, const unsigned kind, const unsigned amount,
                                               const unsigned width) {
        const std::uint64_t masked = value & LowMask(width);
        if (amount == 0) {
            return masked;
        }
        switch (kind) {
        case 0:
            return (masked << amount) & LowMask(width);
        case 1:
            return masked >> amount;
        case 2: {
            const bool negative = ((masked >> (width - 1)) & 1U) != 0;
            return ((masked >> amount) | (negative ? ~LowMask(width - amount) : 0)) & LowMask(width);
        }
        default:
            return ((masked >> amount) | (masked << (width - amount))) & LowMask(width);
        }
    }

    // A load or store of one general register; `offset` is already scaled.
    void Access(const std::uint32_t word, const std::int64_t offset) {
        const auto bytes = 1U << Field(word, 30, 2);
        const auto opc = static_cast<unsigned>(Field(word, 22, 2));
        const unsigned rt = word & 31U;
        const std::uint64_t address = Read((word >> 5U) & 31U, true) + static_cast<std::uint64_t>(offset);
        if (opc == 0) {
            Store(address, Read(rt), bytes);
            return;
        }
        if (bytes == 8 && opc != 1) {
            failure = "a prefetch";
            return;
        }
        std::uint64_t value = Load(address, bytes);
        if (opc != 1 && ((value >> (8 * bytes - 1)) & 1U) != 0) {
            value |= ~LowMask(8 * bytes);
        }
        Write(rt, value, opc == 3 ? 32 : 64);
    }

    [[nodiscard]] bool Step() {
        if (pc >= code.size()) {
            failure = "execution left the function";
            return false;
        }
        const std::uint32_t word = code[pc];
        std::size_t next = pc + 1;
        const unsigned width = (word >> 31U) != 0 ? 64 : 32;
        const unsigned rd = word & 31U;
        const unsigned rn = (word >> 5U) & 31U;
        const unsigned rm = (word >> 16U) & 31U;
        const auto opc = static_cast<unsigned>(Field(word, 29, 2));

        if (word == 0xD65F03C0U) {
            if (registers[30] != kReturnAddress) {
                failure = "a return to somewhere other than the caller";
            }
            return false;
        }
        if ((word & 0xFFFFF01FU) == 0xD503201FU) {
            // A hint, NOP included.
        }
        else if ((word & 0xFE000000U) == 0xA8000000U && Field(word, 23, 2) != 0) {
            const auto mode = static_cast<unsigned>(Field(word, 23, 2));
            const std::int64_t immediate = SignedField(word, 15, 7) * 8;
            const std::uint64_t base = Read(rn, true);
            const std::uint64_t address = mode == 1 ? base : base + static_cast<std::uint64_t>(immediate);
            const unsigned second = (word >> 10U) & 31U;
            if ((word & (1U << 22U)) != 0) {
                const std::uint64_t first = Load(address, 8);
                Write(second, Load(address + 8, 8), 64);
                Write(rd, first, 64);
            }
            else {
                Store(address, Read(rd), 8);
                Store(address + 8, Read(second), 8);
            }
            if (mode != 2) {
                Write(rn, base + static_cast<std::uint64_t>(immediate), 64, true);
            }
        }
        else if ((word & 0x3F000000U) == 0x39000000U) {
            Access(word, static_cast<std::int64_t>(Field(word, 10, 12) << Field(word, 30, 2)));
        }
        else if ((word & 0x3F200C00U) == 0x38000000U) {
            Access(word, SignedField(word, 12, 9));
        }
        else if ((word & 0x1F800000U) == 0x12800000U && opc != 1) {
            const auto shift = static_cast<unsigned>(Field(word, 21, 2) * 16);
            const std::uint64_t immediate = Field(word, 5, 16) << shift;
            if (opc == 0) {
                Write(rd, ~immediate, width);
            }
            else if (opc == 2) {
                Write(rd, immediate, width);
            }
            else {
                Write(rd, (Read(rd) & ~(0xFFFFULL << shift)) | immediate, width);
            }
        }
        else if ((word & 0x1F800000U) == 0x12000000U || (word & 0x1F000000U) == 0x0A000000U) {
            const bool immediateForm = (word & 0x1F800000U) == 0x12000000U;
            std::uint64_t right = 0;
            if (immediateForm) {
                right = BitMask((word >> 22U) & 1U, static_cast<unsigned>(Field(word, 16, 6)),
                                static_cast<unsigned>(Field(word, 10, 6)), width);
            }
            else {
                right = Shifted(Read(rm), static_cast<unsigned>(Field(word, 22, 2)),
                                static_cast<unsigned>(Field(word, 10, 6)), width);
                right = (word & (1U << 21U)) != 0 ? ~right : right;
            }
            const std::uint64_t left = Read(rn);
            const std::uint64_t result = (opc == 1   ? left | right
                                          : opc == 2 ? left ^ right
                                                     : left & right) &
                                         LowMask(width);
            if (opc == 3) {
                n = ((result >> (width - 1)) & 1U) != 0;
                z = result == 0;
                c = false;
                v = false;
            }
            Write(rd, result, width, immediateForm && opc != 3);
        }
        else if ((word & 0x1F800000U) == 0x11000000U || (word & 0x1F200000U) == 0x0B000000U ||
                 (word & 0x1FE00000U) == 0x0B200000U) {
            const bool subtract = (opc & 2U) != 0;
            const bool setFlags = (opc & 1U) != 0;
            const bool shiftedForm = (word & 0x1F200000U) == 0x0B000000U;
            std::uint64_t right = 0;
            if ((word & 0x1F800000U) == 0x11000000U) {
                right = Field(word, 10, 12) << ((word & (1U << 22U)) != 0 ? 12 : 0);
            }
            else if (shiftedForm) {
                right = Shifted(Read(rm), static_cast<unsigned>(Field(word, 22, 2)),
                                static_cast<unsigned>(Field(word, 10, 6)), width);
            }
            else {
                const auto option = static_cast<unsigned>(Field(word, 13, 3));
                const unsigned bits = 8U << (option & 3U);
                right = Read(rm) & LowMask(bits);
                if ((option & 4U) != 0 && ((right >> (bits - 1)) & 1U) != 0) {
                    right |= ~LowMask(bits);
                }
                right <<= Field(word, 10, 3);
            }
            const std::uint64_t result =
                AddWithCarry(Read(rn, !shiftedForm), subtract ? ~right : right, subtract, width, setFlags);
            Write(rd, result, width, !shiftedForm && !setFlags);
        }
        else if ((word & 0x1FE00000U) == 0x1A800000U) {
            std::uint64_t result = Read(rn);
            if (!Holds(static_cast<unsigned>(Field(word, 12, 4)))) {
                result = Read(rm);
                result = (word & (1U << 30U)) != 0 ? ~result : result;
                result += (word & (1U << 10U)) != 0 ? 1 : 0;
            }
            Write(rd, result, width);
        }
        else if ((word & 0x1F800000U) == 0x13000000U && opc != 1 && opc != 3) {
            const auto immr = static_cast<unsigned>(Field(word, 16, 6));
            const auto imms = static_cast<unsigned>(Field(word, 10, 6));
            const std::uint64_t source = Read(rn) & LowMask(width);
            std::uint64_t result = 0;
            unsigned top = 0;
            if (imms >= immr) {
                result = (source >> immr) & LowMask(imms - immr + 1);
                top = imms - immr;
            }
            else {
                result = (source & LowMask(imms + 1)) << (width - immr);
                top = width - immr + imms;
            }
            if (opc == 0 && ((result >> top) & 1U) != 0) {
                result |= ~LowMask(top + 1);
            }
            Write(rd, result, width);
        }
        else if ((word & 0xFC000000U) == 0x14000000U) {
            next = pc + static_cast<std::size_t>(SignedField(word, 0, 26));
        }
        else if ((word & 0xFF000010U) == 0x54000000U) {
            if (Holds(word & 15U)) {
                next = pc + static_cast<std::size_t>(SignedField(word, 5, 19));
            }
        }
        else if ((word & 0x7E000000U) == 0x34000000U) {
            const bool nonZero = (word & (1U << 24U)) != 0;
            if (((Read(rd) & LowMask(width)) != 0) == nonZero) {
                next = pc + static_cast<std::size_t>(SignedField(word, 5, 19));
            }
        }
        else if ((word & 0xFFE0001FU) == 0xD4200000U) {
            failure = "a trap";
        }
        else {
            failure = "an instruction this interpreter does not know: " + HexWord(word);
        }
        pc = next;
        return failure.empty();
    }
};

// LIR builders

[[nodiscard]] TypeRef WideBool(const int bits) {
    return TypeRef::MakePrimitive(bits == 128   ? TypeRef::Kind::Bool128
                                  : bits == 256 ? TypeRef::Kind::Bool256
                                                : TypeRef::Kind::Bool512);
}

[[nodiscard]] LirInstr Operation(const LirOpcode opcode, const LirReg destination, const TypeRef &type,
                                 std::vector<LirReg> sources) {
    LirInstr instruction;
    instruction.op = opcode;
    instruction.dst = destination;
    instruction.type = type;
    instruction.srcs = std::move(sources);
    return instruction;
}

[[nodiscard]] LirInstr Cast(const LirReg destination, const TypeRef &type, const LirReg source, const TypeRef &from) {
    LirInstr instruction = Operation(LirOpcode::Cast, destination, type, {source});
    instruction.strArg = from.ToString();
    return instruction;
}

[[nodiscard]] LirBlock Returning(std::vector<LirInstr> instructions, const LirReg value, const TypeRef &type) {
    LirBlock block;
    block.instrs = std::move(instructions);
    block.term = ReturnTerm(value);
    block.term->retType = type;
    return block;
}

/// What a function returns, as little-endian words: x0 for a narrow value, x0 and x1 for a 16-byte one, and the
/// indirect result for anything wider. The function is emitted and run once for each AArch64 profile, and every run
/// must agree.
[[nodiscard]] std::vector<std::uint64_t> Returned(std::vector<LirBlock> blocks, const TypeRef &type) {
    LirFunc function;
    function.name = "Exercise";
    function.returnType = type;
    function.blocks = std::move(blocks);
    const LirPackage package = PackageOf(std::move(function));
    const int size = Layout::SizeOf(type);
    std::optional<std::vector<std::uint64_t>> first;
    for (const Target::OS os : {Target::OS::Linux, Target::OS::MacOS, Target::OS::Windows}) {
        CAPTURE(static_cast<int>(os));
        AArch64RcuEmitter emitter(package, "WideBool", os);
        const auto objects = emitter.Generate();
        REQUIRE_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
        REQUIRE_EQ(objects.size(), 1);
        Machine machine(FunctionWords(objects.front(), "Exercise"));
        REQUIRE_MESSAGE(machine.Run(), machine.Failure());
        std::vector<std::uint64_t> words;
        if (size <= 8) {
            words.push_back(machine.Register(0) & ((size == 8 ? 0ULL : 1ULL << (size * 8)) - 1));
        }
        else if (size == 16) {
            words = {machine.Register(0), machine.Register(1)};
        }
        else {
            for (int word = 0; word < size / 8; ++word) {
                words.push_back(machine.Load(kResultAddress + static_cast<std::uint64_t>(word) * 8, 8));
            }
        }
        if (first) {
            CHECK(words == *first);
        }
        else {
            first = std::move(words);
        }
    }
    return *first;
}

/// What one block of instructions leaves in `result`.
[[nodiscard]] std::vector<std::uint64_t> Evaluate(std::vector<LirInstr> instructions, const LirReg result,
                                                  const TypeRef &type) {
    std::vector<LirBlock> blocks;
    blocks.push_back(Returning(std::move(instructions), result, type));
    return Returned(std::move(blocks), type);
}

/// A binary operation on two wide constants of the same width.
[[nodiscard]] std::vector<std::uint64_t> Combine(const LirOpcode opcode, const TypeRef &operand, const char *left,
                                                 const char *right, const TypeRef &result) {
    return Evaluate({ConstInstr(0, left, operand), ConstInstr(1, right, operand), Operation(opcode, 2, result, {0, 1})},
                    2, result);
}

[[nodiscard]] std::vector<std::uint64_t> Canonical(const int bits, const bool value) {
    std::vector<std::uint64_t> words(static_cast<std::size_t>(bits / 64), 0);
    words[0] = value ? 1 : 0;
    return words;
}

[[nodiscard]] std::vector<std::uint64_t> Narrow(const bool value) {
    return {value ? 1ULL : 0ULL};
}

constexpr std::array kWidths = {128, 256, 512};
} // namespace

TEST_CASE("AArch64 materializes wide bool constants as one in the low word and zeros above it") {
    for (const int bits : kWidths) {
        CAPTURE(bits);
        const TypeRef type = WideBool(bits);
        CHECK(Evaluate({ConstInstr(0, "true", type)}, 0, type) == Canonical(bits, true));
        CHECK(Evaluate({ConstInstr(0, "false", type)}, 0, type) == Canonical(bits, false));
        CHECK(Evaluate({ConstInstr(0, "1", type)}, 0, type) == Canonical(bits, true));
    }
}

TEST_CASE("AArch64 keeps wide bool logic canonical, complement included") {
    for (const int bits : kWidths) {
        CAPTURE(bits);
        const TypeRef type = WideBool(bits);
        CHECK(Combine(LirOpcode::And, type, "true", "true", type) == Canonical(bits, true));
        CHECK(Combine(LirOpcode::And, type, "true", "false", type) == Canonical(bits, false));
        CHECK(Combine(LirOpcode::Or, type, "false", "true", type) == Canonical(bits, true));
        CHECK(Combine(LirOpcode::Or, type, "false", "false", type) == Canonical(bits, false));
        CHECK(Combine(LirOpcode::Xor, type, "true", "false", type) == Canonical(bits, true));
        CHECK(Combine(LirOpcode::Xor, type, "true", "true", type) == Canonical(bits, false));
        for (const bool value : {true, false}) {
            CAPTURE(value);
            CHECK(Evaluate({ConstInstr(0, value ? "true" : "false", type), Operation(LirOpcode::BitNot, 1, type, {0})},
                           1, type) == Canonical(bits, !value));
        }
    }
}

TEST_CASE("AArch64 negates and compares wide bools into a narrow bool") {
    const TypeRef narrow = TypeRef::MakeBool();
    for (const int bits : kWidths) {
        CAPTURE(bits);
        const TypeRef type = WideBool(bits);
        for (const bool value : {true, false}) {
            CAPTURE(value);
            CHECK(Evaluate({ConstInstr(0, value ? "true" : "false", type), Operation(LirOpcode::Not, 1, narrow, {0})},
                           1, narrow) == Narrow(!value));
        }
        CHECK(Combine(LirOpcode::CmpEq, type, "true", "true", narrow) == Narrow(true));
        CHECK(Combine(LirOpcode::CmpEq, type, "true", "false", narrow) == Narrow(false));
        CHECK(Combine(LirOpcode::CmpNe, type, "false", "true", narrow) == Narrow(true));
        CHECK(Combine(LirOpcode::CmpNe, type, "false", "false", narrow) == Narrow(false));
    }
}

TEST_CASE("AArch64 casts into a wide bool read only the source's words and zero the rest") {
    const TypeRef narrow = TypeRef::MakeBool();
    for (const int bits : kWidths) {
        CAPTURE(bits);
        const TypeRef type = WideBool(bits);
        for (const bool value : {true, false}) {
            CAPTURE(value);
            CHECK(Evaluate({ConstInstr(0, value ? "true" : "false", narrow), Cast(1, type, 0, narrow)}, 1, type) ==
                  Canonical(bits, value));
            for (const int other : kWidths) {
                CAPTURE(other);
                const TypeRef source = WideBool(other);
                CHECK(Evaluate({ConstInstr(0, value ? "true" : "false", source), Cast(1, type, 0, source)}, 1, type) ==
                      Canonical(bits, value));
            }
        }
        // Only the second word is set, so a truth test that stopped at the low word would call this false.
        const TypeRef uint128 = TypeRef::MakePrimitive(TypeRef::Kind::UInt128);
        CHECK(Evaluate({ConstInstr(0, "18446744073709551616", uint128), Cast(1, type, 0, uint128)}, 1, type) ==
              Canonical(bits, true));
    }
}

TEST_CASE("AArch64 casts out of a wide bool as an unsigned value") {
    const TypeRef uint512 = TypeRef::MakePrimitive(TypeRef::Kind::UInt512);
    const TypeRef int128 = TypeRef::MakePrimitive(TypeRef::Kind::Int128);
    for (const int bits : kWidths) {
        for (const bool value : {true, false}) {
            CAPTURE(bits);
            CAPTURE(value);
            const TypeRef type = WideBool(bits);
            const auto cast = [&](const TypeRef &target) {
                return Evaluate({ConstInstr(0, value ? "true" : "false", type), Cast(1, target, 0, type)}, 1, target);
            };
            CHECK(cast(TypeRef::MakeBool()) == Narrow(value));
            CHECK(cast(TypeRef::MakeUInt64()) == Narrow(value));
            CHECK(cast(uint512) == Canonical(512, value));
            CHECK(cast(int128) == Canonical(128, value));
        }
    }
}

TEST_CASE("AArch64 branches on a wide bool's low word") {
    const TypeRef uint64 = TypeRef::MakeUInt64();
    for (const int bits : kWidths) {
        for (const bool value : {true, false}) {
            CAPTURE(bits);
            CAPTURE(value);
            std::vector<LirBlock> blocks;
            LirBlock entry;
            entry.instrs = {ConstInstr(0, value ? "true" : "false", WideBool(bits))};
            entry.term.emplace();
            entry.term->kind = LirTermKind::Branch;
            entry.term->cond = 0;
            entry.term->trueTarget = 1;
            entry.term->falseTarget = 2;
            blocks.push_back(std::move(entry));
            blocks.push_back(Returning({ConstInstr(1, "1", uint64)}, 1, uint64));
            blocks.push_back(Returning({ConstInstr(2, "2", uint64)}, 2, uint64));
            CHECK(Returned(std::move(blocks), uint64) == std::vector<std::uint64_t>{value ? 1ULL : 2ULL});
        }
    }
}

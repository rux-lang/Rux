// The x86-64 assembly text against the object code it describes.
//
// `--emit asm` is meant to read as what the RCU emitter writes, instruction for instruction and on the same frame
// addresses. Nothing assembles the text back, so the agreement is checked two ways: exact text for the lowerings that
// have their own rules (wide values passed by address, software-float constants and negation, the NaN-aware SSE
// comparisons, word-by-word wide integers), and, over a broad set of functions, the sequence of rbp-relative addresses
// each side uses, which only agrees when every instruction that touches the frame does so in the same order.

#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Object/Rcu/Rcu.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <doctest.h>
#include <format>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
const TypeRef Float16 = TypeRef::MakePrimitive(TypeRef::Kind::Float16);
const TypeRef Float80 = TypeRef::MakePrimitive(TypeRef::Kind::Float80);
const TypeRef Float128 = TypeRef::MakePrimitive(TypeRef::Kind::Float128);
const TypeRef Float256 = TypeRef::MakePrimitive(TypeRef::Kind::Float256);
const TypeRef Int128 = TypeRef::MakePrimitive(TypeRef::Kind::Int128);
const TypeRef UInt128 = TypeRef::MakePrimitive(TypeRef::Kind::UInt128);
const TypeRef Int256 = TypeRef::MakePrimitive(TypeRef::Kind::Int256);
const TypeRef Bool256 = TypeRef::MakePrimitive(TypeRef::Kind::Bool256);
const TypeRef Char256 = TypeRef::MakePrimitive(TypeRef::Kind::Char256);

constexpr Target::OS Systems[] = {Target::OS::Windows, Target::OS::Linux};

[[nodiscard]] LirInstr Instruction(const LirOpcode op, const LirReg destination, const TypeRef &type,
                                   std::vector<LirReg> sources = {}, std::string text = {}) {
    LirInstr instruction;
    instruction.op = op;
    instruction.dst = destination;
    instruction.type = type;
    instruction.srcs = std::move(sources);
    instruction.strArg = std::move(text);
    return instruction;
}

/// A function of `instructions` returning `result`. A second block follows the first, so the frame planner leaves
/// every value in its frame slot rather than in a callee-saved register, and the text below reads off one address
/// per value.
[[nodiscard]] LirFunc Function(std::string name, std::vector<LirParam> params, std::vector<LirInstr> instructions,
                               const LirReg result, const TypeRef &resultType) {
    LirFunc function;
    function.name = std::move(name);
    function.isPublic = true;
    function.returnType = resultType;
    function.params = std::move(params);
    LirBlock entry;
    entry.label = "entry";
    entry.instrs = std::move(instructions);
    entry.term.emplace();
    entry.term->kind = LirTermKind::Jump;
    entry.term->trueTarget = 1;
    LirBlock exit;
    exit.label = "exit";
    exit.term.emplace();
    exit.term->kind = LirTermKind::Return;
    exit.term->retVal = result;
    exit.term->retType = resultType;
    function.blocks = {std::move(entry), std::move(exit)};
    return function;
}

[[nodiscard]] LirPackage PackageOf(std::vector<LirFunc> functions) {
    LirModule module;
    module.name = "Consistency.rux";
    module.funcs = std::move(functions);
    LirPackage package;
    package.modules.push_back(std::move(module));
    return package;
}

/// The lines as the module printer writes them: indented four spaces, one per line.
[[nodiscard]] std::string Lines(const std::vector<std::string> &lines) {
    std::string text;
    for (const auto &line : lines) {
        text += "    " + line + "\n";
    }
    return text;
}

/// Every rbp-relative address the text names, in order.
[[nodiscard]] std::vector<std::int32_t> TextAddresses(const std::string &text) {
    static const std::regex address(R"(\[rbp ([-+]) (\d+)\])");
    std::vector<std::int32_t> addresses;
    for (auto match = std::sregex_iterator(text.begin(), text.end(), address); match != std::sregex_iterator();
         ++match) {
        const std::int32_t magnitude = std::stoi((*match)[2].str());
        addresses.push_back((*match)[1].str() == "-" ? -magnitude : magnitude);
    }
    return addresses;
}

/// The frame displacement a ModRM byte at `index` introduces, when it is one. The emitter addresses the frame only
/// through ModRM mod=10 r/m=101 — rbp plus a disp32 — and its frames are far smaller than 64 KiB, while no slot ends
/// nearer rbp than two bytes; a byte after the 0x0F escape is the second byte of a `jcc rel32` instead.
[[nodiscard]] std::optional<std::int32_t> FrameDisplacement(const std::vector<std::uint8_t> &code,
                                                            const std::size_t index) {
    if (index == 0 || index + 4 >= code.size() || (code[index] & 0xC7) != 0x85 || code[index - 1] == 0x0F) {
        return std::nullopt;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, &code[index + 1], 4);
    if (displacement > -2 && displacement < 2) {
        return std::nullopt;
    }
    if (displacement <= -65536 || displacement >= 65536) {
        return std::nullopt;
    }
    return displacement;
}

/// Every rbp-relative address the object code uses, in order. An opcode byte can have the ModRM shape too (`lea` is
/// 0x8D), so a candidate whose next byte is itself one is the opcode, not the ModRM.
[[nodiscard]] std::vector<std::int32_t> CodeAddresses(const std::vector<std::uint8_t> &code) {
    std::vector<std::int32_t> addresses;
    for (std::size_t index = 1; index < code.size(); ++index) {
        const auto displacement = FrameDisplacement(code, index);
        if (!displacement || FrameDisplacement(code, index + 1)) {
            continue;
        }
        addresses.push_back(*displacement);
        index += 4;
    }
    return addresses;
}

[[nodiscard]] std::vector<std::uint8_t> CodeOf(const LirPackage &package, const Target::OS os) {
    RcuEmitter emitter(package, "Consistency", os);
    const auto objects = emitter.Generate();
    REQUIRE(emitter.Diagnostics().empty());
    REQUIRE_EQ(objects.size(), 1);
    return objects.front().sections.at(RCU_TEXT_IDX).data;
}

/// The printed text and the object code of `function` use the same frame addresses in the same order.
void CheckAddressesAgree(const LirFunc &function, const Target::OS os) {
    CAPTURE(function.name);
    CAPTURE(static_cast<int>(os));
    const LirPackage package = PackageOf({function});
    const std::string text = AssemblyPrinter(package, os).Generate();
    CAPTURE(text);
    const auto spell = [](const std::vector<std::int32_t> &addresses) {
        std::string spelled;
        for (const std::int32_t address : addresses) {
            spelled += std::format("{} ", address);
        }
        return spelled;
    };
    CHECK_EQ(spell(TextAddresses(text)), spell(CodeAddresses(CodeOf(package, os))));
}

// ---- the functions -------------------------------------------------------------------------------------------------

/// `Add(l, r) = l + r` over float80, spilled through allocas the way lowering writes a parameter's home.
[[nodiscard]] LirFunc Float80Add() {
    return Function(
        "Add", {{0, Float80, "l"}, {2, Float80, "r"}},
        {Instruction(LirOpcode::Alloca, 1, Float80), Instruction(LirOpcode::Store, LirNoReg, Float80, {0, 1}),
         Instruction(LirOpcode::Alloca, 3, Float80), Instruction(LirOpcode::Store, LirNoReg, Float80, {2, 3}),
         Instruction(LirOpcode::Load, 4, Float80, {1}), Instruction(LirOpcode::Load, 5, Float80, {3}),
         Instruction(LirOpcode::Add, 6, Float80, {4, 5})},
        6, Float80);
}

/// Calls `Add` with two float80 constants and returns its result.
[[nodiscard]] LirFunc Float80Caller() {
    LirInstr call = Instruction(LirOpcode::Call, 2, Float80, {0, 1}, "Add");
    return Function("Caller", {},
                    {Instruction(LirOpcode::Const, 0, Float80, {}, "1.5"),
                     Instruction(LirOpcode::Const, 1, Float80, {}, "2.25"), call},
                    2, Float80);
}

/// A software-float constant and its negation.
[[nodiscard]] LirFunc Negation(const std::string &name, const TypeRef &type) {
    return Function(name, {},
                    {Instruction(LirOpcode::Const, 0, type, {}, "1.5"), Instruction(LirOpcode::Neg, 1, type, {0})}, 1,
                    type);
}

/// Every ordered and unordered comparison, and the remainder, of two machine floats.
[[nodiscard]] LirFunc FloatComparisons(const TypeRef &type) {
    std::vector<LirInstr> instructions;
    LirReg next = 2;
    for (const LirOpcode op :
         {LirOpcode::CmpEq, LirOpcode::CmpNe, LirOpcode::CmpLt, LirOpcode::CmpLe, LirOpcode::CmpGt, LirOpcode::CmpGe}) {
        instructions.push_back(Instruction(op, next++, TypeRef::MakeBool(), {0, 1}));
    }
    instructions.push_back(Instruction(LirOpcode::Mod, next, type, {0, 1}));
    return Function(type.kind == TypeRef::Kind::Float32 ? "Compare32" : "Compare64", {{0, type, "a"}, {1, type, "b"}},
                    std::move(instructions), next, type);
}

const TypeRef Triple = TypeRef::MakeTuple({TypeRef::MakeInt64(), TypeRef::MakeInt64(), TypeRef::MakeInt64()});
const TypeRef View = TypeRef::MakeSlice(TypeRef::MakeInt32());

/// The placeholder `Const 0` lowering gives an expression with no value of its own, at aggregate types of 24 and 16
/// bytes, and an int64 to return.
[[nodiscard]] LirFunc Placeholders() {
    return Function("Placeholders", {},
                    {Instruction(LirOpcode::Const, 0, Triple, {}, "0"), Instruction(LirOpcode::Const, 1, View, {}, "0"),
                     Instruction(LirOpcode::Const, 2, TypeRef::MakeInt64(), {}, "0")},
                    2, TypeRef::MakeInt64());
}

/// Every word-by-word operation on a wide integer of `type`, and the casts in and out of it.
[[nodiscard]] LirFunc WideIntegers(const std::string &name, const TypeRef &type) {
    std::vector<LirInstr> instructions = {Instruction(LirOpcode::Const, 2, type, {}, "340282366920938463463374607431"),
                                          Instruction(LirOpcode::Const, 3, TypeRef::MakeInt64(), {}, "5")};
    LirReg next = 4;
    for (const LirOpcode op : {LirOpcode::Add, LirOpcode::Sub, LirOpcode::And, LirOpcode::Or, LirOpcode::Xor,
                               LirOpcode::Mul, LirOpcode::Div, LirOpcode::Mod}) {
        instructions.push_back(Instruction(op, next++, type, {0, 1}));
    }
    for (const LirOpcode op : {LirOpcode::Shl, LirOpcode::Shr, LirOpcode::Lshr}) {
        instructions.push_back(Instruction(op, next++, type, {0, 3}));
    }
    for (const LirOpcode op : {LirOpcode::Neg, LirOpcode::BitNot}) {
        instructions.push_back(Instruction(op, next++, type, {2}));
    }
    instructions.push_back(Instruction(LirOpcode::Not, next++, TypeRef::MakeBool(), {0}));
    for (const LirOpcode op :
         {LirOpcode::CmpEq, LirOpcode::CmpNe, LirOpcode::CmpLt, LirOpcode::CmpLe, LirOpcode::CmpGt, LirOpcode::CmpGe}) {
        instructions.push_back(Instruction(op, next++, TypeRef::MakeBool(), {0, 1}));
    }
    instructions.push_back(Instruction(LirOpcode::Cast, next++, TypeRef::MakeBool(), {0}));
    instructions.push_back(Instruction(LirOpcode::Cast, next++, TypeRef::MakeInt64(), {0}));
    instructions.push_back(Instruction(LirOpcode::Cast, next++, type, {3}));
    instructions.push_back(Instruction(LirOpcode::Cast, next++, Int256, {0}));
    instructions.push_back(Instruction(LirOpcode::Cast, next++, UInt128, {0}));
    const LirReg result = next - 1;
    return Function(name, {{0, type, "a"}, {1, type, "b"}}, std::move(instructions), result, UInt128);
}

/// A wide bool and a wide character: constants, logic, equality and the casts the canonical form constrains.
[[nodiscard]] LirFunc WideBoolAndChar() {
    return Function("WideBoolChar", {{0, Bool256, "flag"}, {1, Char256, "letter"}},
                    {Instruction(LirOpcode::Const, 2, Bool256, {}, "true"),
                     Instruction(LirOpcode::And, 3, Bool256, {0, 2}), Instruction(LirOpcode::BitNot, 4, Bool256, {3}),
                     Instruction(LirOpcode::Not, 5, TypeRef::MakeBool(), {4}),
                     Instruction(LirOpcode::Cast, 6, TypeRef::MakeFloat64(), {4}),
                     Instruction(LirOpcode::Const, 7, Char256, {}, "65"),
                     Instruction(LirOpcode::CmpEq, 8, TypeRef::MakeBool(), {1, 7}),
                     Instruction(LirOpcode::CmpLt, 9, TypeRef::MakeBool(), {1, 7}),
                     Instruction(LirOpcode::Cast, 10, Bool256, {8}), Instruction(LirOpcode::Cast, 11, Char256, {0})},
                    11, Char256);
}

/// A call with more arguments than registers, mixing values passed by address under Win64 and in memory under System
/// V, and a callee receiving the same list.
[[nodiscard]] std::vector<LirFunc> ManyArguments() {
    const std::vector<TypeRef> types = {Int128, TypeRef::MakeInt64(), Float80, TypeRef::MakeFloat64(), Int256, Float128,
                                        Int128, TypeRef::MakeInt32(), Float80, TypeRef::MakeFloat32()};
    std::vector<LirParam> params;
    std::vector<LirReg> arguments;
    for (LirReg reg = 0; reg < types.size(); ++reg) {
        params.push_back({reg, types[reg], std::format("p{}", reg)});
        arguments.push_back(reg);
    }
    const auto count = static_cast<LirReg>(types.size());
    LirFunc callee = Function("Callee", params, {Instruction(LirOpcode::Add, count, Int256, {4, 4})}, count, Int256);
    LirInstr direct = Instruction(LirOpcode::Call, count, Int256, arguments, "Callee");
    LirFunc caller = Function("Forward", params, {direct}, count, Int256);
    return {callee, caller};
}

/// Wide values meeting at a phi and selected by a switch, and stored and loaded through pointers.
[[nodiscard]] LirFunc WidePhi() {
    LirFunc function;
    function.name = "WidePhi";
    function.isPublic = true;
    function.returnType = Int256;
    function.params = {{0, TypeRef::MakeInt64(), "selector"},
                       {1, Int256, "a"},
                       {2, Int256, "b"},
                       {3, TypeRef::MakePointer(Int128), "where"}};
    LirBlock entry;
    entry.label = "entry";
    entry.instrs = {Instruction(LirOpcode::Load, 4, Int128, {3}),
                    Instruction(LirOpcode::Store, LirNoReg, Int128, {4, 3})};
    entry.term.emplace();
    entry.term->kind = LirTermKind::Switch;
    entry.term->cond = 0;
    entry.term->cases.push_back({.value = "1", .target = 1});
    entry.term->defaultTarget = 2;
    LirBlock one;
    one.label = "one";
    one.term.emplace();
    one.term->kind = LirTermKind::Jump;
    one.term->trueTarget = 3;
    LirBlock other;
    other.label = "other";
    other.term.emplace();
    other.term->kind = LirTermKind::Jump;
    other.term->trueTarget = 3;
    LirBlock join;
    join.label = "join";
    LirInstr phi = Instruction(LirOpcode::Phi, 5, Int256);
    phi.phiPreds = {{1, 1}, {2, 2}};
    join.instrs = {phi};
    join.term.emplace();
    join.term->kind = LirTermKind::Return;
    join.term->retVal = 5;
    join.term->retType = Int256;
    function.blocks = {std::move(entry), std::move(one), std::move(other), std::move(join)};
    return function;
}
} // namespace

TEST_CASE("x86-64 assembly text and object code use the same frame addresses in the same order") {
    std::vector<LirFunc> functions = {Float80Add(),
                                      Float80Caller(),
                                      Negation("Negate16", Float16),
                                      Negation("Negate80", Float80),
                                      Negation("Negate128", Float128),
                                      Negation("Negate256", Float256),
                                      FloatComparisons(TypeRef::MakeFloat32()),
                                      FloatComparisons(TypeRef::MakeFloat64()),
                                      WideIntegers("Signed128", Int128),
                                      WideIntegers("Unsigned128", UInt128),
                                      WideIntegers("Signed256", Int256),
                                      WideBoolAndChar(),
                                      WidePhi(),
                                      Placeholders()};
    for (LirFunc &function : ManyArguments()) {
        functions.push_back(std::move(function));
    }
    for (const Target::OS os : Systems) {
        for (const LirFunc &function : functions) {
            CheckAddressesAgree(function, os);
        }
    }
}

TEST_CASE("x86-64 assembly text passes and returns wide values the way each convention does") {
    for (const Target::OS os : Systems) {
        CAPTURE(static_cast<int>(os));
        const bool windows = os == Target::OS::Windows;
        const LirFunc add = Float80Add();
        const LirFunc caller = Float80Caller();
        const std::string text = AssemblyPrinter(PackageOf({add, caller}), os).Generate();
        const X86_64FramePlan plan = PlanX86_64Frame(add, {}, {}, os);
        const auto slot = [&](const X86_64FramePlan &frame, const LirReg reg) { return frame.SlotOffsets().at(reg); };
        const auto contains = [&](const std::vector<std::string> &lines) {
            CAPTURE(Lines(lines));
            CHECK(text.contains(Lines(lines)));
        };

        if (windows) {
            // A float80 is sixteen bytes, so Win64 passes it by address and returns it through a pointer the caller
            // passes in rcx, which moves the two parameters to rdx and r8. Each is copied into the callee's own slot.
            REQUIRE_NE(plan.HiddenReturnOffset(), 0);
            contains({std::format("mov     qword [rbp - {}], rcx", plan.HiddenReturnOffset()), "mov     r10, rdx",
                      "mov     rax, qword [r10]", std::format("mov     qword [rbp - {}], rax", slot(plan, 0)),
                      "mov     rax, qword [r10 + 8]", std::format("mov     qword [rbp - {}], rax", slot(plan, 0) - 8),
                      "mov     r10, r8", "mov     rax, qword [r10]",
                      std::format("mov     qword [rbp - {}], rax", slot(plan, 2)), "mov     rax, qword [r10 + 8]",
                      std::format("mov     qword [rbp - {}], rax", slot(plan, 2) - 8)});
            // The result is copied through the hidden pointer, which is also returned in rax.
            contains({std::format("mov     r11, qword [rbp - {}]", plan.HiddenReturnOffset()),
                      std::format("lea     r10, [rbp - {}]", slot(plan, 6)), "mov     rax, qword [r10]",
                      "mov     qword [r11], rax", "mov     rax, qword [r10 + 8]", "mov     qword [r11 + 8], rax",
                      "mov     rax, r11"});
            CHECK_FALSE(text.contains(std::format("mov     qword [rbp - {}], rcx", slot(plan, 0))));
        }
        else {
            // System V passes a sixteen-byte value in two integer registers and returns it in rax:rdx.
            CHECK_EQ(plan.HiddenReturnOffset(), 0);
            contains({std::format("mov     qword [rbp - {}], rdi", slot(plan, 0)),
                      std::format("mov     qword [rbp - {}], rsi", slot(plan, 0) - 8),
                      std::format("mov     qword [rbp - {}], rdx", slot(plan, 2)),
                      std::format("mov     qword [rbp - {}], rcx", slot(plan, 2) - 8)});
            contains({std::format("mov     rax, qword [rbp - {}]", slot(plan, 6)),
                      std::format("mov     r10, qword [rbp - {}]", slot(plan, 6) - 8), "mov     rdx, r10", "leave"});
        }

        const X86_64FramePlan callerPlan = PlanX86_64Frame(caller, {}, {}, os);
        if (windows) {
            // The caller passes its result slot and the addresses of its argument slots.
            contains({"sub     rsp, 32", std::format("lea     rcx, [rbp - {}]", slot(callerPlan, 2)),
                      std::format("lea     rdx, [rbp - {}]", slot(callerPlan, 0)),
                      std::format("lea     r8, [rbp - {}]", slot(callerPlan, 1)), "call    Add", "add     rsp, 32"});
        }
        else {
            contains({std::format("mov     rax, qword [rbp - {}]", slot(callerPlan, 0)), "mov     rdi, rax",
                      std::format("mov     rax, qword [rbp - {}]", slot(callerPlan, 0) - 8), "mov     rsi, rax",
                      std::format("mov     rax, qword [rbp - {}]", slot(callerPlan, 1)), "mov     rdx, rax",
                      std::format("mov     rax, qword [rbp - {}]", slot(callerPlan, 1) - 8), "mov     rcx, rax",
                      "mov     eax, 0", "call    Add",
                      std::format("mov     qword [rbp - {}], rax", slot(callerPlan, 2)),
                      std::format("mov     qword [rbp - {}], rdx", slot(callerPlan, 2) - 8)});
        }
    }
}

TEST_CASE("x86-64 assembly text encodes software-float constants and negates them by the sign bit") {
    for (const Target::OS os : Systems) {
        CAPTURE(static_cast<int>(os));
        const LirFunc wide = Negation("Negate128", Float128);
        const LirFunc narrow = Negation("Negate16", Float16);
        const std::string text = AssemblyPrinter(PackageOf({wide, narrow}), os).Generate();
        const auto contains = [&](const std::vector<std::string> &lines) {
            CAPTURE(Lines(lines));
            CHECK(text.contains(Lines(lines)));
        };

        // float128 1.5: sign 0, exponent 0x3FFF, the fraction's top bit; written as its two words.
        const X86_64FramePlan widePlan = PlanX86_64Frame(wide, {}, {}, os);
        const std::int32_t constant = widePlan.SlotOffsets().at(0);
        const std::int32_t negated = widePlan.SlotOffsets().at(1);
        contains({"mov     eax, 0", std::format("mov     qword [rbp - {}], rax", constant),
                  "mov     rax, 0x3FFF800000000000", std::format("mov     qword [rbp - {}], rax", constant - 8)});
        // Negation copies the value and flips bit 127, in the high word.
        contains({std::format("mov     rax, qword [rbp - {}]", constant),
                  std::format("mov     qword [rbp - {}], rax", negated),
                  std::format("mov     rax, qword [rbp - {}]", constant - 8),
                  std::format("mov     qword [rbp - {}], rax", negated - 8),
                  std::format("mov     r10, qword [rbp - {}]", negated - 8), "mov     rax, 0x8000000000000000",
                  "xor     rax, r10", std::format("mov     qword [rbp - {}], rax", negated - 8)});

        // float16 1.5 is 0x3E00, a word; negation flips bit 15 in rax.
        const X86_64FramePlan narrowPlan = PlanX86_64Frame(narrow, {}, {}, os);
        contains({"mov     eax, 15872", std::format("mov     word [rbp - {}], ax", narrowPlan.SlotOffsets().at(0))});
        contains({std::format("movzx   rax, word [rbp - {}]", narrowPlan.SlotOffsets().at(0)), "xor     rax, 0x8000",
                  std::format("mov     word [rbp - {}], ax", narrowPlan.SlotOffsets().at(1))});

        // Neither is spelled as an integer literal or an integer negation.
        CHECK_FALSE(text.contains("1.5"));
        CHECK_FALSE(text.contains("neg     rax"));
    }
}

TEST_CASE("x86-64 assembly text gives SSE comparisons the unordered handling and remainders the fprem loop") {
    for (const Target::OS os : Systems) {
        for (const TypeRef &type : {TypeRef::MakeFloat32(), TypeRef::MakeFloat64()}) {
            CAPTURE(static_cast<int>(os));
            CAPTURE(type.ToString());
            const bool float32 = type.kind == TypeRef::Kind::Float32;
            const LirFunc function = FloatComparisons(type);
            const std::string text = AssemblyPrinter(PackageOf({function}), os).Generate();
            const X86_64FramePlan plan = PlanX86_64Frame(function, {}, {}, os);
            const auto contains = [&](const std::vector<std::string> &lines) {
                CAPTURE(Lines(lines));
                CHECK(text.contains(Lines(lines)));
            };
            // A NaN operand sets PF: every comparison but `!=` masks it off, and `!=` folds it in.
            const std::string compare = float32 ? "ucomiss xmm0, xmm1" : "ucomisd xmm0, xmm1";
            contains({compare, "sete    al", "setnp   dl", "and     al, dl", "movzx   rax, al"});
            contains({compare, "setne   al", "setp    dl", "or      al, dl", "movzx   rax, al"});
            for (const std::string_view set : {"setb", "setbe", "seta", "setae"}) {
                contains({compare, std::format("{:<7} al", set), "setnp   dl", "and     al, dl"});
            }

            // The remainder (register 8) pushes the divisor and then the dividend from their slots, loops on
            // fprem's C2 flag, drops the divisor and stores the exact remainder: the `EmitRemainder` sequence.
            const std::string_view width = float32 ? "dword" : "qword";
            const std::string pushes = Lines({std::format("fld     {} [rbp - {}]", width, plan.SlotOffsets().at(1)),
                                              std::format("fld     {} [rbp - {}]", width, plan.SlotOffsets().at(0))});
            const std::size_t start = text.find(pushes);
            REQUIRE(start != std::string::npos);
            const std::size_t label = start + pushes.size();
            const std::size_t colon = text.find(":\n", label);
            REQUIRE(colon != std::string::npos);
            const std::string loop = text.substr(label, colon - label);
            CHECK(loop.starts_with(".x87_fprem_"));
            const std::string body = Lines({"fprem", "fnstsw  ax", "test    ah, 4", "jnz     " + loop, "fstp    st1",
                                            std::format("fstp    {} [rbp - {}]", width, plan.SlotOffsets().at(8))});
            CHECK_EQ(text.compare(colon + 2, body.size(), body), 0);
            // Exact, so the control word is untouched, and nothing of the truncated-quotient formula is left.
            CHECK_FALSE(text.contains("fnstcw"));
            CHECK_FALSE(text.contains("cvtts"));
            CHECK_FALSE(text.contains("divs"));
        }
    }
}

TEST_CASE("an aggregate placeholder constant zeroes every word of its slot in the object code and the text") {
    for (const Target::OS os : Systems) {
        CAPTURE(static_cast<int>(os));
        const LirFunc function = Placeholders();
        const LirPackage package = PackageOf({function});
        const std::string text = AssemblyPrinter(package, os).Generate();
        const X86_64FramePlan plan = PlanX86_64Frame(function, {}, {}, os);
        const std::int32_t triple = plan.SlotOffsets().at(0);
        const std::int32_t view = plan.SlotOffsets().at(1);
        // Once `al` was the whole of a 24-byte placeholder, and rdx's leftover the second word of a 16-byte one.
        const std::vector<std::string> zeroTriple = {"mov     eax, 0",
                                                     std::format("mov     qword [rbp - {}], rax", triple),
                                                     std::format("mov     qword [rbp - {}], rax", triple - 8),
                                                     std::format("mov     qword [rbp - {}], rax", triple - 16)};
        const std::vector<std::string> zeroView = {"mov     eax, 0", std::format("mov     qword [rbp - {}], rax", view),
                                                   std::format("mov     qword [rbp - {}], rax", view - 8)};
        CHECK(text.contains(Lines(zeroTriple)));
        CHECK(text.contains(Lines(zeroView)));
        CHECK_FALSE(text.contains("], al"));
        CHECK_FALSE(text.contains("], rdx"));

        // The object code writes the same words: each `mov [rbp - n], rax` is REX.W 89 85 and the displacement.
        const std::vector<std::uint8_t> code = CodeOf(package, os);
        const auto stores = [&](const std::int32_t slot, const int words) {
            std::vector<std::uint8_t> bytes = {0xB8, 0, 0, 0, 0}; // mov eax, 0
            for (int word = 0; word < words; ++word) {
                const std::int32_t displacement = -slot + word * 8;
                bytes.insert(bytes.end(), {0x48, 0x89, 0x85});
                for (int shift = 0; shift < 32; shift += 8) {
                    bytes.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(displacement) >> shift));
                }
            }
            return std::ranges::search(code, bytes).begin() != code.end();
        };
        CHECK(stores(triple, 3));
        CHECK(stores(view, 2));
    }
}

TEST_CASE("x86-64 assembly text lowers wide integers word by word") {
    for (const Target::OS os : Systems) {
        CAPTURE(static_cast<int>(os));
        const LirFunc function = WideIntegers("Signed128", Int128);
        const std::string text = AssemblyPrinter(PackageOf({function}), os).Generate();
        const X86_64FramePlan plan = PlanX86_64Frame(function, {}, {}, os);
        const auto slot = [&](const LirReg reg, const int word) { return plan.SlotOffsets().at(reg) - word * 8; };
        const auto contains = [&](const std::vector<std::string> &lines) {
            CAPTURE(Lines(lines));
            CHECK(text.contains(Lines(lines)));
        };
        // The constant's two words, both wider than an imm32.
        contains({"mov     rax, 0xB5A52CB98B405447", std::format("mov     qword [rbp - {}], rax", slot(2, 0)),
                  "mov     rax, 0x44B82FA09", std::format("mov     qword [rbp - {}], rax", slot(2, 1))});
        // Addition carries from the low word into the high one.
        contains({std::format("mov     rax, qword [rbp - {}]", slot(0, 0)),
                  std::format("mov     r10, qword [rbp - {}]", slot(1, 0)), "add     rax, r10",
                  std::format("mov     qword [rbp - {}], rax", slot(4, 0)),
                  std::format("mov     rax, qword [rbp - {}]", slot(0, 1)),
                  std::format("mov     r10, qword [rbp - {}]", slot(1, 1)), "adc     rax, r10",
                  std::format("mov     qword [rbp - {}], rax", slot(4, 1))});
        // A signed comparison decides on the top word, signed, before the low word, unsigned.
        const std::size_t less =
            text.find(Lines({std::format("mov     rax, qword [rbp - {}]", slot(0, 1)),
                             std::format("mov     r10, qword [rbp - {}]", slot(1, 1)), "cmp     rax, r10"}) +
                      "    jl      ");
        CHECK_NE(less, std::string::npos);
        // Nothing reaches the single-register spellings.
        CHECK_FALSE(text.contains("imul    rax, r10"));
        CHECK_FALSE(text.contains("idiv"));
        CHECK_FALSE(text.contains("neg     rax"));
        CHECK_FALSE(text.contains(", 340282366920938463463374607431"));
    }
}

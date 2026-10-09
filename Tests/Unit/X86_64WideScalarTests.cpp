// x86-64 code generation for the wide bools and the wide characters.
//
// A wide bool is held as 0 or 1 in word 0 and zeros in every word above, and a wide character as the unsigned integer
// of its code point. The run-time checks below store each result to memory and load it back as the unsigned integer of
// the same width, so they compare every byte of the slot and not only the low word.

#include "CodeGen/X86_64/AssemblyInstructionPrinter.h"
#include "CodeGen/X86_64/AssemblyModulePrinter.h"
#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Linker/Linker.h"
#include "System/Process.h"
#include "Target/Platform.h"

#include <atomic>
#include <doctest.h>
#include <filesystem>
#include <format>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
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

/// A straight-line function that returns a bit mask of the checks that failed, so a non-zero exit code names them.
class WideScalarProgram {
public:
    WideScalarProgram() {
        failures = Constant(TypeRef::MakeInt32(), "0");
    }

    [[nodiscard]] LirReg Constant(const TypeRef &type, std::string value) {
        return Add(Instruction(LirOpcode::Const, next, type, {}, std::move(value)));
    }

    [[nodiscard]] LirReg Binary(const LirOpcode op, const LirReg left, const LirReg right, const TypeRef &type) {
        return Add(Instruction(op, next, type, {left, right}));
    }

    [[nodiscard]] LirReg Unary(const LirOpcode op, const LirReg value, const TypeRef &type) {
        return Add(Instruction(op, next, type, {value}));
    }

    /// The bytes of `value`, a `type`, reread as `bits` through memory rather than through a cast.
    [[nodiscard]] LirReg Reinterpret(const LirReg value, const TypeRef &type, const TypeRef &bits) {
        const LirReg home = Add(Instruction(LirOpcode::Alloca, next, type));
        instructions.push_back(Instruction(LirOpcode::Store, LirNoReg, type, {value, home}));
        return Add(Instruction(LirOpcode::Load, next, bits, {home}));
    }

    [[nodiscard]] LirReg Call(std::string name, const LirReg argument, const TypeRef &type) {
        return Add(Instruction(LirOpcode::Call, next, type, {argument}, std::move(name)));
    }

    void Assert(const LirReg condition) {
        // The message is only read on the failure path, where any value faults and fails the run anyway.
        const LirReg message = Constant(TypeRef::MakeInt64(), "0");
        instructions.push_back(Instruction(LirOpcode::Assert, LirNoReg, TypeRef::MakeOpaque(), {condition, message}));
    }

    /// Checks that `actual` equals the constant `expected` of type `type`.
    void Expect(const LirReg actual, const TypeRef &type, std::string expected) {
        ExpectTrue(Binary(LirOpcode::CmpEq, actual, Constant(type, std::move(expected)), TypeRef::MakeBool()));
    }

    void ExpectTrue(const LirReg condition) {
        REQUIRE_LT(checkIndex, 31U);
        const LirReg mismatch = Unary(LirOpcode::Not, condition, TypeRef::MakeBool());
        const LirReg mismatchCode = Unary(LirOpcode::Cast, mismatch, TypeRef::MakeInt32());
        const LirReg weight = Constant(TypeRef::MakeInt32(), std::to_string(1U << checkIndex++));
        const LirReg weighted = Binary(LirOpcode::Mul, mismatchCode, weight, TypeRef::MakeInt32());
        failures = Binary(LirOpcode::Add, failures, weighted, TypeRef::MakeInt32());
    }

    [[nodiscard]] LirFunc Finish() {
        LirBlock block;
        block.label = "entry";
        block.instrs = std::move(instructions);
        block.term.emplace();
        block.term->kind = LirTermKind::Return;
        block.term->retVal = failures;
        block.term->retType = TypeRef::MakeInt32();

        LirFunc function;
        function.name = "Checks";
        function.returnType = TypeRef::MakeInt32();
        function.blocks.push_back(std::move(block));
        return function;
    }

private:
    LirReg Add(LirInstr instruction) {
        instructions.push_back(std::move(instruction));
        return next++;
    }

    std::vector<LirInstr> instructions;
    LirReg next = 0;
    LirReg failures = LirNoReg;
    std::uint32_t checkIndex = 0;
};

/// A function with no parameters returning int32: `Scribble` fills a large frame with all-ones words, and `Main` calls
/// it before `Checks`, whose frame then starts on that dirty stack. A fresh process stack is zero, which would let a
/// wide value whose upper words were never written pass every check by accident.
[[nodiscard]] LirFunc StraightLineFunction(std::string name, std::vector<LirInstr> instructions, const LirReg result) {
    LirBlock block;
    block.label = "entry";
    block.instrs = std::move(instructions);
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = result;
    block.term->retType = TypeRef::MakeInt32();

    LirFunc function;
    function.name = std::move(name);
    function.isPublic = function.name == "Main";
    function.returnType = TypeRef::MakeInt32();
    function.blocks.push_back(std::move(block));
    return function;
}

[[nodiscard]] LirFunc ScribbleFunction() {
    const TypeRef uint512 = TypeRef::MakePrimitive(TypeRef::Kind::UInt512);
    std::vector<LirInstr> instructions;
    LirReg reg = 0;
    for (; reg < 256; ++reg) {
        instructions.push_back(Instruction(LirOpcode::Const, reg, uint512, {}, "-1"));
    }
    // Each constant feeds the next sum, so none of them is dead.
    for (LirReg source = 1; source < 256; ++source) {
        instructions.push_back(Instruction(LirOpcode::Xor, reg, uint512, {source == 1 ? LirReg{0} : reg - 1, source}));
        ++reg;
    }
    const LirReg narrowed = reg;
    instructions.push_back(Instruction(LirOpcode::Cast, narrowed, TypeRef::MakeInt32(), {reg - 1}));
    return StraightLineFunction("Scribble", std::move(instructions), narrowed);
}

[[nodiscard]] LirFunc MainFunction() {
    std::vector<LirInstr> instructions = {Instruction(LirOpcode::Call, 0, TypeRef::MakeInt32(), {}, "Scribble"),
                                          Instruction(LirOpcode::Call, 1, TypeRef::MakeInt32(), {}, "Checks")};
    return StraightLineFunction("Main", std::move(instructions), 1);
}

/// `Choose(condition)` returns 1 when its wide bool argument is true and 0 otherwise, through a `Branch` terminator.
[[nodiscard]] LirFunc ChooseFunction(const TypeRef &boolType) {
    LirFunc function;
    function.name = "Choose";
    function.params.push_back({0, boolType, "condition"});
    function.returnType = TypeRef::MakeInt32();

    LirBlock entry;
    entry.label = "entry";
    entry.term.emplace();
    entry.term->kind = LirTermKind::Branch;
    entry.term->cond = 0;
    entry.term->trueTarget = 1;
    entry.term->falseTarget = 2;
    function.blocks.push_back(std::move(entry));

    for (const auto &[label, value, reg] : {std::tuple{"yes", "1", LirReg{1}}, std::tuple{"no", "0", LirReg{2}}}) {
        LirBlock block;
        block.label = label;
        block.instrs = {Instruction(LirOpcode::Const, reg, TypeRef::MakeInt32(), {}, value)};
        block.term.emplace();
        block.term->kind = LirTermKind::Return;
        block.term->retVal = reg;
        block.term->retType = TypeRef::MakeInt32();
        function.blocks.push_back(std::move(block));
    }
    return function;
}

[[nodiscard]] TypeRef UnsignedOfSize(const int bytes) {
    return TypeRef::MakePrimitive(bytes == 16   ? TypeRef::Kind::UInt128
                                  : bytes == 32 ? TypeRef::Kind::UInt256
                                                : TypeRef::Kind::UInt512);
}

[[nodiscard]] LirPackage WideBoolPackage(const TypeRef::Kind kind) {
    const TypeRef boolType = TypeRef::MakePrimitive(kind);
    const int size = Layout::SizeOf(boolType);
    const TypeRef bits = UnsignedOfSize(size);
    // Another wide bool width, wider unless this is already the widest, for the casts between widths.
    const TypeRef otherBool =
        TypeRef::MakePrimitive(kind == TypeRef::Kind::Bool512 ? TypeRef::Kind::Bool128 : TypeRef::Kind::Bool512);
    const TypeRef otherBits = UnsignedOfSize(Layout::SizeOf(otherBool));
    const TypeRef bool8 = TypeRef::MakeBool();

    WideScalarProgram program;
    const LirReg yes = program.Constant(boolType, "true");
    const LirReg no = program.Constant(boolType, "false");
    const auto expectBits = [&](const LirReg value, std::string expected) {
        program.Expect(program.Reinterpret(value, boolType, bits), bits, std::move(expected));
    };

    // Constants and the bitwise operators keep the canonical form in every word.
    expectBits(yes, "1");
    expectBits(no, "0");
    expectBits(program.Binary(LirOpcode::And, yes, yes, boolType), "1");
    expectBits(program.Binary(LirOpcode::And, yes, no, boolType), "0");
    expectBits(program.Binary(LirOpcode::Or, no, yes, boolType), "1");
    expectBits(program.Binary(LirOpcode::Xor, yes, yes, boolType), "0");
    expectBits(program.Binary(LirOpcode::Xor, yes, no, boolType), "1");
    expectBits(program.Unary(LirOpcode::BitNot, yes, boolType), "0");
    expectBits(program.Unary(LirOpcode::BitNot, no, boolType), "1");

    // `!` and the equalities produce a narrow bool.
    program.ExpectTrue(program.Unary(LirOpcode::Not, no, bool8));
    program.Expect(program.Unary(LirOpcode::Not, yes, bool8), bool8, "false");
    program.ExpectTrue(program.Binary(LirOpcode::CmpEq, yes, program.Constant(boolType, "true"), bool8));
    program.ExpectTrue(program.Binary(LirOpcode::CmpNe, yes, no, bool8));
    program.Expect(program.Binary(LirOpcode::CmpEq, yes, no, bool8), bool8, "false");

    // Into a wide bool: a narrow bool zero-extends, and any other source reads as whether it has a bit set. The
    // all-ones neighbours of the zero int64 are what a cast reading past its source's slot would pick up.
    expectBits(program.Unary(LirOpcode::Cast, program.Constant(bool8, "true"), boolType), "1");
    expectBits(program.Unary(LirOpcode::Cast, program.Constant(TypeRef::MakeInt64(), "5"), boolType), "1");
    const LirReg allOnesBefore = program.Constant(TypeRef::MakeInt64(), "-1");
    const LirReg zero64 = program.Constant(TypeRef::MakeInt64(), "0");
    const LirReg allOnesAfter = program.Constant(TypeRef::MakeInt64(), "-1");
    expectBits(program.Unary(LirOpcode::Cast, zero64, boolType), "0");
    program.ExpectTrue(program.Binary(LirOpcode::CmpEq, allOnesBefore, allOnesAfter, bool8));
    expectBits(program.Unary(LirOpcode::Cast, program.Constant(bits, "18446744073709551616"), boolType), "1");

    // Out of a wide bool: another bool width keeps the truth bit, and an integer gets the unsigned value 0 or 1.
    program.ExpectTrue(program.Unary(LirOpcode::Cast, yes, bool8));
    program.Expect(program.Unary(LirOpcode::Cast, no, bool8), bool8, "false");
    program.Expect(program.Unary(LirOpcode::Cast, yes, TypeRef::MakeInt32()), TypeRef::MakeInt32(), "1");
    program.Expect(program.Unary(LirOpcode::Cast, yes, TypeRef::MakeFloat64()), TypeRef::MakeFloat64(), "1.0");
    program.Expect(program.Reinterpret(program.Unary(LirOpcode::Cast, yes, otherBool), otherBool, otherBits), otherBits,
                   "1");
    program.Expect(program.Unary(LirOpcode::Cast, yes, otherBits), otherBits, "1");

    // Control flow and an assertion read the truth from word 0.
    program.Expect(program.Call("Choose", yes, TypeRef::MakeInt32()), TypeRef::MakeInt32(), "1");
    program.Expect(program.Call("Choose", no, TypeRef::MakeInt32()), TypeRef::MakeInt32(), "0");
    program.Assert(yes);

    LirModule module;
    module.name = "WideBool.rux";
    module.funcs.push_back(ChooseFunction(boolType));
    module.funcs.push_back(ScribbleFunction());
    module.funcs.push_back(program.Finish());
    module.funcs.push_back(MainFunction());
    LirPackage package;
    package.modules.push_back(std::move(module));
    return package;
}

/// `Echo(value)` returns its wide character argument unchanged, so a call passes one by value both ways.
[[nodiscard]] LirFunc EchoFunction(const TypeRef &charType) {
    LirFunc function;
    function.name = "Echo";
    function.params.push_back({0, charType, "value"});
    function.returnType = charType;
    LirBlock entry;
    entry.label = "entry";
    entry.term.emplace();
    entry.term->kind = LirTermKind::Return;
    entry.term->retVal = 0;
    entry.term->retType = charType;
    function.blocks.push_back(std::move(entry));
    return function;
}

[[nodiscard]] LirPackage WideCharPackage(const TypeRef::Kind kind) {
    const TypeRef charType = TypeRef::MakePrimitive(kind);
    const int size = Layout::SizeOf(charType);
    const TypeRef bits = UnsignedOfSize(size);
    // Another wide character width, wider unless this is already the widest, for the casts between widths.
    const TypeRef otherChar =
        TypeRef::MakePrimitive(kind == TypeRef::Kind::Char512 ? TypeRef::Kind::Char128 : TypeRef::Kind::Char512);
    const TypeRef otherBits = UnsignedOfSize(Layout::SizeOf(otherChar));
    const TypeRef char32 = TypeRef::MakeChar32();
    const TypeRef char64 = TypeRef::MakePrimitive(TypeRef::Kind::Char64);
    const TypeRef bool8 = TypeRef::MakeBool();

    WideScalarProgram program;
    const auto expectBits = [&](const LirReg value, std::string expected) {
        program.Expect(program.Reinterpret(value, charType, bits), bits, std::move(expected));
    };

    // A constant is its code point in word 0 and zeros above it, the last scalar value included.
    const LirReg letter = program.Constant(charType, "65");
    const LirReg last = program.Constant(charType, "1114111");
    expectBits(letter, "65");
    expectBits(last, "1114111");

    // Widening from a narrow character or another wide one zero-extends; narrowing keeps the code point.
    const LirReg emoji = program.Unary(LirOpcode::Cast, program.Constant(char32, "128512"), charType);
    expectBits(emoji, "128512");
    expectBits(program.Unary(LirOpcode::Cast, program.Constant(char64, "955"), charType), "955");
    program.Expect(program.Reinterpret(program.Unary(LirOpcode::Cast, emoji, otherChar), otherChar, otherBits),
                   otherBits, "128512");
    program.Expect(program.Unary(LirOpcode::Cast, emoji, char32), char32, "128512");
    program.Expect(program.Unary(LirOpcode::Cast, last, char64), char64, "1114111");
    program.Expect(program.Unary(LirOpcode::Cast, emoji, otherBits), otherBits, "128512");
    program.Expect(program.Unary(LirOpcode::Cast, letter, TypeRef::MakeUInt32()), TypeRef::MakeUInt32(), "65");

    // The comparisons read every word, unsigned: a value whose top bit is set orders above a letter, where a signed
    // comparison of the top word would put it below.
    program.ExpectTrue(program.Binary(LirOpcode::CmpEq, letter, program.Constant(charType, "65"), bool8));
    program.ExpectTrue(program.Binary(LirOpcode::CmpNe, letter, emoji, bool8));
    program.ExpectTrue(program.Binary(LirOpcode::CmpLt, letter, emoji, bool8));
    program.ExpectTrue(program.Binary(LirOpcode::CmpGe, last, emoji, bool8));
    program.Expect(program.Binary(LirOpcode::CmpGt, letter, emoji, bool8), bool8, "false");
    const LirReg topBit = program.Unary(LirOpcode::Cast, program.Constant(bits, "-1"), charType);
    program.ExpectTrue(program.Binary(LirOpcode::CmpGt, topBit, letter, bool8));
    const LirReg secondWord = program.Unary(LirOpcode::Cast, program.Constant(bits, "18446744073709551616"), charType);
    program.ExpectTrue(program.Binary(LirOpcode::CmpNe, secondWord, program.Constant(charType, "0"), bool8));

    // A call passes a wide character by value and returns it the same way.
    program.Expect(program.Call("Echo", emoji, charType), charType, "128512");

    LirModule module;
    module.name = "WideChar.rux";
    module.funcs.push_back(EchoFunction(charType));
    module.funcs.push_back(ScribbleFunction());
    module.funcs.push_back(program.Finish());
    module.funcs.push_back(MainFunction());
    LirPackage package;
    package.modules.push_back(std::move(module));
    return package;
}

void RunWideScalarPackage(const LirPackage &package) {
    RcuEmitter sysvEmitter(package, "WideBool", Target::OS::Linux);
    const auto sysvObjects = sysvEmitter.Generate();
    REQUIRE(sysvEmitter.Diagnostics().empty());
    REQUIRE_EQ(sysvObjects.size(), 1);

    RcuEmitter emitter(package, "WideBool", Target::OS::Windows);
    auto objects = emitter.Generate();
    REQUIRE(emitter.Diagnostics().empty());
    REQUIRE_EQ(objects.size(), 1);

#if RUX_OS_WINDOWS && RUX_ARCH_X86_64
    static std::atomic_uint64_t sequence = 0;
    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / ("rux-wide-scalar-x86-64-" + std::to_string(++sequence) + ".exe");
    std::error_code error;
    std::filesystem::remove(output, error);
    Linker linker(std::move(objects), "WideBool", {}, ArtifactKind::Executable, Target::OS::Windows,
                  Target::Arch::X86_64);
    const bool linked = linker.Link(output);
    const std::string linkMessage = linker.Errors().empty() ? "link failed" : linker.Errors().front().message;
    REQUIRE_MESSAGE(linked, linkMessage);
    const auto result = System::RunCaptured(output);
    std::filesystem::remove(output, error);
    REQUIRE(result.has_value());
    // Each set bit is one failed check, numbered in the order the package records them.
    CHECK_EQ(result->exitCode, 0);
#else
    CHECK_FALSE(objects.front().sections.at(RCU_TEXT_IDX).data.empty());
#endif
}
} // namespace

TEST_CASE("x86-64 RCU executes bool128 constants, logic, casts and branches") {
    RunWideScalarPackage(WideBoolPackage(TypeRef::Kind::Bool128));
}

TEST_CASE("x86-64 RCU executes bool256 constants, logic, casts and branches") {
    RunWideScalarPackage(WideBoolPackage(TypeRef::Kind::Bool256));
}

TEST_CASE("x86-64 RCU executes bool512 constants, logic, casts and branches") {
    RunWideScalarPackage(WideBoolPackage(TypeRef::Kind::Bool512));
}

TEST_CASE("x86-64 RCU executes char128, char256 and char512 constants, casts, comparisons and calls") {
    for (const auto kind : {TypeRef::Kind::Char128, TypeRef::Kind::Char256, TypeRef::Kind::Char512}) {
        CAPTURE(TypeRef::MakePrimitive(kind).ToString());
        RunWideScalarPackage(WideCharPackage(kind));
    }
}

TEST_CASE("x86-64 assembly text keeps wide bools canonical") {
    const TypeRef bool256 = TypeRef::MakePrimitive(TypeRef::Kind::Bool256);
    LirFunc function;
    function.name = "WideBoolText";
    function.callConv = CallingConvention::SysV;
    const LirInstr constant = Instruction(LirOpcode::Const, 0, bool256, {}, "true");
    const LirInstr inverted = Instruction(LirOpcode::BitNot, 1, bool256, {0});
    const LirInstr narrow = Instruction(LirOpcode::Cast, 2, TypeRef::MakeBool(), {1});
    const LirInstr source = Instruction(LirOpcode::Const, 3, TypeRef::MakeInt64(), {}, "7");
    const LirInstr widened = Instruction(LirOpcode::Cast, 4, bool256, {3});
    const LirInstr equal = Instruction(LirOpcode::CmpEq, 5, TypeRef::MakeBool(), {0, 4});
    LirBlock block;
    block.instrs = {constant, inverted, narrow, source, widened, equal};
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    function.blocks.push_back(std::move(block));

    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaceNames;
    const X86_64FramePlan framePlan = PlanX86_64Frame(function, layouts, interfaceNames, Target::OS::Linux);
    AssemblyModulePrinter modulePrinter(Target::OS::Linux);
    AssemblyInstructionPrinter printer(modulePrinter, framePlan, layouts, interfaceNames, Target::OS::Linux);
    printer.EmitFunctionSetup(function);
    CHECK(printer.EmitMemory(constant));
    CHECK(printer.EmitArithmetic(inverted));
    CHECK(printer.EmitArithmetic(narrow));
    CHECK(printer.EmitMemory(source));
    CHECK(printer.EmitArithmetic(widened));
    CHECK(printer.EmitArithmetic(equal));
    const std::string output = modulePrinter.Finalize();

    const auto slot = [&](const LirReg reg, const int word) { return framePlan.SlotOffsets().at(reg) - word * 8; };
    const auto lines = [](const std::vector<std::string> &text) {
        std::string joined;
        for (const auto &line : text) {
            joined += "    " + line + "\n";
        }
        return joined;
    };
    // The constant writes its truth to word 0 and zeros to the three words above it, a word at a time.
    CHECK(output.contains(lines({"mov     eax, 1", std::format("mov     qword [rbp - {}], rax", slot(0, 0)),
                                 "mov     eax, 0", std::format("mov     qword [rbp - {}], rax", slot(0, 1)),
                                 "mov     eax, 0", std::format("mov     qword [rbp - {}], rax", slot(0, 2)),
                                 "mov     eax, 0", std::format("mov     qword [rbp - {}], rax", slot(0, 3))})));
    // Widening an int64 to a bool256 stores its truth and zero-fills the words above.
    CHECK(output.contains(lines({std::format("mov     qword [rbp - {}], rax", slot(4, 0)), "mov     eax, 0",
                                 std::format("mov     qword [rbp - {}], rax", slot(4, 1)),
                                 std::format("mov     qword [rbp - {}], rax", slot(4, 2)),
                                 std::format("mov     qword [rbp - {}], rax", slot(4, 3))})));
    // `~` flips only the truth bit, and the narrowing cast folds all four source words.
    CHECK(output.find("xor     rax, 1") != std::string::npos);
    CHECK(output.find("not     rax") == std::string::npos);
    // A bool source is already 0 or 1, so the folded words are stored without normalizing.
    CHECK(output.contains(lines({std::format("mov     r10, qword [rbp - {}]", slot(1, 3)), "or      rax, r10"})));
    // The equality compares every word, the last included.
    CHECK(output.contains(lines({std::format("mov     r10, qword [rbp - {}]", slot(0, 3)),
                                 std::format("mov     rax, qword [rbp - {}]", slot(4, 3)), "cmp     rax, r10"})));
}

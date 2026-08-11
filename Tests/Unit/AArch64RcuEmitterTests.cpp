// The AArch64 RCU back end: the object it produces for a whole function, and
// the reports it makes for everything it does not lower yet.
//
// The expected words below came from `llvm-mc -triple=aarch64 -show-encoding`
// on the instruction named beside each, so a disagreement here is a
// disagreement with a second implementation rather than with someone's reading
// of the ARM manual.

#include "CodeGen/AArch64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <cstdint>
#include <doctest.h>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {
LirPackage CompileToAArch64Lir(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    const TargetContext target = Driver::TargetContextForTriple("linux-aarch64");
    Parser parser(std::move(lexed.tokens), "test.rux", target.arch);
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    std::vector<Module *> modules = {&parsed.module};
    SemanticAnalyzer analyzer(modules, {}, "test", "linux");
    auto semaModel = analyzer.Analyze();
    REQUIRE_FALSE(semaModel.HasErrors());

    AstToHirLowering hirLowering(semaModel);
    auto hirPackage = hirLowering.Generate();

    HirToLirLowering lirLowering(std::move(hirPackage), target);
    return lirLowering.Generate();
}

// An instruction word as a table spells it: doctest reports an integer in
// decimal, which says nothing at a glance against a column of hexadecimal.
[[nodiscard]] std::string HexWord(const std::uint32_t word) {
    return std::format("0x{:08X}", word);
}

// The words of one function's body, read back out of .text through its symbol.
[[nodiscard]] std::vector<std::uint32_t> FunctionWords(const RcuFile &object, const std::string_view name) {
    const auto symbol = std::ranges::find_if(object.symbols, [name](const RcuSymbol &s) { return s.name == name; });
    REQUIRE(symbol != object.symbols.end());
    REQUIRE(symbol->sectionIdx < object.sections.size());

    const auto &text = object.sections[symbol->sectionIdx].data;
    REQUIRE(symbol->value + symbol->size <= text.size());
    REQUIRE(symbol->size % 4 == 0);

    std::vector<std::uint32_t> words;
    for (std::uint32_t offset = symbol->value; offset < symbol->value + symbol->size; offset += 4) {
        words.push_back(static_cast<std::uint32_t>(text[offset]) | static_cast<std::uint32_t>(text[offset + 1]) << 8U |
                        static_cast<std::uint32_t>(text[offset + 2]) << 16U |
                        static_cast<std::uint32_t>(text[offset + 3]) << 24U);
    }
    return words;
}

// The byte count of `sub sp, sp, #imm` or `add sp, sp, #imm`, in either shift
// position, or nothing when the word is some other instruction. The frame of a
// function too large for a pre-indexed STP is opened by a run of these, and how
// many it takes is FrameAdjust's business rather than this test's.
[[nodiscard]] std::optional<std::int64_t> StackPointerAdjustment(const std::uint32_t word, const bool subtract) {
    const std::uint32_t opcode = subtract ? 0xD1000000U : 0x91000000U;
    if ((word & 0xFF800000U) != opcode || (word & 0x3FFU) != 0x3FFU) {
        return std::nullopt; // not SUB/ADD (immediate) with SP as both operands
    }
    const std::int64_t imm12 = (word >> 10U) & 0xFFFU;
    return (word & (1U << 22U)) != 0 ? imm12 << 12U : imm12;
}

// The one message an emitter is expected to have produced, joined so a failure
// prints what was actually reported rather than a count.
[[nodiscard]] std::string JoinMessages(const std::vector<Diagnostic> &diagnostics) {
    std::string joined;
    for (const auto &diagnostic : diagnostics) {
        if (!joined.empty()) {
            joined += " | ";
        }
        joined += diagnostic.message;
    }
    return joined;
}
} // namespace

TEST_CASE("AArch64 RCU emitter generates a complete function returning a constant") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            return 42;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    REQUIRE_EQ(objects.size(), 1);

    const auto &object = objects.front();
    CHECK_EQ(object.arch, RcuArch::AArch64);
    REQUIRE_EQ(object.sections.size(), 3);
    CHECK_EQ(object.sections[RCU_TEXT_IDX].name, ".text");
    CHECK_EQ(object.sections[RCU_RODATA_IDX].name, ".rodata");
    CHECK_EQ(object.sections[RCU_DATA_IDX].name, ".data");

    // One 8-byte slot above the 16-byte frame record rounds the frame to 32.
    const std::vector<std::uint32_t> expected = {
        0xA9BE7BFD, // stp  x29, x30, [sp, #-32]!
        0x910003FD, // mov  x29, sp
        0xD2800549, // mov  x9, #42
        0xF9000BA9, // str  x9, [x29, #16]
        0xF9400BA0, // ldr  x0, [x29, #16]
        0xA8C27BFD, // ldp  x29, x30, [sp], #32
        0xD65F03C0, // ret
    };
    const auto words = FunctionWords(object, "Main");
    REQUIRE_EQ(words.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(HexWord(words[i]), HexWord(expected[i]));
    }
}

TEST_CASE("AArch64 RCU emitter predeclares every function before emitting bodies") {
    const auto package = CompileToAArch64Lir(R"(
        pub func First() -> int {
            return 1;
        }

        func Second() -> int {
            return 2;
        }
    )");

    const auto objects = AArch64RcuEmitter(package, "test").Generate();
    REQUIRE_EQ(objects.size(), 1);
    const auto &object = objects.front();

    // Both symbols exist, both point into .text, and the second body starts
    // where the first one ended rather than at zero.
    const auto first = std::ranges::find_if(object.symbols, [](const RcuSymbol &s) { return s.name == "First"; });
    const auto second = std::ranges::find_if(object.symbols, [](const RcuSymbol &s) { return s.name == "Second"; });
    REQUIRE(first != object.symbols.end());
    REQUIRE(second != object.symbols.end());
    CHECK_EQ(first->sectionIdx, RCU_TEXT_IDX);
    CHECK_EQ(second->sectionIdx, RCU_TEXT_IDX);
    CHECK_EQ(first->visibility, RcuSymVis::Global);
    CHECK_EQ(second->visibility, RcuSymVis::Local);
    CHECK_EQ(first->value, 0);
    CHECK_EQ(second->value, first->size);
    CHECK_EQ(first->size + second->size, object.sections[RCU_TEXT_IDX].data.size());
}

TEST_CASE("AArch64 RCU emitter keeps the stack pointer 16-byte aligned across a large frame") {
    // Enough locals to put the frame past the reach of a pre-indexed STP, so
    // the prologue opens it with FrameAdjust instead.
    std::string body;
    for (int i = 0; i < 200; ++i) {
        body += std::format("    var v{}: int = {};\n", i, i);
    }
    const auto package = CompileToAArch64Lir(std::format(R"(
        func Main() -> int {{
{}            return 0;
        }}
    )",
                                                         body));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    const auto words = FunctionWords(objects.front(), "Main");
    REQUIRE_GE(words.size(), 6);

    // The prologue opens the frame with as many SUBs as it takes, and only then
    // stores the frame record and takes the frame pointer.
    std::int64_t opened = 0;
    std::size_t index = 0;
    while (index < words.size()) {
        const auto step = StackPointerAdjustment(words[index], true);
        if (!step) {
            break;
        }
        // Each instruction of a multi-instruction adjustment has to leave SP a
        // multiple of 16, not just the last one.
        CHECK_EQ(*step % 16, 0);
        opened += *step;
        ++index;
    }
    CHECK_MESSAGE(opened > 512, "the frame is meant to be past the reach of a pre-indexed STP: ", opened);
    CHECK_EQ(opened % 16, 0);
    REQUIRE_GT(index + 1, 1);
    CHECK_EQ(HexWord(words[index]), HexWord(0xA9007BFD));     // stp x29, x30, [sp]
    CHECK_EQ(HexWord(words[index + 1]), HexWord(0x910003FD)); // mov x29, sp

    // The epilogue restores the record first and closes exactly what was opened.
    CHECK_EQ(HexWord(words.back()), HexWord(0xD65F03C0)); // ret
    std::int64_t closed = 0;
    std::size_t tail = words.size() - 1;
    while (tail > 0) {
        const auto step = StackPointerAdjustment(words[tail - 1], false);
        if (!step) {
            break;
        }
        CHECK_EQ(*step % 16, 0);
        closed += *step;
        --tail;
    }
    CHECK_EQ(closed, opened);
    REQUIRE_GT(tail, 0);
    CHECK_EQ(HexWord(words[tail - 1]), HexWord(0xA9407BFD)); // ldp x29, x30, [sp]
}

TEST_CASE("AArch64 RCU emitter reports an unimplemented opcode by name") {
    // Written so the sum survives constant folding and reaches the back end as
    // an `add` rather than as one more constant.
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var total: int = 1;
            total = total + 2;
            return total;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_EQ(objects.size(), 1);

    const auto reports = JoinMessages(emitter.Diagnostics());
    CHECK_MESSAGE(reports.contains("'add' opcode"), reports);
    CHECK_MESSAGE(reports.contains("not implemented yet"), reports);
    CHECK_MESSAGE(reports.contains("'Main'"), reports);
    for (const auto &diagnostic : emitter.Diagnostics()) {
        CHECK(diagnostic.IsError());
    }
}

TEST_CASE("AArch64 RCU emitter names each unimplemented construct once") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var total: int = 0;
            for i in 0..8 {
                total = total + i;
            }
            return total;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_EQ(objects.size(), 1);

    std::vector<std::string> messages;
    for (const auto &diagnostic : emitter.Diagnostics()) {
        messages.push_back(diagnostic.message);
    }
    auto sorted = messages;
    std::ranges::sort(sorted);
    CHECK_EQ(std::ranges::unique(sorted).begin(), sorted.end());
    CHECK_MESSAGE(!messages.empty(), "a loop reaches opcodes this back end does not lower yet");
}

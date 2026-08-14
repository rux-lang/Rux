// AArch64 inline-assembly RCU emission.
//
// The expected words below came from `llvm-mc -triple=aarch64 -show-encoding`
// on the instruction named beside each, so a disagreement here is a
// disagreement with a second implementation rather than with someone's reading
// of the ARM manual.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"

#include <cstddef>
#include <cstdint>
#include <doctest.h>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

// An `asm func` is the one body this back end selects no instructions for.
// The cases below verify its raw bytes and relocations.

TEST_CASE("AArch64 RCU emitter emits an asm func as a raw blob") {
    const auto package = CompileToAArch64Lir(R"(
        asm func Add(a: int64, b: int64) -> int64 {
            add x0, x0, x1
            ret
        }

        func Main() -> int {
            return Add(40, 2) as int;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // Exactly what the program wrote down: no frame record, no frame pointer,
    // no spill of the arguments AAPCS64 already put in the right registers.
    const std::vector<std::uint32_t> expected = {
        0x8B010000, // add x0, x0, x1
        0xD65F03C0, // ret
    };
    const auto words = FunctionWords(object, "Add");
    REQUIRE_EQ(words.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(HexWord(words[i]), HexWord(expected[i]));
    }

    // The caller is an ordinary body, so the two live in one .text and the
    // symbol of each names its own bytes.
    const RcuSymbol *symbol = FindSymbol(object, "Add");
    REQUIRE(symbol != nullptr);
    CHECK_EQ(symbol->sectionIdx, RCU_TEXT_IDX);
    CHECK_EQ(symbol->size, expected.size() * 4);
    CHECK(FunctionWords(object, "Main").size() > expected.size());
}

TEST_CASE("AArch64 RCU emitter relocates a symbol an asm func names") {
    const auto package = CompileToAArch64Lir(R"(
        asm func Triple(x: int64) -> int64 {
            mov x1, #3
            mul x0, x0, x1
            ret
        }

        asm func TripleThenAdd(x: int64) -> int64 {
            stp x0, x30, [sp, #-16]!
            bl Triple
            ldp x1, x30, [sp], #16
            add x0, x0, x1
            ret
        }

        func Main() -> int {
            return TripleThenAdd(10) as int;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // The branch carries no displacement of its own; the relocation on it names
    // the callee, and the callee is this module's own symbol rather than an
    // extern the reference invented.
    const auto words = FunctionWords(object, "TripleThenAdd");
    const auto branch = BranchAndLinkIndex(words);
    REQUIRE_MESSAGE(branch.has_value(), "no bl in the body");
    CHECK_EQ(BranchDisplacement(words[*branch]), 0);

    const auto relocs = RelocsFor(object, RCU_TEXT_IDX, "Triple");
    REQUIRE_EQ(relocs.size(), 1);
    CHECK_EQ(relocs.front().type, RcuRelType::AArch64Call26);
    const RcuSymbol *callee = FindSymbol(object, "Triple");
    REQUIRE(callee != nullptr);
    CHECK_EQ(callee->sectionIdx, RCU_TEXT_IDX);

    const RcuSymbol *caller = FindSymbol(object, "TripleThenAdd");
    REQUIRE(caller != nullptr);
    CHECK_EQ(relocs.front().sectionOffset, caller->value + *branch * 4);
}

TEST_CASE("AArch64 RCU emitter emits every word of an asm func") {
    // An `asm func` is the body the source wrote and nothing else: no
    // prologue opens a frame the body did not ask for, and no epilogue follows
    // the RET the body already wrote.
    const auto package = CompileToAArch64Lir(R"(
        asm func AddAsm(a: int64, b: int64) -> int64 {
            add x0, x0, x1
            ret
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    CheckFunctionImage(objects.front(), "AddAsm",
                       {
                           0x8B010000, // add x0, x0, x1
                           0xD65F03C0, // ret
                       });
}

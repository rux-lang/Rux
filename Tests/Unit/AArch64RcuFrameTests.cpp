// AArch64 RCU function skeletons, frames, stack probing and unsupported-opcode
// diagnostics.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <doctest.h>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

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

    // The constant is the one value this function holds, so the allocation
    // gives it X19 and the constant is materialized there rather than in the
    // scratch register and stored. One doubleword preserves X19 for the caller
    // and one is the slot the value keeps anyway, which above the 16-byte frame
    // record rounds the frame to 32.
    CheckFunctionImage(object, "Main",
                       {
                           0xA9BE7BFD, // stp  x29, x30, [sp, #-32]!
                           0x910003FD, // mov  x29, sp
                           0xF9000BB3, // str  x19, [x29, #16]
                           0xD2800553, // mov  x19, #42
                           0xAA1303E0, // mov  x0, x19
                           0xF9400BB3, // ldr  x19, [x29, #16]
                           0xA8C27BFD, // ldp  x29, x30, [sp], #32
                           0xD65F03C0, // ret
                       });
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

    // Linux keeps its existing frame emission byte-for-byte: page touches are
    // selected only for Windows.
    CHECK(std::ranges::find(words, kStackProbeTouch) == words.end());
}

TEST_CASE("Windows AArch64 selects stack probing at the 4 KiB frame boundary") {
    struct BoundaryCase {
        int arrayBytes;
        std::int64_t frameBytes;
        std::size_t probes;
    };

    // The frame record, alloca pointer slot, and return constant occupy the 48 bytes
    // above these arrays. The three resulting frames sit immediately below,
    // at, and immediately above one Windows stack page.
    constexpr std::array cases{
        BoundaryCase{4032, 4080, 0},
        BoundaryCase{4048, 4096, 1},
        BoundaryCase{4064, 4112, 1},
    };

    for (const BoundaryCase &boundary : cases) {
        CAPTURE(boundary.arrayBytes);
        CAPTURE(boundary.frameBytes);
        const auto package = CompileToAArch64Lir(std::format(R"(
            func Main() -> int {{
                var frame: uint8[{}];
                return 0;
            }}
        )",
                                                             boundary.arrayBytes),
                                                 "windows-aarch64");

        AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
        const auto objects = emitter.Generate();
        CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
        const auto words = FunctionWords(objects.front(), "Main");

        std::int64_t opened = 0;
        std::size_t probes = 0;
        std::size_t index = 0;
        while (index < words.size()) {
            const auto step = StackPointerAdjustment(words[index], true);
            if (!step) {
                break;
            }
            opened += *step;
            ++index;
            if (*step == 4096) {
                REQUIRE_LT(index, words.size());
                CHECK_EQ(HexWord(words[index]), HexWord(kStackProbeTouch));
                ++probes;
                ++index;
            }
        }
        CHECK_EQ(opened, boundary.frameBytes);
        CHECK_EQ(probes, boundary.probes);
        REQUIRE_LT(index, words.size());
        CHECK_EQ(HexWord(words[index]), HexWord(0xA9007BFD)); // stp x29, x30, [sp]
    }
}

TEST_CASE("Windows AArch64 probes every page before opening a multi-page function frame") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var pages: uint8[12288];
            pages[0] = 1;
            pages[4096] = 2;
            pages[12287] = 3;
            return 0;
        }
    )",
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // Each full page is entered by one aligned SUB and touched immediately.
    // The frame contains three pages of local storage plus its record and
    // slots, so there are at least three pairs before the final aligned tail.
    std::int64_t opened = 0;
    std::size_t probes = 0;
    std::size_t index = 0;
    while (index < words.size()) {
        const auto step = StackPointerAdjustment(words[index], true);
        if (!step) {
            break;
        }
        CHECK_EQ(*step % 16, 0);
        opened += *step;
        ++index;
        if (*step == 4096) {
            REQUIRE_LT(index, words.size());
            CHECK_EQ(HexWord(words[index]), HexWord(kStackProbeTouch));
            ++probes;
            ++index;
        }
    }
    CHECK_GE(probes, 3);
    CHECK_GE(opened, 12288);
    CHECK_EQ(opened % 16, 0);

    // The ordinary frame record and frame pointer follow the completed probe,
    // preserving the x29/x30 chain used by every other AArch64 frame.
    REQUIRE_LT(index + 1, words.size());
    CHECK_EQ(HexWord(words[index]), HexWord(0xA9007BFD));     // stp x29, x30, [sp]
    CHECK_EQ(HexWord(words[index + 1]), HexWord(0x910003FD)); // mov x29, sp

    // Returning restores the full frame with ordinary ADDs. Growing SP cannot
    // encounter a guard page, so no page touch belongs in the epilogue.
    CHECK_EQ(HexWord(words.back()), HexWord(0xD65F03C0)); // ret
    std::int64_t closed = 0;
    std::size_t tail = words.size() - 1;
    while (tail > 0) {
        const auto step = StackPointerAdjustment(words[tail - 1], false);
        if (!step) {
            break;
        }
        closed += *step;
        --tail;
    }
    CHECK_EQ(closed, opened);
    REQUIRE_GT(tail, 0);
    CHECK_EQ(HexWord(words[tail - 1]), HexWord(0xA9407BFD)); // ldp x29, x30, [sp]
}

TEST_CASE("Windows AArch64 probes a large outgoing copy and restores it without touching result registers") {
    const auto package = CompileToAArch64Lir(R"(
        func Take(payload: uint8[8192]) -> int {
            return payload[0] as int;
        }

        func Main() -> int {
            var payload: uint8[8192];
            payload[0] = 37;
            return Take(payload);
        }
    )",
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());

    // Ignore the function-frame probes before MOV x29, sp. The by-reference
    // copy opens an exact two-page outgoing area later, and each page is
    // touched before the unrolled copy begins.
    const auto framePointer = std::ranges::find(caller, 0x910003FDU);
    REQUIRE(framePointer != caller.end());
    const auto bodyStart = static_cast<std::size_t>(framePointer - caller.begin() + 1);
    REQUIRE_LT(bodyStart, *call);
    const auto outgoingTouches =
        std::ranges::count(std::ranges::subrange(caller.begin() + static_cast<std::ptrdiff_t>(bodyStart),
                                                 caller.begin() + static_cast<std::ptrdiff_t>(*call)),
                           kStackProbeTouch);
    CHECK_EQ(outgoingTouches, 2);

    // The call result arrives in X0. Closing the area is a single ordinary ADD
    // that names only SP, followed by the store that keeps X0 in its slot.
    REQUIRE_LT(*call + 2, caller.size());
    CHECK_EQ(StackPointerAdjustment(caller[*call + 1], false), std::optional<std::int64_t>(8192));
    CHECK_EQ(ArgumentDrained(caller[*call + 2]), std::optional<unsigned>(0));
    CHECK(std::ranges::find(caller.begin() + static_cast<std::ptrdiff_t>(*call + 1), caller.end(), kStackProbeTouch) ==
          caller.end());
}

// A struct and two values of it, which is the one construct a source program
// reaches that this back end still refuses: comparing two values that are not a
// bit pattern in one register is a run of comparisons the x86-64 back end emits
// and this one does not.
constexpr std::string_view kAggregateCompare = R"(
        struct Point {
            x: int;
            y: int;
        }
)";

TEST_CASE("AArch64 RCU emitter reports an unimplemented opcode by name") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            let a = (1, 2);
            let b = (1, 2);
            if a == b {{
                return 1;
            }}
            return 0;
        }}
    )",
                                                         kAggregateCompare));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_EQ(objects.size(), 1);

    const auto reports = JoinMessages(emitter.Diagnostics());
    CHECK_MESSAGE(reports.contains("the 'cmpeq' opcode on '(int, int)'"), reports);
    CHECK_MESSAGE(reports.contains("target 'linux-aarch64'"), reports);
    CHECK_MESSAGE(reports.contains("'Main'"), reports);
    for (const auto &diagnostic : emitter.Diagnostics()) {
        CHECK(diagnostic.IsError());
    }
}

TEST_CASE("AArch64 RCU emitter names each unimplemented construct once") {
    // Three comparisons, which are three instructions of the one opcode this
    // back end does not lower yet: what a report names is the construct, so
    // reaching it again says nothing new.
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            let a = (1, 2);
            let b = (1, 2);
            let first = a == b;
            let second = a == b;
            let third = a == b;
            return 0;
        }}
    )",
                                                         kAggregateCompare));

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
    REQUIRE_EQ(messages.size(), 1);
    CHECK_MESSAGE(messages.front().contains("the 'cmpeq' opcode on '(int, int)'"), messages.front());
    CHECK_MESSAGE(messages.front().contains("target 'linux-aarch64'"), messages.front());
}

// AArch64 runtime-failure paths: platform writers, syscalls, imports and traps.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/RcuEmitter.h"

#include <algorithm>
#include <cstdint>
#include <doctest.h>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

namespace {
// The two intrinsics an assertion and a panic are written as, and the type
// their message travels in, declared here rather than imported: these cases are
// compiled as a package of one module with no dependencies, and `Core` is where
// all three usually live.
constexpr std::string_view kAssertIntrinsics = R"(
        struct Slice<T> {
            data: *T;
            length: uint;
        }

        intrinsic func Assert(condition: bool, message: Slice<char8>);
        intrinsic func Panic(message: Slice<char8>);
)";

// The words a write to standard error takes once its buffer and length are in
// place: the descriptor, the call number, and the trap that asks for it. Linux
// numbers a write 64 and takes the number in X8.
constexpr std::uint32_t kMovX0StdErr = 0xD2800040U;       // mov x0, #2
constexpr std::uint32_t kMovX8Write = 0xD2800808U;        // mov x8, #64
constexpr std::uint32_t kMovX8FreeBsdWrite = 0xD2800088U; // mov x8, #4
constexpr std::uint32_t kSvc0 = 0xD4000001U;              // svc #0
constexpr std::uint32_t kAdrpX1 = 0x90000001U;            // adrp x1, <symbol>
constexpr std::uint32_t kAddX1Lo12 = 0x91000021U;         // add  x1, x1, #:lo12:<symbol>
constexpr std::uint32_t kLdpX1X2 = 0xA9400941U;           // ldp  x1, x2, [x10]
constexpr std::uint32_t kBrk1 = 0xD4200020U;              // brk  #1

// Windows reaches standard error through KERNEL32. GetStdHandle receives -12
// in X0, and every WriteFile receives its last two arguments in X3 and X4.
constexpr std::uint32_t kSubSp16 = 0xD10043FFU;           // sub sp, sp, #16
constexpr std::uint32_t kMovX0StdErrHandle = 0x92800160U; // mov x0, #-12
constexpr std::uint32_t kMovX3Sp = 0x910003E3U;           // mov x3, sp
constexpr std::uint32_t kMovX4Zero = 0xD2800004U;         // mov x4, #0
constexpr std::uint32_t kBl0 = 0x94000000U;               // bl <import>

// The immediate of `mov xN, #imm` in the one-instruction move-wide form, or
// nothing when the word is some other instruction. A message's length is
// materialized this way, and how long a message is depends on where in this
// file the source it came from sits.
[[nodiscard]] std::optional<std::uint32_t> MoveWideImm(const std::uint32_t word, const unsigned reg) {
    if ((word & 0xFFE0001FU) != (0xD2800000U | reg)) {
        return std::nullopt;
    }
    return word >> 5U & 0xFFFFU;
}

// The index of the one `brk` in a body, which is where an assertion's failure
// path ends and therefore how the words before it are found without counting
// the instructions ahead of them.
[[nodiscard]] std::optional<std::size_t> TrapIndex(const std::vector<std::uint32_t> &words) {
    const auto found = std::ranges::find(words, kBrk1);
    if (found == words.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - words.begin());
}

// How many times a run of bytes appears in the read-only section, which is what
// says whether two assertions shared one interned prefix or were each given a
// copy of it.
[[nodiscard]] std::size_t RodataOccurrences(const RcuFile &object, const std::string_view text) {
    const auto &rodata = object.sections[RCU_RODATA_IDX].data;
    const std::string_view bytes(reinterpret_cast<const char *>(rodata.data()), rodata.size());
    std::size_t count = 0;
    for (std::size_t at = bytes.find(text); at != std::string_view::npos; at = bytes.find(text, at + 1)) {
        ++count;
    }
    return count;
}
} // namespace

TEST_CASE("AArch64 RCU emitter writes a failed assertion's three parts and traps") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Assert(1 == 1, "one");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();
    const auto words = FunctionWords(object, "Main");

    const auto trap = TrapIndex(words);
    REQUIRE_MESSAGE(trap.has_value(), "no brk in the body");
    REQUIRE(*trap >= 19);

    // Read backwards from the trap: the location, the message and the prefix,
    // each of them one write, and each write ending in the same three words.
    const std::size_t location = *trap - 6;
    const std::size_t message = *trap - 11;
    const std::size_t prefix = *trap - 17;
    for (const std::size_t write : {prefix, message, location}) {
        const std::size_t end = write == message ? message + 2 : write + 3;
        CHECK_EQ(HexWord(words[end]), HexWord(kMovX0StdErr));
        CHECK_EQ(HexWord(words[end + 1]), HexWord(kMovX8Write));
        CHECK_EQ(HexWord(words[end + 2]), HexWord(kSvc0));
    }

    // The prefix and the location are addresses this object knows, reached as a
    // page and an offset with both immediates left to their relocations; the
    // lengths are what the two texts actually are.
    CHECK_EQ(HexWord(words[prefix]), HexWord(kAdrpX1));
    CHECK_EQ(HexWord(words[prefix + 1]), HexWord(kAddX1Lo12));
    CHECK_EQ(MoveWideImm(words[prefix + 2], 2), std::string("Assertion failed: ").size());
    CHECK_EQ(HexWord(words[location]), HexWord(kAdrpX1));
    CHECK_EQ(HexWord(words[location + 1]), HexWord(kAddX1Lo12));
    CHECK_MESSAGE(MoveWideImm(words[location + 2], 2).has_value(), HexWord(words[location + 2]));

    // The message is neither: what the operand holds is the address of a
    // `Slice<char8>`, and one LDP takes both of its fields into the two
    // registers the call reads them from.
    CHECK_EQ(ArgumentFilled(words[message]), std::optional<unsigned>(10)); // the address, into X10
    CHECK_EQ(HexWord(words[message + 1]), HexWord(kLdpX1X2));

    // A condition that held branches over the whole of it, landing on the
    // instruction after the trap.
    const std::size_t condition = *trap - 18;
    CHECK_EQ(words[condition] & 0xFF00001FU, 0xB5000009U); // cbnz x9, <after the trap>
    CHECK_EQ(BranchDisplacement(words[condition]), static_cast<std::int32_t>(*trap + 1 - condition));

    // Both texts are in the read-only section, spelled the way the x86-64 back
    // end spells them, and the location names the function, the file and the
    // position the front end recorded.
    CHECK_EQ(RodataOccurrences(object, "Assertion failed: "), 1);
    CHECK_EQ(RodataOccurrences(object, "\n  at Main (test.rux:"), 1);
}

TEST_CASE("AArch64 RCU emitter interns one assertion prefix for every site") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Assert(1 == 1, "one");
            Assert(2 == 2, "two");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // One prefix behind both sites, and a location of its own for each: the
    // prefix is the same string twice and the two locations differ in the line
    // they name.
    CHECK_EQ(RodataOccurrences(object, "Assertion failed: "), 1);
    CHECK_EQ(RodataOccurrences(object, "\n  at Main (test.rux:"), 2);

    // Two traps and two branches over them, which is what says the second
    // assertion is a path of its own rather than a jump into the first.
    const auto words = FunctionWords(object, "Main");
    CHECK_EQ(std::ranges::count(words, kBrk1), 2);
    CHECK_EQ(std::ranges::count(words, kSvc0), 6);
}

TEST_CASE("AArch64 RCU emitter reaches a panic with no condition and no branch") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Panic("stop");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();
    const auto words = FunctionWords(object, "Main");

    const auto trap = TrapIndex(words);
    REQUIRE_MESSAGE(trap.has_value(), "no brk in the body");
    CHECK_EQ(std::ranges::count(words, kSvc0), 3);
    CHECK_EQ(RodataOccurrences(object, "Panic: "), 1);
    CHECK_EQ(RodataOccurrences(object, "Assertion failed: "), 0);

    // Nothing is tested and nothing is skipped: a panic always trapped, so no
    // conditional branch stands anywhere in the body.
    for (const std::uint32_t word : words) {
        CHECK_MESSAGE((word & 0xFF000010U) != 0x54000000U, HexWord(word)); // b.<cond>
        CHECK_MESSAGE((word & 0x7E000000U) != 0x34000000U, HexWord(word)); // cbz / cbnz
    }
}

TEST_CASE("AArch64 RCU emitter asks each kernel for a write by its own numbering") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Assert(1 == 1, "one");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics));

    AArch64RcuEmitter darwin(package, "test", Target::OS::MacOS);
    const auto objects = darwin.Generate();
    CHECK_MESSAGE(darwin.Diagnostics().empty(), JoinMessages(darwin.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // Darwin numbers a write 4, takes the number in X16 and is asked through a
    // trap of its own, so not one word of the three that surround the buffer is
    // the same as Linux's.
    CHECK_EQ(std::ranges::count(words, 0xD2800090U), 3); // mov x16, #4
    CHECK_EQ(std::ranges::count(words, 0xD4001001U), 3); // svc #0x80
    CHECK_EQ(std::ranges::count(words, kMovX8Write), 0);
    CHECK_EQ(std::ranges::count(words, kSvc0), 0);
    CHECK_EQ(std::ranges::count(words, kMovX0StdErr), 3); // the descriptor is the same
}

TEST_CASE("FreeBSD AArch64 assertions and panics write every fragment through syscall 4 before trapping") {
    const auto check = [](const std::string_view statement, const std::string_view prefix) {
        const auto package = CompileToAArch64Lir(std::format(R"(
            {}
            func Main() -> int {{
                {}
                return 0;
            }}
        )",
                                                             kAssertIntrinsics, statement),
                                                 "freebsd-aarch64");

        AArch64RcuEmitter emitter(package, "test", Target::OS::FreeBSD);
        const auto objects = emitter.Generate();
        CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
        const auto &object = objects.front();
        const auto words = FunctionWords(object, "Main");
        const auto trap = TrapIndex(words);
        REQUIRE_MESSAGE(trap.has_value(), "no diagnostic trap in FreeBSD body");

        std::vector<std::size_t> writes;
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (words[i] == kSvc0) {
                writes.push_back(i);
            }
        }
        REQUIRE_EQ(writes.size(), 3);
        for (const std::size_t svc : writes) {
            REQUIRE_GE(svc, 2);
            CHECK_EQ(HexWord(words[svc - 2]), HexWord(kMovX0StdErr));
            CHECK_EQ(HexWord(words[svc - 1]), HexWord(kMovX8FreeBsdWrite));
        }

        // Static prefix/location writes materialize X1 and X2 separately; the
        // dynamic slice moves both with one LDP. All three complete before the
        // diagnostic BRK, which is immediately after the final SVC.
        CHECK_EQ(std::ranges::count(words, kAdrpX1), 2);
        CHECK_EQ(std::ranges::count(words, kAddX1Lo12), 2);
        CHECK_EQ(std::ranges::count(words, kLdpX1X2), 1);
        CHECK_EQ(writes.back() + 1, *trap);
        CHECK_EQ(std::ranges::count(words, kMovX8FreeBsdWrite), 3);
        CHECK_EQ(std::ranges::count(words, kMovX8Write), 0);
        CHECK_EQ(std::ranges::count(words, 0xD2800090U), 0); // Darwin's mov x16, #4
        CHECK_EQ(RodataOccurrences(object, prefix), 1);
    };

    check("Assert(1 == 1, \"held\");", "Assertion failed: ");
    check("Panic(\"stop\");", "Panic: ");
}

TEST_CASE("Windows AArch64 assertions write through KERNEL32 and branch over the failure path") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Assert(1 == 1, "one");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics),
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();
    const auto words = FunctionWords(object, "Main");

    const RcuSymbol *getStdHandle = FindSymbol(object, "GetStdHandle");
    const RcuSymbol *writeFile = FindSymbol(object, "WriteFile");
    REQUIRE(getStdHandle != nullptr);
    REQUIRE(writeFile != nullptr);
    for (const RcuSymbol *symbol : {getStdHandle, writeFile}) {
        CHECK_EQ(symbol->sectionIdx, RCU_SEC_EXTERNAL);
        CHECK_EQ(symbol->kind, RcuSymKind::ExternFunc);
        CHECK_EQ(symbol->visibility, RcuSymVis::Global);
        CHECK_EQ(symbol->typeName, "KERNEL32.DLL");
    }

    const auto getCalls = FunctionRelocs(object, "Main", "GetStdHandle");
    const auto writeCalls = FunctionRelocs(object, "Main", "WriteFile");
    REQUIRE_EQ(getCalls.size(), 3);
    REQUIRE_EQ(writeCalls.size(), 3);
    for (std::size_t i = 0; i < getCalls.size(); ++i) {
        CHECK_EQ(getCalls[i].second, RcuRelType::AArch64Call26);
        CHECK_EQ(writeCalls[i].second, RcuRelType::AArch64Call26);

        const std::size_t get = getCalls[i].first / A64Enc::InstrSize;
        const std::size_t write = writeCalls[i].first / A64Enc::InstrSize;
        REQUIRE(get + 2 < words.size());
        REQUIRE(write < words.size());
        CHECK(get < write);
        CHECK_EQ(HexWord(words[get]), HexWord(kBl0));
        CHECK_EQ(HexWord(words[get + 1]), HexWord(kMovX3Sp));
        CHECK_EQ(HexWord(words[get + 2]), HexWord(kMovX4Zero));
        CHECK_EQ(HexWord(words[write]), HexWord(kBl0));
    }

    // All five WriteFile arguments are visible in the instruction stream: X0
    // comes back from GetStdHandle, static writes materialize X1/X2 through a
    // relocated address and length, the slice loads both together for the
    // dynamic write, and X3/X4 were checked beside every imported call above.
    CHECK_EQ(std::ranges::count(words, kMovX0StdErrHandle), 3);
    CHECK_EQ(std::ranges::count(words, kLdpX1X2), 1);
    CHECK_EQ(std::ranges::count(words, kSubSp16), 1);
    CHECK_EQ(std::ranges::count(words, kSvc0), 0);

    std::vector<const RcuSymbol *> staticTexts;
    for (const auto &symbol : object.symbols) {
        if (symbol.sectionIdx != RCU_RODATA_IDX) {
            continue;
        }
        const auto bytes = RodataOf(object, symbol.name);
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size() - 1);
        if (text == "Assertion failed: " || text.starts_with("\n  at Main (test.rux:")) {
            staticTexts.push_back(&symbol);
        }
    }
    REQUIRE_EQ(staticTexts.size(), 2);
    for (const RcuSymbol *symbol : staticTexts) {
        const auto address = FunctionRelocs(object, "Main", symbol->name);
        REQUIRE_EQ(address.size(), 2);
        CHECK_EQ(address[0].second, RcuRelType::AArch64AdrPrelPgHi21);
        CHECK_EQ(address[1].second, RcuRelType::AArch64AddAbsLo12Nc);
        CHECK_EQ(HexWord(words[address[0].first / A64Enc::InstrSize]), HexWord(kAdrpX1));
        CHECK_EQ(HexWord(words[address[1].first / A64Enc::InstrSize]), HexWord(kAddX1Lo12));
    }

    const auto trap = TrapIndex(words);
    REQUIRE_MESSAGE(trap.has_value(), "no brk in the body");
    const auto held = std::ranges::find_if(words, [](const std::uint32_t word) {
        return (word & 0xFF00001FU) == 0xB5000009U; // cbnz x9, <after the trap>
    });
    REQUIRE(held != words.end());
    const std::size_t heldIndex = static_cast<std::size_t>(held - words.begin());
    CHECK_EQ(BranchDisplacement(*held), static_cast<std::int32_t>(*trap + 1 - heldIndex));
}

TEST_CASE("Windows AArch64 panics use the same imported writer and always trap") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Panic("stop");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics),
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();
    const auto words = FunctionWords(object, "Main");

    CHECK_EQ(FunctionRelocs(object, "Main", "GetStdHandle").size(), 3);
    CHECK_EQ(FunctionRelocs(object, "Main", "WriteFile").size(), 3);
    CHECK_EQ(std::ranges::count(words, kBrk1), 1);
    CHECK_EQ(RodataOccurrences(object, "Panic: "), 1);
    CHECK_EQ(RodataOccurrences(object, "Assertion failed: "), 0);
    for (const std::uint32_t word : words) {
        CHECK_MESSAGE((word & 0xFF000010U) != 0x54000000U, HexWord(word)); // b.<cond>
        CHECK_MESSAGE((word & 0x7E000000U) != 0x34000000U, HexWord(word)); // cbz / cbnz
    }
}

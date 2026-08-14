// AArch64 fixed-argument AAPCS64 call and return lowering.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/CallLayout.h"
#include "CodeGen/AArch64/RcuEmitter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <doctest.h>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

// AAPCS64 calls
//
// One convention for both sides of every call: the cases below read the caller
// and the callee out of the same object and check that what one wrote is where
// the other looks. Nothing here depends on a calling convention the LIR names,
// because AArch64 has one and a Rux function and a C function are called by it
// alike.

TEST_CASE("FreeBSD AArch64 selects generic AAPCS64 while Apple selects its fixed-argument variant") {
    constexpr AArch64CallLayoutPolicy apple = AArch64CallPolicyFor(Target::OS::MacOS);
    constexpr AArch64CallLayoutPolicy linux = AArch64CallPolicyFor(Target::OS::Linux);
    constexpr AArch64CallLayoutPolicy freebsd = AArch64CallPolicyFor(Target::OS::FreeBSD);
    constexpr AArch64CallLayoutPolicy windows = AArch64CallPolicyFor(Target::OS::Windows);

    CHECK_EQ(apple.StackAlignment(1), 1);
    CHECK_EQ(apple.StackBytes(1), 1);
    CHECK_EQ(apple.FirstGeneralRegister(1, 16), 1);
    CHECK(apple.callerExtendsNarrowIntegers);

    for (const AArch64CallLayoutPolicy policy : {linux, freebsd, windows}) {
        CHECK_EQ(policy.StackAlignment(1), 8);
        CHECK_EQ(policy.StackBytes(1), 8);
        CHECK_EQ(policy.FirstGeneralRegister(1, 16), 2);
        CHECK_FALSE(policy.callerExtendsNarrowIntegers);
    }
}

TEST_CASE("FreeBSD AArch64 fixed calls match generic AAPCS64 through aggregate and stack exhaustion") {
    const auto package = CompileToAArch64Lir(R"(
        struct Pair { first: int64; second: int64; }
        struct Big { first: int64; second: int64; third: int64; }

        func Exercise(a: int64, b: int64, c: int64, d: int64, e: int64, f: int64, g: int64,
                      pair: Pair, tail: uint16) -> Big {
            return Big { first: pair.first, second: pair.second, third: tail as int64 };
        }

        func Main() -> int {
            var pair = Pair { first: 8i64, second: 9i64 };
            var result = Exercise(1i64, 2i64, 3i64, 4i64, 5i64, 6i64, 7i64, pair, 10u16);
            return result.third as int;
        }
    )",
                                             "freebsd-aarch64");

    const auto emitFor = [&package](const Target::OS os) {
        AArch64RcuEmitter emitter(package, "test", os);
        auto objects = emitter.Generate();
        CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
        REQUIRE_EQ(objects.size(), 1);
        return objects.front();
    };

    const RcuFile linux = emitFor(Target::OS::Linux);
    const RcuFile freebsd = emitFor(Target::OS::FreeBSD);
    const RcuFile windows = emitFor(Target::OS::Windows);
    const RcuFile apple = emitFor(Target::OS::MacOS);

    // FreeBSD uses the same fixed-argument PCS as Linux: seven scalars fill
    // X0-X6, the two-register aggregate exhausts the general file and moves to
    // the stack, and the narrow tail follows in its own eight-byte slot. A
    // 24-byte result is returned indirectly through X8 on both sides.
    for (const std::string_view function : {"Exercise", "Main"}) {
        const auto expected = FunctionWords(linux, function);
        CHECK_EQ(FunctionWords(freebsd, function), expected);
        CHECK_EQ(FunctionWords(windows, function), expected);
    }

    const auto caller = FunctionWords(freebsd, "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    CHECK(std::ranges::any_of(caller, [](const std::uint32_t word) {
        const auto bytes = StackPointerAdjustment(word, true);
        return bytes.has_value() && *bytes == 32 && *bytes % 16 == 0;
    }));
    CHECK(std::ranges::any_of(caller, [](const std::uint32_t word) {
        return StackArgumentStoredBy(word, 8, 9) == std::optional<std::uint32_t>(16);
    }));
    REQUIRE_GE(*call, 1);
    CHECK(FramePointerAddImm(caller[*call - 1], 8).has_value());

    // Apple keeps the same public 16-byte SP alignment but naturally sized
    // stack slots, so the uint16 store provides a byte-level discriminator.
    const auto appleCaller = FunctionWords(apple, "Main");
    CHECK_NE(appleCaller, caller);
    CHECK(std::ranges::any_of(appleCaller, [](const std::uint32_t word) {
        return StackArgumentStoredBy(word, 2, 9) == std::optional<std::uint32_t>(16);
    }));
}

TEST_CASE("Apple AArch64 packs fixed stack arguments at natural alignment") {
    auto lowered = CompileToAArch64Lir(R"(
        func Packed(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int,
                    i: int8, j: uint16, k: int32, l: int64) -> int64 {
            return l;
        }

        func Main() -> int {
            var result = Packed(1, 2, 3, 4, 5, 6, 7, 8, -9, 10, 11, 12);
            return 0;
        }
    )");

    // Put the definition and use in separate RCU objects. Each module then
    // classifies its own side of the call, as separately compiled packages do.
    LirPackage package;
    package.modules.resize(2);
    package.modules[0].name = "callee.rux";
    package.modules[1].name = "caller.rux";
    for (auto &func : lowered.modules.front().funcs) {
        const std::size_t module = func.name == "Packed" ? 0 : 1;
        package.modules[module].funcs.push_back(std::move(func));
    }

    const auto emitFor = [&package](const Target::OS os) {
        AArch64RcuEmitter emitter(package, "test", os);
        auto objects = emitter.Generate();
        CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
        return objects;
    };
    const auto appleObjects = emitFor(Target::OS::MacOS);
    const auto linuxObjects = emitFor(Target::OS::Linux);
    const auto windowsObjects = emitFor(Target::OS::Windows);

    const auto objectWith = [](const std::vector<RcuFile> &objects, const std::string_view symbol) -> const RcuFile & {
        const auto found = std::ranges::find_if(objects, [symbol](const RcuFile &object) {
            const RcuSymbol *candidate = FindSymbol(object, symbol);
            return candidate != nullptr && candidate->sectionIdx != RCU_SEC_EXTERNAL;
        });
        REQUIRE(found != objects.end());
        return *found;
    };

    const auto outgoingArea = [](const std::vector<std::uint32_t> &words) {
        for (const std::uint32_t word : words) {
            if (const auto bytes = StackPointerAdjustment(word, true)) {
                return *bytes;
            }
        }
        return std::int64_t{0};
    };
    const auto hasStore = [](const std::vector<std::uint32_t> &words, const unsigned width,
                             const std::uint32_t offset) {
        return std::ranges::any_of(words, [width, offset](const std::uint32_t word) {
            return StackArgumentStored(word, width) == std::optional<std::uint32_t>(offset);
        });
    };

    const auto appleCaller = FunctionWords(objectWith(appleObjects, "Main"), "Main");
    CHECK_EQ(outgoingArea(appleCaller), 16);
    CHECK(hasStore(appleCaller, 1, 0));
    CHECK(hasStore(appleCaller, 2, 2));
    CHECK(hasStore(appleCaller, 4, 4));
    CHECK(hasStore(appleCaller, 8, 8));

    // The callee makes the same target-selected walk independently and reads
    // the four values immediately above its valid X29/X30 frame record.
    const auto appleCallee = FunctionWords(objectWith(appleObjects, "Packed"), "Packed");
    const auto frame = PreIndexedFrameSize(appleCallee.front());
    REQUIRE_MESSAGE(frame.has_value(), HexWord(appleCallee.front()));
    const auto hasIncoming = [&appleCallee, frame](const unsigned width, const bool sign, const std::int32_t offset) {
        return std::ranges::any_of(appleCallee, [width, sign, offset, frame](const std::uint32_t word) {
            return IncomingDisplacement(word, width, sign) ==
                   std::optional<std::int32_t>(static_cast<std::int32_t>(*frame) + offset);
        });
    };
    CHECK(hasIncoming(1, true, 0));
    CHECK(hasIncoming(2, false, 2));
    CHECK(hasIncoming(4, true, 4));
    CHECK(hasIncoming(8, true, 8));

    // Linux and non-variadic Windows retain the previous generic AAPCS64
    // doubleword layout for the same LIR call.
    for (const auto *objects : {&linuxObjects, &windowsObjects}) {
        const auto caller = FunctionWords(objectWith(*objects, "Main"), "Main");
        CHECK_EQ(outgoingArea(caller), 32);
        for (std::uint32_t slot = 0; slot < 4; ++slot) {
            CHECK(hasStore(caller, 8, slot * 8));
        }
    }
}

TEST_CASE("Apple AArch64 callers extend fixed narrow integer arguments") {
    const auto package = CompileToAArch64Lir(R"(
        func Narrow(signedByte: int8, unsignedShort: uint16) -> int {
            return 0;
        }

        func Main() -> int {
            return Narrow(-1, 65535);
        }
    )");

    AArch64RcuEmitter emitter(package, "test", Target::OS::MacOS);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    const auto beforeCall = std::ranges::subrange(caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call));

    // SXT[BH] writes the full X register for signed values; UXT[BH] writes W
    // and therefore zeroes the upper half. The destination fields are X0 and
    // W1, the registers in which the declared parameters travel.
    CHECK(
        std::ranges::any_of(beforeCall, [](const std::uint32_t word) { return (word & 0xFFFFFC1FU) == 0x93401C00U; }));
    CHECK(
        std::ranges::any_of(beforeCall, [](const std::uint32_t word) { return (word & 0xFFFFFC1FU) == 0x53003C01U; }));
}

TEST_CASE("AArch64 RCU emitter passes the first eight integer arguments in the registers AAPCS64 names") {
    const auto package = CompileToAArch64Lir(R"(
        func Eight(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int) -> int {
            return h;
        }

        func Main() -> int {
            return Eight(1, 2, 3, 4, 5, 6, 7, 8);
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // The caller fills X0 through X7 in order, and those eight loads are the
    // eight instructions the branch follows.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 8);
    for (unsigned reg = 0; reg < 8; ++reg) {
        const std::uint32_t word = caller[*call - 8 + reg];
        const auto loaded = ArgumentFilled(word);
        REQUIRE_MESSAGE(loaded.has_value(), HexWord(word));
        CHECK_EQ(*loaded, reg);
    }

    // Eight arguments need no stack of their own, so nothing moves the stack
    // pointer between the prologue that opened the frame and the branch.
    for (const auto word : caller) {
        CHECK_FALSE(StackPointerAdjustment(word, true).has_value());
        CHECK_FALSE(StackPointerAdjustment(word, false).has_value());
    }

    // The callee takes the same eight registers out of them in the same order
    // and before it does anything else, which is what makes every later mention
    // of a parameter a read of wherever it put it.
    const auto callee = FunctionWords(objects.front(), "Eight");
    const auto first = std::ranges::find_if(
        callee, [](const std::uint32_t w) { return ArgumentDrained(w) == std::optional<unsigned>(0); });
    REQUIRE(first != callee.end());
    const auto spills = static_cast<std::size_t>(first - callee.begin());
    REQUIRE_GE(callee.size(), spills + 8);
    for (unsigned reg = 0; reg < 8; ++reg) {
        const std::uint32_t word = callee[spills + reg];
        const auto drained = ArgumentDrained(word);
        REQUIRE_MESSAGE(drained.has_value(), HexWord(word));
        CHECK_EQ(*drained, reg);
    }
}

TEST_CASE("AArch64 RCU emitter sends the ninth argument and everything past it on the stack") {
    const auto package = CompileToAArch64Lir(R"(
        func Ten(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int, i: int, j: uint16) -> uint16 {
            return j;
        }

        func Main() -> int {
            var result = Ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Two arguments past the eighth take a doubleword each, and the area they
    // sit in is rounded to the sixteen bytes the stack pointer is a multiple of.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    // Five instructions open the area and fill it, and the eight argument
    // registers are loaded after them, so the branch is the fourteenth.
    REQUIRE_GE(*call, 13);
    const std::size_t open = *call - 13;
    const auto opened = StackPointerAdjustment(caller[open], true);
    REQUIRE_MESSAGE(opened.has_value(), HexWord(caller[open]));
    CHECK_EQ(*opened, 16);
    REQUIRE_LT(*call + 1, caller.size());
    const auto closed = StackPointerAdjustment(caller[*call + 1], false);
    REQUIRE_MESSAGE(closed.has_value(), HexWord(caller[*call + 1]));
    CHECK_EQ(*closed, 16);

    // Both stack arguments are written a whole doubleword at a time, so the
    // narrow one occupies its slot's low bytes and leaves the next slot alone.
    CHECK_EQ(HexWord(caller[open + 2]), HexWord(0xF90003E9U)); // str x9, [sp]
    CHECK_EQ(HexWord(caller[open + 4]), HexWord(0xF90007E9U)); // str x9, [sp, #8]
    // The narrow one is read out of its own slot at its own width before it is
    // written out at the stack slot's.
    CHECK_EQ(HexWord(caller[open + 3] & 0xFFC003FFU), HexWord(0x794003A9U)); // ldrh w9, [x29]

    // The callee finds them directly above its own frame: the frame record sits
    // at the bottom of the frame, so what the caller wrote at its stack pointer
    // is at X29 plus the frame size.
    const auto callee = FunctionWords(objects.front(), "Ten");
    REQUIRE_GT(callee.size(), 12);
    const auto frame = PreIndexedFrameSize(callee.front());
    REQUIRE_MESSAGE(frame.has_value(), HexWord(callee.front()));
    const auto reads = [&callee](const unsigned width, const std::int32_t displacement) {
        return std::ranges::any_of(callee, [width, displacement](const std::uint32_t w) {
            return IncomingDisplacement(w, width) == std::optional<std::int32_t>(displacement);
        });
    };
    CHECK(reads(8, *frame));
    // A narrow one is read at the width its own type occupies rather than a
    // whole doubleword, since a C caller leaves the bytes above it as it found
    // them.
    CHECK(reads(2, *frame + 8));
}

TEST_CASE("AArch64 RCU emitter returns a value in X0 and extends a narrow one on the way out") {
    const auto package = CompileToAArch64Lir(R"(
        func Byte(a: uint8, b: uint8) -> uint8 {
            return a + b;
        }

        func Short(a: int16) -> int16 {
            return a;
        }

        func Main() -> int {
            var wrapped = Byte(200, 100);
            var negative = Short(-3);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // What fills X0 extends by the returned type, which is the whole of what
    // AAPCS64 asks a callee to do: unsigned zeroes the register above the
    // value, signed sign-extends it. The value being returned lives in a
    // register the allocation gave it, so the extension is out of one rather
    // than out of a slot — the same instruction either way.
    const auto byteReturn = FunctionWords(objects.front(), "Byte");
    CHECK_MESSAGE(
        std::ranges::any_of(byteReturn, [](const std::uint32_t w) { return (w & 0xFFFFFC1FU) == 0x53001C00U; }),
        "uxtb w0, wN");
    const auto shortReturn = FunctionWords(objects.front(), "Short");
    CHECK_MESSAGE(
        std::ranges::any_of(shortReturn, [](const std::uint32_t w) { return (w & 0xFFFFFC1FU) == 0x93403C00U; }),
        "sxth x0, wN");

    // The caller keeps what came back and writes only the bytes the type
    // occupies into the local it belongs to, so nothing it does afterwards
    // depends on the bits above them.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_LT(*call + 2, caller.size());
    const auto kept = ArgumentDrained(caller[*call + 1]);
    REQUIRE_MESSAGE(kept == std::optional<unsigned>(0), HexWord(caller[*call + 1]));
    const std::uint32_t result = caller[*call + 1] & 0x1FU;
    CHECK_EQ(HexWord(caller[*call + 2] & 0xFFC0001FU), HexWord(0x39000000U | result)); // strb wN, [xM]
}

TEST_CASE("AArch64 RCU emitter branches to a function this module defines through a relocation") {
    const auto package = CompileToAArch64Lir(R"(
        func Callee() -> int {
            return 7;
        }

        func Main() -> int {
            return Callee();
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // The branch is emitted with no displacement at all: where Callee ended up
    // is the relocation's answer rather than this generator's.
    const auto caller = FunctionWords(object, "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    CHECK_EQ(HexWord(caller[*call]), HexWord(0x94000000U)); // bl #0

    const RcuSymbol *main = FindSymbol(object, "Main");
    REQUIRE(main != nullptr);
    const auto relocs = RelocsFor(object, RCU_TEXT_IDX, "Callee");
    REQUIRE_EQ(relocs.size(), 1);
    CHECK_EQ(relocs.front().type, RcuRelType::AArch64Call26);
    CHECK_EQ(relocs.front().sectionOffset, main->value + *call * 4);
    CHECK_EQ(TextWordAt(object, relocs.front().sectionOffset), 0x94000000U);
}

TEST_CASE("AArch64 RCU emitter calls through a register when the callee is a value") {
    const auto package = CompileToAArch64Lir(R"(
        func Add(a: int, b: int) -> int {
            return a + b;
        }

        func Apply(f: func(int, int) -> int, a: int, b: int) -> int {
            return f(a, b);
        }

        func Main() -> int {
            return Apply(Add, 1, 2);
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto words = FunctionWords(objects.front(), "Apply");
    const auto indirect = std::ranges::find(words, 0xD63F0120U); // blr x9
    REQUIRE_MESSAGE(indirect != words.end(), "blr x9");
    const auto index = static_cast<std::size_t>(indirect - words.begin());
    REQUIRE_GE(index, 3);

    // The address is fetched after the argument registers and into X9 rather
    // than one of them, so fetching it cannot disturb what it is called with.
    CHECK_EQ(ArgumentFilled(words[index - 3]), std::optional<unsigned>(0));
    CHECK_EQ(ArgumentFilled(words[index - 2]), std::optional<unsigned>(1));
    CHECK_EQ(ArgumentFilled(words[index - 1]), std::optional<unsigned>(9));

    // Nothing names a symbol: an indirect call has no target to relocate.
    CHECK_FALSE(BranchAndLinkIndex(words).has_value());
}

TEST_CASE("AArch64 RCU emitter carries the library an extern declaration names to its symbol") {
    const auto package = CompileToAArch64Lir(R"(
        #Link("libc.so.6")
        extern {
            func abs(n: int32) -> int32;
        }

        func Main() -> int {
            var magnitude = abs(-5);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // The declaration was predeclared with its library, so the call site found
    // that symbol rather than creating a second one without it.
    const RcuSymbol *symbol = FindSymbol(object, "abs");
    REQUIRE(symbol != nullptr);
    CHECK_EQ(symbol->kind, RcuSymKind::ExternFunc);
    CHECK_EQ(symbol->visibility, RcuSymVis::Global);
    CHECK_EQ(symbol->sectionIdx, RCU_SEC_EXTERNAL);
    CHECK_EQ(symbol->typeName, "libc.so.6");

    const auto relocs = RelocsFor(object, RCU_TEXT_IDX, "abs");
    REQUIRE_EQ(relocs.size(), 1);
    CHECK_EQ(relocs.front().type, RcuRelType::AArch64Call26);
    CHECK_EQ(TextWordAt(object, relocs.front().sectionOffset), 0x94000000U); // bl #0
}

TEST_CASE("AArch64 RCU emitter ends a path at a call that does not return") {
    const auto package = CompileToAArch64Lir(R"(
        #NoReturn()
        func Die() {
            Die();
        }

        func Main() -> int {
            Die();
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // The branch is the last thing on the path and the trap is what stands
    // where a fall-through would have been: no epilogue is emitted after a call
    // control does not come back from.
    const auto words = FunctionWords(objects.front(), "Main");
    REQUIRE_GE(words.size(), 2);
    CHECK_EQ(HexWord(words.back()), HexWord(0x00000000U));                          // udf #0
    CHECK_EQ(HexWord(words[words.size() - 2] & 0xFC000000U), HexWord(0x94000000U)); // bl
    CHECK_EQ(std::ranges::count(words, 0xD65F03C0U), 0);                            // ret

    // The function it names is the same shape, and is the whole of it: a frame
    // record, the branch and the trap.
    const auto callee = FunctionWords(objects.front(), "Die");
    REQUIRE_EQ(callee.size(), 4);
    CHECK_EQ(HexWord(callee.back()), HexWord(0x00000000U)); // udf #0
}

TEST_CASE("AArch64 RCU emitter passes the first eight floats in the vector registers and returns in V0") {
    const auto package = CompileToAArch64Lir(R"(
        func Eight(a: float64, b: float64, c: float64, d: float64,
                   e: float64, f: float64, g: float64, h: float64) -> float64 {
            return h;
        }

        func Main() -> int {
            var last = Eight(1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // The caller fills D0 through D7 in order, and those eight loads are the
    // eight instructions the branch follows. Nothing touches the stack: the
    // vector file carries all eight on its own.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 8);
    for (unsigned reg = 0; reg < 8; ++reg) {
        const std::uint32_t word = caller[*call - 8 + reg];
        const auto filled = VectorArgumentFilled(word, 64);
        REQUIRE_MESSAGE(filled.has_value(), HexWord(word));
        CHECK_EQ(*filled, reg);
    }
    for (const auto word : caller) {
        CHECK_FALSE(StackPointerAdjustment(word, true).has_value());
    }

    // The callee spills the same eight, and answers in the register the first
    // argument arrived in.
    const auto callee = FunctionWords(objects.front(), "Eight");
    const std::size_t spills = VectorDrainStart(callee, 64);
    REQUIRE_GE(callee.size(), spills + 8);
    for (unsigned reg = 0; reg < 8; ++reg) {
        const std::uint32_t word = callee[spills + reg];
        const auto drained = VectorArgumentDrained(word, 64);
        REQUIRE_MESSAGE(drained.has_value(), HexWord(word));
        CHECK_EQ(*drained, reg);
    }
    CHECK(std::ranges::any_of(
        callee, [](const std::uint32_t w) { return VectorArgumentFilled(w, 64) == std::optional<unsigned>(0); }));
}

TEST_CASE("AArch64 RCU emitter counts the two register files apart") {
    const auto package = CompileToAArch64Lir(R"(
        func Mixed(a: int, b: float64, c: int, d: float32, e: int) -> float32 {
            return d;
        }

        func Main() -> int {
            var narrow = Mixed(1, 2.5, 3, 4.5f32, 5);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Neither file knows what the other has taken: the three integers are X0
    // through X2 and the two floats are V0 and V1, with the single-precision
    // one read at its own width.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 5);
    CHECK_EQ(ArgumentFilled(caller[*call - 5]), std::optional<unsigned>(0));
    CHECK_EQ(VectorArgumentFilled(caller[*call - 4], 64), std::optional<unsigned>(0));
    CHECK_EQ(ArgumentFilled(caller[*call - 3]), std::optional<unsigned>(1));
    CHECK_EQ(VectorArgumentFilled(caller[*call - 2], 32), std::optional<unsigned>(1));
    CHECK_EQ(ArgumentFilled(caller[*call - 1]), std::optional<unsigned>(2));

    // A float32 result comes back in S0, which is the same register the second
    // argument was passed in and a different width from it.
    const auto callee = FunctionWords(objects.front(), "Mixed");
    CHECK(std::ranges::any_of(
        callee, [](const std::uint32_t w) { return VectorArgumentFilled(w, 32) == std::optional<unsigned>(0); }));
}

TEST_CASE("AArch64 RCU emitter passes a homogeneous float aggregate in consecutive vector registers") {
    const auto package = CompileToAArch64Lir(R"(
        struct Pair { x: float64; y: float64; }
        struct Quad { a: float32; b: float32; c: float32; d: float32; }

        func TakePair(p: Pair) -> float64 {
            return p.y;
        }

        func TakeQuad(q: Quad) -> float32 {
            return q.d;
        }

        func MakePair(v: float64) -> Pair {
            return Pair { x: v, y: v };
        }

        func Main() -> int {
            var pair = Pair { x: 1.5, y: 2.5 };
            var quad = Quad { a: 1.0f32, b: 2.0f32, c: 3.0f32, d: 4.0f32 };
            var y = TakePair(pair);
            var d = TakeQuad(quad);
            var made = MakePair(3.5);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Two doubles are D0 and D1, read eight bytes apart because that is how
    // wide one member is; four singles are S0 through S3, four bytes apart.
    // No register holds an aggregate, so an HFA is spilled to the frame
    // whatever the allocation did with the scalars around it.
    const auto pairCallee = FunctionWords(objects.front(), "TakePair");
    const std::size_t pairSpills = VectorDrainStart(pairCallee, 64);
    REQUIRE_GE(pairCallee.size(), pairSpills + 2);
    const auto first = VectorSlotAccessOf(pairCallee[pairSpills], 64, true);
    const auto second = VectorSlotAccessOf(pairCallee[pairSpills + 1], 64, true);
    REQUIRE_MESSAGE(first.has_value(), HexWord(pairCallee[pairSpills]));
    REQUIRE_MESSAGE(second.has_value(), HexWord(pairCallee[pairSpills + 1]));
    CHECK_EQ(first->reg, 0);
    CHECK_EQ(second->reg, 1);
    CHECK_EQ(second->displacement, first->displacement + 8);

    const auto quadCallee = FunctionWords(objects.front(), "TakeQuad");
    const std::size_t quadSpills = VectorDrainStart(quadCallee, 32);
    REQUIRE_GE(quadCallee.size(), quadSpills + 4);
    const auto base = VectorSlotAccessOf(quadCallee[quadSpills], 32, true);
    REQUIRE_MESSAGE(base.has_value(), HexWord(quadCallee[quadSpills]));
    for (unsigned member = 0; member < 4; ++member) {
        const auto spilled = VectorSlotAccessOf(quadCallee[quadSpills + member], 32, true);
        REQUIRE_MESSAGE(spilled.has_value(), HexWord(quadCallee[quadSpills + member]));
        CHECK_EQ(spilled->reg, member);
        CHECK_EQ(spilled->displacement, base->displacement + 4 * member);
    }

    // Sixteen bytes of floats come back in two vector registers rather than
    // through memory the caller named: an aggregate this large is returned
    // indirectly only when it is made of something else.
    const auto maker = FunctionWords(objects.front(), "MakePair");
    std::optional<VectorSlotAccess> returnedLow;
    std::optional<VectorSlotAccess> returnedHigh;
    for (std::size_t i = 0; i + 1 < maker.size(); ++i) {
        const auto low = VectorSlotAccessOf(maker[i], 64, false);
        const auto high = VectorSlotAccessOf(maker[i + 1], 64, false);
        if (low && high && low->reg == 0 && high->reg == 1) {
            returnedLow = low;
            returnedHigh = high;
        }
    }
    REQUIRE_MESSAGE(returnedLow.has_value(), "ldr d0, [x29, #imm]");
    REQUIRE_MESSAGE(returnedHigh.has_value(), "ldr d1, [x29, #imm]");
    CHECK_EQ(returnedHigh->displacement, returnedLow->displacement + 8);
    for (const auto word : maker) {
        CHECK_FALSE(ArgumentDrained(word) == std::optional<unsigned>(8));
    }
}

TEST_CASE("AArch64 RCU emitter carries a composite of no more than sixteen bytes in whole registers") {
    const auto package = CompileToAArch64Lir(R"(
        struct Small { a: int32; b: int32; }
        struct Mid { a: int64; b: int32; }

        func TakeSmall(s: Small) -> int32 {
            return s.b;
        }

        func TakeMid(m: Mid) -> int32 {
            return m.b;
        }

        func MakeMid(n: int64) -> Mid {
            return Mid { a: n, b: 7i32 };
        }

        func Main() -> int {
            var small = Small { a: 1i32, b: 2i32 };
            var mid = Mid { a: 3i64, b: 4i32 };
            var fromSmall = TakeSmall(small);
            var fromMid = TakeMid(mid);
            var made = MakeMid(5i64);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Eight bytes are one register, and twelve are two: a composite is broken
    // into whole doublewords, and the second of them carries the four bytes of
    // padding its slot was rounded up by rather than the value beside it.
    const auto smallCallee = FunctionWords(objects.front(), "TakeSmall");
    const std::size_t smallSpills = DrainStart(smallCallee);
    REQUIRE_GE(smallCallee.size(), smallSpills + 2);
    CHECK_FALSE(ArgumentDrained(smallCallee[smallSpills + 1]) == std::optional<unsigned>(1));

    const auto midCallee = FunctionWords(objects.front(), "TakeMid");
    const std::size_t midSpills = DrainStart(midCallee);
    REQUIRE_GE(midCallee.size(), midSpills + 2);
    CHECK_EQ(ArgumentDrained(midCallee[midSpills + 1]), std::optional<unsigned>(1));
    CHECK_EQ(SlotAccessDisplacement(midCallee[midSpills + 1]), SlotAccessDisplacement(midCallee[midSpills]) + 8);

    // The same shape backwards: twelve bytes come back in X0 and X1, and the
    // caller keeps both.
    const auto maker = FunctionWords(objects.front(), "MakeMid");
    bool returnsPair = false;
    for (std::size_t i = 0; i + 1 < maker.size(); ++i) {
        returnsPair = returnsPair || (ArgumentFilled(maker[i]) == std::optional<unsigned>(0) &&
                                      ArgumentFilled(maker[i + 1]) == std::optional<unsigned>(1));
    }
    CHECK(returnsPair);
}

TEST_CASE("AArch64 RCU emitter passes a composite past sixteen bytes as the address of a copy") {
    const auto package = CompileToAArch64Lir(R"(
        struct Big { a: int64; b: int64; c: int64; }

        func TakeBig(b: Big) -> int64 {
            return b.a;
        }

        func Main() -> int {
            var big = Big { a: 1i64, b: 2i64, c: 3i64 };
            var first = TakeBig(big);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Twenty-four bytes of copy, rounded to the sixteen the stack pointer is a
    // multiple of, and the address of that copy is what X0 carries.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 1);
    CHECK_EQ(StackPointerAddImm(caller[*call - 1], 0), std::optional<std::uint32_t>(0));
    std::optional<std::int64_t> opened;
    for (const auto word : caller) {
        if (const auto adjustment = StackPointerAdjustment(word, true); adjustment.has_value()) {
            opened = adjustment;
            break;
        }
    }
    CHECK_EQ(opened, std::optional<std::int64_t>(32));

    // The callee reads the copy into its own frame once and never writes
    // through the address again, so a parameter it modifies is its own.
    const auto callee = FunctionWords(objects.front(), "TakeBig");
    CHECK(std::ranges::find(callee, 0xAA0003EBU) != callee.end()); // mov x11, x0
    CHECK(std::ranges::any_of(callee, [](const std::uint32_t word) { return IsPairAccess(word); }));
}

TEST_CASE("AArch64 RCU emitter returns a large composite through the memory the caller names in X8") {
    const auto package = CompileToAArch64Lir(R"(
        struct Big { a: int64; b: int64; c: int64; }

        func MakeBig(n: int64) -> Big {
            return Big { a: n, b: n, c: n };
        }

        func Main() -> int {
            var big = MakeBig(4i64);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // The caller names the memory before it branches, and keeps nothing
    // afterwards: the callee has already written the whole value there.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 1);
    CHECK(FramePointerAddImm(caller[*call - 1], 8).has_value());
    REQUIRE_LT(*call + 1, caller.size());
    CHECK_FALSE(ArgumentDrained(caller[*call + 1]) == std::optional<unsigned>(0));

    // The callee keeps that address the way it keeps a parameter, and nothing
    // it returns travels in a register: no load ever fills X0.
    const auto callee = FunctionWords(objects.front(), "MakeBig");
    CHECK(std::ranges::any_of(callee,
                              [](const std::uint32_t w) { return ArgumentDrained(w) == std::optional<unsigned>(8); }));
    for (const auto word : callee) {
        CHECK_FALSE(ArgumentFilled(word) == std::optional<unsigned>(0));
    }
}

TEST_CASE("AArch64 RCU emitter leaves the general-purpose file behind once an argument overflows it") {
    const auto package = CompileToAArch64Lir(R"(
        struct Mid { a: int64; b: int32; }

        func Saturates(a: int, b: int, c: int, d: int, e: int, f: int, g: int, pair: Mid, last: int) -> int {
            return last;
        }

        func Main() -> int {
            var mid = Mid { a: 8i64, b: 9i32 };
            var result = Saturates(1, 2, 3, 4, 5, 6, 7, mid, 10);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Seven integers take X0 through X6, and the composite behind them needs
    // two registers where one is left — so it goes to the stack, and the
    // integer after it follows even though X7 is still free. That is the rule
    // the standard states as saturating the counter, and it is what a caller
    // written argument by argument gets wrong.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 7);
    for (unsigned reg = 0; reg < 7; ++reg) {
        CHECK_EQ(ArgumentFilled(caller[*call - 7 + reg]), std::optional<unsigned>(reg));
    }
    for (const auto word : caller) {
        CHECK_FALSE(ArgumentFilled(word) == std::optional<unsigned>(7));
    }

    // The callee finds both above its own frame: the composite in the first two
    // doublewords of the caller's area, and the integer in the third.
    const auto callee = FunctionWords(objects.front(), "Saturates");
    const auto frame = PreIndexedFrameSize(callee.front());
    REQUIRE_MESSAGE(frame.has_value(), HexWord(callee.front()));
    const auto found = std::ranges::find_if(callee, [frame](const std::uint32_t word) {
        return IncomingDisplacement(word, 8) == std::optional<std::int32_t>(*frame + 16);
    });
    CHECK_MESSAGE(found != callee.end(), "the ninth argument is read sixteen bytes into the area");
}

TEST_CASE("AArch64 RCU emitter leaves the vector file behind without touching the other one") {
    const auto package = CompileToAArch64Lir(R"(
        struct Pair { x: float64; y: float64; }

        func FivePairs(p: Pair, q: Pair, r: Pair, s: Pair, t: Pair, n: int) -> float64 {
            return t.y;
        }

        func Main() -> int {
            var pair = Pair { x: 1.5, y: 2.5 };
            var last = FivePairs(pair, pair, pair, pair, pair, 42);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Five pairs need ten vector registers where eight remain, so the fifth goes
    // to the stack whole — and the integer behind it still takes X0, because a
    // file that ran out says nothing about the other one.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 9);
    CHECK_EQ(ArgumentFilled(caller[*call - 1]), std::optional<unsigned>(0));
    for (unsigned member = 0; member < 8; ++member) {
        const std::uint32_t word = caller[*call - 9 + member];
        const auto loaded = VectorSlotAccessOf(word, 64, false);
        REQUIRE_MESSAGE(loaded.has_value(), HexWord(word));
        CHECK_EQ(loaded->reg, member);
    }

    // Sixteen bytes of stack for the pair that did not fit, and nothing more:
    // the integer went to X0 rather than to the area behind it.
    std::optional<std::int64_t> opened;
    for (const auto word : caller) {
        if (const auto adjustment = StackPointerAdjustment(word, true); adjustment.has_value()) {
            opened = adjustment;
            break;
        }
    }
    CHECK_EQ(opened, std::optional<std::int64_t>(16));
}

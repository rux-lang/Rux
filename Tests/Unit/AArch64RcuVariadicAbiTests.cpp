// AArch64 platform-specific C variadic call lowering.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <doctest.h>
#include <optional>
#include <string>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

TEST_CASE("Apple AArch64 variadic calls keep fixed arguments in registers and promote the stack tail") {
    const auto package = CompileToAArch64Lir(R"(
        struct Wide { first: int64; second: int64; }

        #Link("libSystem.B.dylib")
        extern {
            func Format(format: *char8, scale: float64, ...) -> int32;
        }

        func Main() -> int {
            var narrow: int16 = -2i16;
            var byte: uint8 = 3u8;
            var wide = Wide { first: 4i64, second: 5i64 };
            Format(null, 1.5, 2.5f32, narrow, byte, wide);
            return 0;
        }
    )",
                                             "macos-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::MacOS);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    const auto beforeCall = std::ranges::subrange(caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call));

    // The declared pointer and double use X0 and V0. The anonymous f32 was
    // promoted to f64 during lowering and is stored at stack offset zero;
    // neither it nor the integer promotions consume another argument register.
    CHECK(std::ranges::any_of(
        beforeCall, [](const std::uint32_t word) { return ArgumentFilled(word) == std::optional<unsigned>(0); }));
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return VectorArgumentFilled(word, 64) == std::optional<unsigned>(0);
    }));
    for (unsigned reg = 1; reg < 8; ++reg) {
        CHECK_FALSE(std::ranges::any_of(beforeCall, [reg](const std::uint32_t word) {
            return ArgumentFilled(word) == std::optional<unsigned>(reg) ||
                   VectorArgumentFilled(word, 32) == std::optional<unsigned>(reg) ||
                   VectorArgumentFilled(word, 64) == std::optional<unsigned>(reg);
        }));
    }
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return VectorStackArgumentStored(word, 64) == std::optional<std::uint32_t>(0);
    }));
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return StackArgumentStoredBy(word, 4, 9) == std::optional<std::uint32_t>(8);
    }));
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return StackArgumentStoredBy(word, 4, 9) == std::optional<std::uint32_t>(16);
    }));
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return StackPairArgumentStored(word) == std::optional<std::uint32_t>(24);
    }));
    CHECK(std::ranges::any_of(caller, [](const std::uint32_t word) {
        return StackPointerAdjustment(word, true) == std::optional<std::int64_t>(48);
    }));
}

TEST_CASE("Apple AArch64 variadic boundary follows packed fixed stack arguments") {
    const auto package = CompileToAArch64Lir(R"(
        struct Wide { first: int64; second: int64; }

        #Link("libSystem.B.dylib")
        extern {
            func Collect(a: int64, b: int64, c: int64, d: int64,
                         e: int64, f: int64, g: int64, h: int64,
                         small: uint8, medium: uint16, ...) -> int32;
        }

        func Main() -> int {
            var wide = Wide { first: 11i64, second: 12i64 };
            Collect(1i64, 2i64, 3i64, 4i64, 5i64, 6i64, 7i64, 8i64, 9u8, 10u16, wide);
            return 0;
        }
    )",
                                             "macos-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::MacOS);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    const auto beforeCall = std::ranges::subrange(caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call));
    for (unsigned reg = 0; reg < 8; ++reg) {
        CHECK(std::ranges::any_of(beforeCall, [reg](const std::uint32_t word) {
            return ArgumentFilled(word) == std::optional<unsigned>(reg);
        }));
    }

    // Apple packs the two fixed stack arguments at offsets 0 and 2. The
    // anonymous boundary rounds that packed prefix to eight; the 16-byte value
    // then occupies the two consecutive slots beginning there.
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return StackArgumentStoredBy(word, 1, 9) == std::optional<std::uint32_t>(0);
    }));
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return StackArgumentStoredBy(word, 2, 9) == std::optional<std::uint32_t>(2);
    }));
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return StackPairArgumentStored(word) == std::optional<std::uint32_t>(8);
    }));
    CHECK(std::ranges::any_of(caller, [](const std::uint32_t word) {
        return StackPointerAdjustment(word, true) == std::optional<std::int64_t>(32);
    }));
}

TEST_CASE("Apple AArch64 zero-fixed variadic calls restart their stack layout") {
    const auto package = CompileToAArch64Lir(R"(
        #Link("libSystem.B.dylib")
        extern func Log(...) -> int32;

        func Main() -> int {
            Log(1, 2.5);
            Log(3, 4.5);
            return 0;
        }
    )",
                                             "macos-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::MacOS);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto caller = FunctionWords(objects.front(), "Main");
    const auto calls = BranchAndLinkIndices(caller);
    REQUIRE_EQ(calls.size(), 2);
    for (const std::size_t call : calls) {
        const auto opened =
            std::find_if(caller.rbegin() + static_cast<std::ptrdiff_t>(caller.size() - call), caller.rend(),
                         [](const std::uint32_t word) { return StackPointerAdjustment(word, true).has_value(); });
        REQUIRE(opened != caller.rend());
        CHECK_EQ(StackPointerAdjustment(*opened, true), std::optional<std::int64_t>(16));
    }
    CHECK_EQ(std::ranges::count_if(caller,
                                   [](const std::uint32_t word) {
                                       return StackArgumentStoredBy(word, 8, 9) == std::optional<std::uint32_t>(0);
                                   }),
             2);
    CHECK_EQ(std::ranges::count_if(caller,
                                   [](const std::uint32_t word) {
                                       return VectorStackArgumentStored(word, 64) == std::optional<std::uint32_t>(8);
                                   }),
             2);
}

TEST_CASE("FreeBSD AArch64 variadic calls promote their tail and retain generic register placement") {
    const auto source = R"(
        #Link("libc.so.7")
        extern {
            func Collect(tag: *char8, scale: float64, ...) -> int32;
        }

        func Main() -> int {
            var tag = "values";
            var fraction: float32 = 2.5f32;
            var negative: int8 = -3i8;
            var positive: uint16 = 4u16;
            Collect(tag.data, 1.5, fraction, negative, positive);
            Collect(tag.data, 5.5, fraction, negative, positive);
            return 0;
        }
    )";

    const auto package = CompileToAArch64Lir(source, "freebsd-aarch64");
    std::size_t metadataCalls = 0;
    for (const auto &function : package.modules.front().funcs) {
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.op == LirOpcode::Call && instruction.strArg == "Collect") {
                    ++metadataCalls;
                    CHECK(instruction.isCVariadic);
                    CHECK_EQ(instruction.cVariadicFixedParamCount, std::optional<std::uint32_t>(2));
                }
            }
        }
    }
    CHECK_EQ(metadataCalls, 2);

    AArch64RcuEmitter emitter(package, "test", Target::OS::FreeBSD);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto calls = BranchAndLinkIndices(caller);
    REQUIRE_EQ(calls.size(), 2);

    // Each call independently restarts both AAPCS64 register files. The fixed
    // pointer takes X0 and the fixed float takes V0. Float32 is promoted to
    // float64 and remains in V1; both narrow integers are promoted to int32 and
    // occupy X1/X2. No anonymous value is forced to Apple's stack tail or
    // copied by bit pattern into Windows' general-purpose file.
    std::size_t previous = 0;
    for (const std::size_t call : calls) {
        const auto callSetup = std::ranges::subrange(caller.begin() + static_cast<std::ptrdiff_t>(previous),
                                                     caller.begin() + static_cast<std::ptrdiff_t>(call));
        for (const unsigned reg : {0U, 1U, 2U}) {
            CHECK(std::ranges::any_of(callSetup, [reg](const std::uint32_t word) {
                return ArgumentFilled(word) == std::optional<unsigned>(reg);
            }));
        }
        for (const unsigned reg : {0U, 1U}) {
            CHECK(std::ranges::any_of(callSetup, [reg](const std::uint32_t word) {
                return VectorArgumentFilled(word, 64) == std::optional<unsigned>(reg);
            }));
        }
        previous = call + 1;
    }
    CHECK(HasFloatForm(caller, 0x1E22C000U, 1)); // fcvt dN, sM
    CHECK_FALSE(std::ranges::any_of(caller, [](const std::uint32_t word) {
        return FloatBitsArgumentFilled(word, 32).has_value() || FloatBitsArgumentFilled(word, 64).has_value();
    }));
    CHECK_FALSE(std::ranges::any_of(
        caller, [](const std::uint32_t word) { return StackPointerAdjustment(word, true).has_value(); }));

    const auto applePackage = CompileToAArch64Lir(source, "macos-aarch64");
    AArch64RcuEmitter appleEmitter(applePackage, "test", Target::OS::MacOS);
    const auto appleObjects = appleEmitter.Generate();
    CHECK_MESSAGE(appleEmitter.Diagnostics().empty(), JoinMessages(appleEmitter.Diagnostics()));
    const auto appleCaller = FunctionWords(appleObjects.front(), "Main");
    CHECK(std::ranges::any_of(appleCaller,
                              [](const std::uint32_t word) { return StackPointerAdjustment(word, true).has_value(); }));

    const auto windowsPackage = CompileToAArch64Lir(source, "windows-aarch64");
    AArch64RcuEmitter windowsEmitter(windowsPackage, "test", Target::OS::Windows);
    const auto windowsObjects = windowsEmitter.Generate();
    CHECK_MESSAGE(windowsEmitter.Diagnostics().empty(), JoinMessages(windowsEmitter.Diagnostics()));
    const auto windowsCaller = FunctionWords(windowsObjects.front(), "Main");
    CHECK(std::ranges::any_of(windowsCaller, [](const std::uint32_t word) {
        return FloatBitsArgumentFilled(word, 32).has_value() || FloatBitsArgumentFilled(word, 64).has_value();
    }));
}

TEST_CASE("AArch64 emitter rejects inconsistent C variadic call metadata") {
    const auto makePackage = [] {
        return CompileToAArch64Lir(R"(
            #Link("libSystem.B.dylib")
            extern func Log(value: int32, ...) -> int32;

            func Main() -> int {
                Log(1, 2);
                return 0;
            }
        )",
                                   "macos-aarch64");
    };
    const auto findCall = [](LirPackage &package) -> LirInstr & {
        for (auto &function : package.modules.front().funcs) {
            for (auto &block : function.blocks) {
                for (auto &instruction : block.instrs) {
                    if (instruction.op == LirOpcode::Call && instruction.strArg == "Log") {
                        return instruction;
                    }
                }
            }
        }
        FAIL("expected a direct call to Log");
        return package.modules.front().funcs.front().blocks.front().instrs.front();
    };

    auto missingCount = makePackage();
    findCall(missingCount).cVariadicFixedParamCount.reset();
    AArch64RcuEmitter missingEmitter(missingCount, "test", Target::OS::MacOS);
    (void)missingEmitter.Generate();
    CHECK(JoinMessages(missingEmitter.Diagnostics()).find("inconsistent C variadic call metadata") !=
          std::string::npos);

    auto excessiveCount = makePackage();
    findCall(excessiveCount).cVariadicFixedParamCount = 3;
    AArch64RcuEmitter excessiveEmitter(excessiveCount, "test", Target::OS::MacOS);
    (void)excessiveEmitter.Generate();
    CHECK(JoinMessages(excessiveEmitter.Diagnostics()).find("invalid C variadic fixed-parameter count") !=
          std::string::npos);
}

TEST_CASE("Windows AArch64 sprintf-style variadic calls use consecutive general-purpose slots") {
    const auto package = CompileToAArch64Lir(R"(
        #Link("ucrtbase.dll")
        extern {
            func sprintf(buffer: *char8, format: *char8, ...) -> int32;
        }

        func Main() -> int {
            var buffer = "result";
            var format = "values";
            var written = sprintf(buffer.data, format.data, 2.5, 7, 3.5f32);
            return 0;
        }
    )",
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());

    // The two fixed pointers and all three anonymous arguments occupy one
    // eight-byte slot each. Both floating-point values cross into X registers
    // by bit pattern; no V argument register receives either one.
    const auto beforeCall = std::ranges::subrange(caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call));
    CHECK(std::ranges::any_of(
        beforeCall, [](const std::uint32_t word) { return ArgumentFilled(word) == std::optional<unsigned>(0); }));
    CHECK(std::ranges::any_of(
        beforeCall, [](const std::uint32_t word) { return ArgumentFilled(word) == std::optional<unsigned>(1); }));
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return FloatBitsArgumentFilled(word, 64) == std::optional<unsigned>(2);
    }));
    CHECK(std::ranges::any_of(
        beforeCall, [](const std::uint32_t word) { return ArgumentFilled(word) == std::optional<unsigned>(3); }));
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return FloatBitsArgumentFilled(word, 32) == std::optional<unsigned>(4);
    }));
    for (const auto word : caller) {
        for (const unsigned bits : {32U, 64U}) {
            const auto vector = VectorArgumentFilled(word, bits);
            CHECK(vector.value_or(8) >= 8);
        }
    }
}

TEST_CASE("Windows AArch64 variadic aggregates straddle the register window and stack") {
    const auto package = CompileToAArch64Lir(R"(
        struct FloatPair { x: float64; y: float64; }
        struct IntPair { x: int64; y: int64; }

        #Link("variadic.dll")
        extern {
            func Collect(first: int, ...) -> int32;
        }

        func Main() -> int {
            var floats = FloatPair { x: 1.5, y: 2.5 };
            var integers = IntPair { x: 8i64, y: 9i64 };
            var result = Collect(1, 2, 3, 4, 5, 6, 7, floats, integers);
            return 0;
        }
    )",
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());

    // Seven scalar slots fill X0-X6. The HFA receives no special treatment:
    // its first doubleword fills X7 and its second becomes stack slot zero.
    // The ordinary aggregate follows in stack slots eight and sixteen.
    for (unsigned reg = 0; reg < 8; ++reg) {
        CHECK(std::ranges::any_of(
            caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call),
            [reg](const std::uint32_t word) { return ArgumentFilled(word) == std::optional<unsigned>(reg); }));
    }
    std::vector<std::uint32_t> stackOffsets;
    for (std::size_t i = 0; i < *call; ++i) {
        if (const auto offset = StackArgumentStored(caller[i])) {
            stackOffsets.push_back(*offset);
        }
        CHECK_FALSE(VectorArgumentFilled(caller[i], 64).value_or(8) < 8);
    }
    CHECK_EQ(stackOffsets, std::vector<std::uint32_t>({0, 8, 16}));

    // Twenty-four real argument bytes are rounded so SP remains aligned at the
    // public call boundary.
    CHECK(std::ranges::any_of(caller, [](const std::uint32_t word) {
        return StackPointerAdjustment(word, true) == std::optional<std::int64_t>(32);
    }));
}

TEST_CASE("Windows AArch64 variadic calls copy large aggregates and keep indirect returns in X8") {
    const auto package = CompileToAArch64Lir(R"(
        struct Big { a: int64; b: int64; c: int64; }

        #Link("variadic.dll")
        extern {
            func Transform(scale: float64, ...) -> Big;
        }

        func Main() -> int {
            var input = Big { a: 1i64, b: 2i64, c: 3i64 };
            var output = Transform(2.5, input);
            return 0;
        }
    )",
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 3);

    // The named float still uses the general file, and the aggregate is copied
    // into the outgoing area with its address in the next consecutive slot.
    CHECK(std::ranges::any_of(
        caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call),
        [](const std::uint32_t word) { return FloatBitsArgumentFilled(word, 64) == std::optional<unsigned>(0); }));
    CHECK(std::ranges::any_of(
        caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call),
        [](const std::uint32_t word) { return StackPointerAddImm(word, 1) == std::optional<std::uint32_t>(0); }));

    // Return classification is independent of the variadic argument variant:
    // the caller names its large result in X8 immediately before the branch.
    CHECK(FramePointerAddImm(caller[*call - 1], 8).has_value());
    REQUIRE_LT(*call + 1, caller.size());
    CHECK_FALSE(ArgumentDrained(caller[*call + 1]) == std::optional<unsigned>(0));
    CHECK(std::ranges::any_of(caller, [](const std::uint32_t word) {
        return StackPointerAdjustment(word, true) == std::optional<std::int64_t>(32);
    }));
}

TEST_CASE("AArch64 RCU emitter passes an anonymous float argument in a vector register") {
    // AAPCS64 states no separate rule for the arguments a variadic declaration
    // does not name: on Linux a float still travels in the vector file, which
    // is where `va_arg` reads it back from. Apple and Windows deviate and
    // neither is reachable through this back end yet.
    const auto package = CompileToAArch64Lir(R"(
        #Link("libc.so.6")
        extern {
            func printf(format: *char8, ...) -> int32;
        }

        func Main() -> int {
            var text = "value";
            var written = printf(text.data, 2.5, 7);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 3);
    CHECK_EQ(ArgumentFilled(caller[*call - 3]), std::optional<unsigned>(0));
    const auto anonymous = VectorArgumentFilled(caller[*call - 2], 64);
    REQUIRE_MESSAGE(anonymous.has_value(), HexWord(caller[*call - 2]));
    CHECK_EQ(*anonymous, 0);
    CHECK_EQ(ArgumentFilled(caller[*call - 1]), std::optional<unsigned>(1));
}

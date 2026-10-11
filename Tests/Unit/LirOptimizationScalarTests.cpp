#include "Optimization/LirMemoryPromotion.h"
#include "Optimization/LirScalarCombines.h"
#include "OptimizerTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::OptimizerTestSupport;

namespace {
std::size_t Count(const LirPackage &package, const LirOpcode opcode) {
    std::size_t count = 0;
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            for (const auto &block : function.blocks) {
                count += std::ranges::count_if(block.instrs,
                                               [&](const auto &instruction) { return instruction.op == opcode; });
            }
        }
    }
    return count;
}

LirPackage Promoted(const std::string &source) {
    auto package = CompileToLir(source, BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    static_cast<void>(promotion.Run(package, {}));
    Optimization::LirConstantPropagation constants;
    static_cast<void>(constants.Run(package, {}));
    return package;
}
} // namespace

TEST_CASE("Unsigned representation ranges remove only proven comparisons") {
    const auto source = R"(
        func Masked(value: uint32) -> bool { return ((value & 63u32) as uint64) < 64u64; }
        func Remainder(value: uint64) -> bool { return value % 17u64 >= 17u64; }
        func Add(value: uint8) -> bool { return ((value as uint64) + 5u64) <= 260u64; }
        func Multiply(value: uint8) -> bool { return (value as uint64) * 2u64 < 511u64; }
        func Wrap(value: uint64) -> bool { return value + 1u64 > 0u64; }
        func Underflow(value: uint64) -> bool { return value - 1u64 < 18446744073709551615u64; }
        func Signed(value: int64) -> bool { return value < 0i64; }
        func Unknown(value: uint64, limit: uint64) -> bool { return value < limit; }
        func Narrow(value: uint64) -> bool { return (value as uint8) < 255u8; }
    )";
    auto package = Promoted(source);
    Optimization::LirScalarCombines combines;
    CHECK(combines.Run(package, {}) == Optimization::PassChange::Changed);
    for (const auto &function : package.modules[0].funcs) {
        std::size_t comparisons = 0;
        for (const auto &block : function.blocks)
            for (const auto &instruction : block.instrs)
                comparisons += instruction.op == LirOpcode::CmpLt || instruction.op == LirOpcode::CmpLe ||
                               instruction.op == LirOpcode::CmpGt || instruction.op == LirOpcode::CmpGe;
        CAPTURE(function.name);
        const bool proven = function.name == "Masked" || function.name == "Remainder" || function.name == "Add" ||
                            function.name == "Multiply";
        CHECK_EQ(comparisons, proven ? 0 : 1);
    }
}

TEST_CASE("Scalar combines drop a mask a logical shift already satisfies") {
    auto package = Promoted(R"(
        func Masked(value: uint64) -> uint64 { return (value << 16) | ((value >> 48) & 0xFFFFu64); }
        func Wider(value: uint32) -> uint32 { return (value >> 20) & 0xFFFFu32; }
        func Narrower(value: uint64) -> uint64 { return (value >> 48) & 0xFFu64; }
        func Arithmetic(value: int64) -> int64 { return (value >> 48) & 0xFFFFi64; }
    )");
    Optimization::LirScalarCombines combines;
    // The first round turns the masks into shifts; the second sees the complementary pair as a rotation.
    CHECK(combines.Run(package, {}) == Optimization::PassChange::Changed);
    static_cast<void>(combines.Run(package, {}));
    CHECK_EQ(Count(package, LirOpcode::RotateRight), 1);
    for (const auto &function : package.modules[0].funcs) {
        std::size_t masks = 0;
        for (const auto &block : function.blocks)
            for (const auto &instruction : block.instrs)
                masks += instruction.op == LirOpcode::And;
        CAPTURE(function.name);
        // A mask that clears shifted bits, and an arithmetic shift's sign fill, keep their and.
        CHECK_EQ(masks, function.name == "Narrower" || function.name == "Arithmetic" ? 1U : 0U);
    }
}

TEST_CASE("Scalar combines recognize only same-value complementary unsigned shifts") {
    auto package = Promoted(R"(
        func Rotate(value: uint64) -> uint64 { return (value >> 19) | (value << 45); }
        func Reverse(value: uint32) -> uint32 { return (value << 7) | (value >> 25); }
        func Different(a: uint64, b: uint64) -> uint64 { return (a >> 19) | (b << 45); }
        func Signed(value: int64) -> int64 { return (value >> 19) | (value << 45); }
        func Wrong(value: uint64) -> uint64 { return (value >> 19) | (value << 44); }
        func Variable(value: uint64, n: uint64) -> uint64 { return (value >> n) | (value << (64 - n)); }
        func Boundary(value: uint64) -> uint64 { return (value >> 0) | (value << 64); }
    )");
    Optimization::LirScalarCombines combines;
    CHECK(combines.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::RotateRight), 2);
    CHECK(combines.Run(package, {}) == Optimization::PassChange::None);
    auto verifier = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug);
    CHECK_FALSE(verifier.RunLir(package).HasErrors());
    RcuEmitter x86(package);
    CHECK_FALSE(x86.Generate().empty());
    CHECK(x86.Diagnostics().empty());
    AArch64RcuEmitter arm(package);
    CHECK_FALSE(arm.Generate().empty());
    CHECK(arm.Diagnostics().empty());
}

TEST_CASE("Power-of-two reduction preserves zero divisors and signed division") {
    auto package = Promoted(R"(
        func Divide(value: uint64) -> uint64 { return value / 8; }
        func Remainder(value: uint32) -> uint32 { return value % 16; }
        func Zero(value: uint64, zero: uint64) -> uint64 { return value / zero; }
        func Signed(value: int64) -> int64 { return value / 8; }
        func Other(value: uint64) -> uint64 { return value / 7; }
    )");
    Optimization::LirScalarCombines combines;
    CHECK(combines.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::Div), 3);
    CHECK_EQ(Count(package, LirOpcode::Mod), 0);
    CHECK_EQ(Count(package, LirOpcode::Lshr), 1);
    CHECK_EQ(Count(package, LirOpcode::And), 1);
}

TEST_CASE("CFG verifier rejects unsupported rotate widths and counts") {
    auto package = Promoted("func Identity(value: uint64) -> uint64 { return value; }");
    auto &function = package.modules[0].funcs[0];
    LirInstr rotate;
    rotate.op = LirOpcode::RotateRight;
    rotate.type = TypeRef::MakeUInt64();
    rotate.dst = 1000;
    rotate.srcs = {function.params[0].reg};
    rotate.strArg = "64";
    function.blocks[0].instrs.push_back(rotate);
    auto verifier = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug);
    CHECK(verifier.RunLir(package).HasErrors());
}

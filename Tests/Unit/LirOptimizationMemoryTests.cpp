#include "Optimization/LirConstantMemory.h"
#include "Optimization/LirMemoryPromotion.h"
#include "Optimization/LirScalarReplacement.h"
#include "Optimization/LirSsaVerifier.h"
#include "OptimizerTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::OptimizerTestSupport;

namespace {
LirPackage ConstantHeader(const TypeRef &element, const std::string &text) {
    LirConstDecl constant;
    constant.name = "Header";
    constant.type = TypeRef::MakeSlice(element);
    constant.elementType = element;
    constant.hasSequenceData = true;
    constant.isTextSlice = true;
    constant.text = text;
    auto function = ReturningFunction("Read");
    function.returnType = TypeRef::MakeUInt64();
    LirInstr address;
    address.op = LirOpcode::GlobalAddr;
    address.dst = 0;
    address.type = TypeRef::MakePointer(constant.type);
    address.strArg = constant.name;
    LirInstr field;
    field.op = LirOpcode::FieldPtr;
    field.dst = 1;
    field.type = TypeRef::MakePointer(TypeRef::MakeUInt64());
    field.srcs = {0};
    field.strArg = "length";
    LirInstr load;
    load.op = LirOpcode::Load;
    load.dst = 2;
    load.type = TypeRef::MakeUInt64();
    load.srcs = {1};
    function.blocks[0].instrs = {address, field, load};
    function.blocks[0].term = ReturnValue(2, load.type);
    LirModule module;
    module.consts = {constant};
    module.funcs = {function};
    LirPackage package;
    package.modules = {module};
    return package;
}
} // namespace

TEST_CASE("Immutable slice headers fold their encoded code-unit count") {
    for (const auto kind : {TypeRef::Kind::Char8, TypeRef::Kind::Char16, TypeRef::Kind::Char32}) {
        auto package = ConstantHeader(TypeRef::MakePrimitive(kind), "A\xF0\x9F\x98\x80");
        Optimization::LirConstantMemory memory;
        REQUIRE(memory.Run(package, {}) == Optimization::PassChange::Changed);
        const auto &load = package.modules[0].funcs[0].blocks[0].instrs.back();
        CHECK(load.op == LirOpcode::Const);
        CHECK(load.strArg == (kind == TypeRef::Kind::Char8 ? "5" : kind == TypeRef::Kind::Char16 ? "3" : "2"));
        CHECK(memory.Run(package, {}) == Optimization::PassChange::None);
    }
    for (const bool empty : {false, true}) {
        auto package = ConstantHeader(TypeRef::MakeUInt32(), "");
        auto &constant = package.modules[0].consts[0];
        constant.isTextSlice = false;
        if (!empty)
            constant.elements = {"1", "2", "3"};
        Optimization::LirConstantMemory memory;
        REQUIRE(memory.Run(package, {}) == Optimization::PassChange::Changed);
        CHECK(package.modules[0].funcs[0].blocks[0].instrs.back().strArg == (empty ? "0" : "3"));
    }
}

TEST_CASE("Constant memory folding rejects volatile mutable ambiguous and mismatched storage") {
    for (int mode = 0; mode < 10; ++mode) {
        auto package = ConstantHeader(TypeRef::MakePrimitive(TypeRef::Kind::Char16), "abc");
        auto &module = package.modules[0];
        auto &instructions = module.funcs[0].blocks[0].instrs;
        if (mode < 3)
            instructions[mode].isVolatile = true;
        if (mode == 3)
            module.consts[0].hasSequenceData = false;
        if (mode == 4)
            instructions[2].type = TypeRef::MakeUInt32();
        if (mode == 5)
            instructions[1].strArg = "data";
        if (mode == 6)
            module.consts.push_back(module.consts.front());
        if (mode == 7) {
            LirGlobal global;
            global.name = "Header";
            module.globals.push_back(global);
        }
        if (mode == 8)
            module.consts[0].text = "\xFF";
        if (mode == 9) {
            LirModule other;
            other.consts = module.consts;
            module.consts.clear();
            package.modules.push_back(other);
        }
        Optimization::LirConstantMemory memory;
        CHECK(memory.Run(package, {}) == Optimization::PassChange::None);
    }
}

namespace {
std::size_t Count(const LirPackage &package, const LirOpcode op) {
    std::size_t count = 0;
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            for (const auto &block : function.blocks) {
                count +=
                    std::ranges::count_if(block.instrs, [&](const auto &instruction) { return instruction.op == op; });
            }
        }
    }
    return count;
}

LirPackage Storage(const bool initialized, const bool volatileStore = false, const bool mixedWidth = false) {
    auto function = ReturningFunction("Storage");
    LirInstr slot;
    slot.op = LirOpcode::Alloca;
    slot.dst = 0;
    slot.type = TypeRef::MakeInt32();
    function.blocks[0].instrs.push_back(slot);
    LirInstr value;
    value.op = LirOpcode::Const;
    value.dst = 1;
    value.type = mixedWidth ? TypeRef::MakeInt64() : slot.type;
    value.strArg = "42";
    function.blocks[0].instrs.push_back(value);
    if (initialized) {
        LirInstr store;
        store.op = LirOpcode::Store;
        store.type = slot.type;
        store.srcs = {1, 0};
        store.isVolatile = volatileStore;
        function.blocks[0].instrs.push_back(store);
    }
    LirInstr load;
    load.op = LirOpcode::Load;
    load.type = slot.type;
    load.dst = 2;
    load.srcs = {0};
    function.blocks[0].instrs.push_back(load);
    function.blocks[0].term = ReturnValue(2, slot.type);
    LirModule module;
    module.funcs.push_back(function);
    LirPackage package;
    package.modules.push_back(module);
    return package;
}
} // namespace

TEST_CASE("Memory promotion forms loop-carried SSA values without memory traffic") {
    auto package = CompileToLir(R"(
        func Sum(n: int) -> int {
            var i = 0;
            var sum = 0;
            while i < n {
                let increment = i + 1;
                sum += increment;
                i += 1;
            }
            return sum;
        }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    CHECK(promotion.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::Alloca), 0);
    CHECK_EQ(Count(package, LirOpcode::Load), 0);
    CHECK_EQ(Count(package, LirOpcode::Store), 0);
    CHECK_GE(Count(package, LirOpcode::Phi), 2);
    auto verifier = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug);
    CHECK_FALSE(verifier.RunLir(package).HasErrors());
    CHECK(promotion.Run(package, {}) == Optimization::PassChange::None);
}

TEST_CASE("Memory promotion rewrites call arguments and branch merges") {
    auto package = CompileToLir(R"(
        func Identity(value: int) -> int { return value; }
        func Choose(test: bool, input: int) -> int {
            var result = input;
            if test { result = input + 2; } else { result = input - 3; }
            return Identity(result);
        }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    CHECK(promotion.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::Alloca), 0);
    CHECK_EQ(Count(package, LirOpcode::Load), 0);
    CHECK_EQ(Count(package, LirOpcode::Store), 0);
    CHECK_EQ(Count(package, LirOpcode::Call), 1);
}

TEST_CASE("Memory promotion preserves uninitialized volatile and mismatched storage") {
    Optimization::LirMemoryPromotion promotion;
    auto uninitialized = Storage(false);
    auto volatileWrite = Storage(true, true);
    auto mixed = Storage(true, false, true);
    for (auto *package : {&uninitialized, &volatileWrite, &mixed}) {
        CHECK(promotion.Run(*package, {}) == Optimization::PassChange::None);
        CHECK_EQ(Count(*package, LirOpcode::Alloca), 1);
        CHECK_EQ(Count(*package, LirOpcode::Load), 1);
    }
    auto direct = Storage(true);
    CHECK(promotion.Run(direct, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(*direct.modules[0].funcs[0].blocks[0].term->retVal, 1);
}

TEST_CASE("Memory promotion leaves escaped storage visible to calls") {
    auto package = CompileToLir(R"(
        func Set(value: *var int) { *value = 42; }
        func Main() -> int { var value = 1; Set(@value); return value; }
    )",
                                BuildProfile::Debug);
    Optimization::LirMemoryPromotion promotion;
    static_cast<void>(promotion.Run(package, {}));
    CHECK_GE(Count(package, LirOpcode::Alloca), 1);
    CHECK_GE(Count(package, LirOpcode::Load), 1);
    CHECK_GE(Count(package, LirOpcode::Store), 1);
}

TEST_CASE("Scalar replacement promotes private range descriptors and loop-local counters") {
    auto package = CompileToLir(R"(
        func Sum(n: int) -> int {
            var sum = 0;
            for i in 0..n {
                for j in 0..i { sum += j; }
            }
            return sum;
        }
    )",
                                BuildProfile::Debug);
    Optimization::LirScalarReplacement replacement;
    Optimization::LirMemoryPromotion promotion;
    CHECK(replacement.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(promotion.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::FieldPtr), 0);
    CHECK_EQ(Count(package, LirOpcode::Alloca), 0);
    CHECK_EQ(Count(package, LirOpcode::Load), 0);
    CHECK_EQ(Count(package, LirOpcode::Store), 0);
    auto verifier = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug);
    CHECK_FALSE(verifier.RunLir(package).HasErrors());
}

TEST_CASE("Scalar replacement preserves snapshots in private struct and tuple copy chains") {
    auto package = CompileToLir(R"(
        struct Pair { small: int8; large: int64; }
        func Work(input: int64) -> int64 {
            var first = Pair { small: 3i8, large: input };
            var second = first;
            first.large = 99;
            second.small = 7i8;
            var pair = (second.large, first.large);
            let copy = pair;
            pair.0 = 11;
            return copy.0 + copy.1 + (second.small as int64);
        }
    )",
                                BuildProfile::Debug);
    Optimization::LirScalarReplacement replacement;
    Optimization::LirMemoryPromotion promotion;
    CHECK(replacement.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK(promotion.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Count(package, LirOpcode::FieldPtr), 0);
    CHECK_EQ(Count(package, LirOpcode::Alloca), 0);
    CHECK_EQ(Count(package, LirOpcode::Load), 0);
    CHECK_EQ(Count(package, LirOpcode::Store), 0);
    CHECK(replacement.Run(package, {}) == Optimization::PassChange::None);
    Optimization::LirSsaVerifier verifier;
    std::vector<Diagnostic> diagnostics;
    Optimization::PassContext context;
    context.diagnostics = &diagnostics;
    static_cast<void>(verifier.Run(package, context));
    CHECK(diagnostics.empty());
}

TEST_CASE("Scalar replacement propagates representation escapes through aggregate copies") {
    auto package = CompileToLir(R"(
        struct Pair { small: int8; large: int64; }
        func Inspect(value: *Pair) -> int8 { return *(value as *int8); }
        func Work(input: int64) -> int64 {
            let first = Pair { small: 3i8, large: input };
            let second = first;
            return second.large + (Inspect(@second) as int64);
        }
    )",
                                BuildProfile::Debug);
    const auto allocations = Count(package, LirOpcode::Alloca);
    const auto loads = Count(package, LirOpcode::Load);
    Optimization::LirScalarReplacement replacement;
    CHECK(replacement.Run(package, {}) == Optimization::PassChange::None);
    CHECK_EQ(Count(package, LirOpcode::Alloca), allocations);
    CHECK_EQ(Count(package, LirOpcode::Load), loads);
}

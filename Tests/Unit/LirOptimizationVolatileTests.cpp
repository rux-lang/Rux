#include "Optimization/LirDeadCodeElimination.h"
#include "OptimizerTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::OptimizerTestSupport;

namespace {
LirPackage LocalRead(const TypeRef &type, const bool volatileRead, const bool used) {
    auto function = ReturningFunction("LocalRead");
    auto &block = function.blocks[0];

    LirInstr slot;
    slot.op = LirOpcode::Alloca;
    slot.dst = 0;
    slot.type = type;
    block.instrs.push_back(slot);

    LirInstr value;
    value.op = LirOpcode::Const;
    value.dst = 1;
    value.type = type;
    value.strArg = type.IsBool() ? "true" : "42";
    block.instrs.push_back(value);

    LirInstr store;
    store.op = LirOpcode::Store;
    store.type = type;
    store.srcs = {1, 0};
    block.instrs.push_back(store);

    LirInstr load;
    load.op = LirOpcode::Load;
    load.dst = 2;
    load.type = type;
    load.srcs = {0};
    load.isVolatile = volatileRead;
    block.instrs.push_back(load);

    if (used) {
        function.returnType = type;
        block.term = ReturnValue(2, type);
    }

    LirModule module;
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));
    return package;
}

std::size_t Loads(const LirPackage &package) {
    std::size_t count = 0;
    for (const auto &module : package.modules)
        for (const auto &function : module.funcs)
            for (const auto &block : function.blocks)
                count += std::ranges::count_if(block.instrs,
                                               [](const LirInstr &instruction) { return instruction.op == LirOpcode::Load; });
    return count;
}
} // namespace

TEST_CASE("Dead code elimination keeps unused volatile loads of unescaped local storage") {
    Optimization::LirDeadCodeElimination deadCode;
    for (const TypeRef &type : {TypeRef::MakeInt32(), TypeRef::MakeUInt64(), TypeRef::MakeBool()}) {
        auto package = LocalRead(type, true, false);
        CHECK(deadCode.Run(package, {}) == Optimization::PassChange::None);
        CHECK_EQ(Loads(package), 1U);
        const auto &instructions = package.modules[0].funcs[0].blocks[0].instrs;
        REQUIRE_EQ(instructions.size(), 4U);
        CHECK(instructions.back().isVolatile);
        CHECK(instructions[2].op == LirOpcode::Store);
    }
}

TEST_CASE("Dead code elimination still removes unused ordinary local loads") {
    Optimization::LirDeadCodeElimination deadCode;
    auto package = LocalRead(TypeRef::MakeInt32(), false, false);
    CHECK(deadCode.Run(package, {}) == Optimization::PassChange::Changed);
    CHECK_EQ(Loads(package), 0U);
    // Reading a local value for the return is not dead, even without volatility.
    auto used = LocalRead(TypeRef::MakeInt32(), false, true);
    CHECK(deadCode.Run(used, {}) == Optimization::PassChange::None);
    CHECK_EQ(Loads(used), 1U);
}

TEST_CASE("Dead code elimination retains volatile loads through nonlocal pointers") {
    auto function = ReturningFunction("PointerRead");
    LirParam parameter;
    parameter.reg = 0;
    parameter.type = TypeRef::MakePointer(TypeRef::MakeUInt64());
    parameter.name = "pointer";
    function.params.push_back(parameter);

    LirInstr load;
    load.op = LirOpcode::Load;
    load.dst = 1;
    load.type = TypeRef::MakeUInt64();
    load.srcs = {0};
    load.isVolatile = true;
    function.blocks[0].instrs.push_back(load);

    LirModule module;
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));
    Optimization::LirDeadCodeElimination deadCode;
    CHECK(deadCode.Run(package, {}) == Optimization::PassChange::None);
    CHECK_EQ(Loads(package), 1U);
}

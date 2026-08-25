// One representation per enum: the shape a variant is built in has to be the shape a match decodes, wherever the value
// came from. A generic enum wider than a word is an aggregate -- a tag at offset 0 and payloads after it -- and a
// value returned from a method is no different from one built in the same function.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
LirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "enums.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "enums.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    HirToLirLowering lowering(AstToHirLowering(model).Generate(), TargetContext::CreateNative());
    LirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

const LirFunc &RequireFunction(const LirPackage &package, const std::string &name) {
    REQUIRE_EQ(package.modules.size(), 1);
    for (const LirFunc &function : package.modules.front().funcs) {
        if (function.name == name) {
            return function;
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

std::vector<LirOpcode> OpcodesOf(const LirFunc &function) {
    std::vector<LirOpcode> opcodes;
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            opcodes.push_back(instruction.op);
        }
    }
    return opcodes;
}

bool Contains(const std::vector<LirOpcode> &opcodes, const LirOpcode op) {
    return std::find(opcodes.begin(), opcodes.end(), op) != opcodes.end();
}

/// A payload reached by byte offset from the enum's own storage is the aggregate representation; one reached by
/// shifting the tag out of a single word is the compact one.
bool ReadsPayloadByOffset(const LirFunc &function) {
    return Contains(OpcodesOf(function), LirOpcode::IndexPtr);
}

bool ReadsPayloadByShift(const LirFunc &function) {
    return Contains(OpcodesOf(function), LirOpcode::Shr);
}

const std::string kOptionPrelude = R"(
    enum Option<T> { Some(T), None }
    struct Counter { value: int32; limit: int32; }
    extend Counter {
        func Next(self: &var Counter) -> Option<int32> {
            if self.value >= self.limit { return Option::None<int32>(); }
            let current = self.value;
            self.value = self.value + 1i32;
            return Option::Some<int32>(current);
        }
    }
)";
} // namespace

TEST_CASE("a returned enum is matched in the representation it was built in") {
    const LirPackage package = LowerSource(kOptionPrelude + R"(
        func Main() -> int {
            var it = Counter { value: 0i32, limit: 3i32 };
            let a = match it.Next() { .Some(v) => v as int, .None => 90 };
            let b = match it.Next() { .Some(v) => v as int, .None => 90 };
            return a * 10 + b;
        }
    )");

    // The method builds the aggregate: every payload it writes is placed at an offset from the enum's storage.
    const LirFunc &next = RequireFunction(package, "Counter::Next");
    CHECK(ReadsPayloadByOffset(next));
    CHECK_FALSE(ReadsPayloadByShift(next));

    // Every match of what it returned decodes that same shape, however many times the method is called. Reading the
    // payload out of the upper half of a word instead decoded a value the method never wrote there, and the call
    // itself was made under an ABI too narrow to carry the returned aggregate.
    const LirFunc &main = RequireFunction(package, "Main");
    CHECK(ReadsPayloadByOffset(main));
    CHECK_FALSE(ReadsPayloadByShift(main));
}

TEST_CASE("a method's returned enum type carries the same layout as one built in place") {
    const LirPackage package = LowerSource(kOptionPrelude + R"(
        func Main() -> int {
            var it = Counter { value: 0i32, limit: 1i32 };
            let built = Option::Some<int32>(1i32);
            let returned = it.Next();
            return match built { .Some(v) => v as int, .None => 0 } +
                   match returned { .Some(v) => v as int, .None => 0 };
        }
    )");

    const LirFunc &main = RequireFunction(package, "Main");
    std::vector<TypeRef> optionSlots;
    for (const LirBlock &block : main.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Alloca && instruction.type.name == "Option<int32>") {
                optionSlots.push_back(instruction.type);
            }
        }
    }

    // A locally built value and a returned one are the same type, so they have to agree on their size: the returned
    // one used to arrive as a bare name with no layout at all, which read as a compact enum.
    REQUIRE_GE(optionSlots.size(), 2);
    for (const TypeRef &slot : optionSlots) {
        CHECK_EQ(slot.SizeInBytes(), optionSlots.front().SizeInBytes());
        CHECK_GT(slot.SizeInBytes().value_or(0), 8);
    }
}

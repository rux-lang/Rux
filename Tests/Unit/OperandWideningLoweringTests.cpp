// What an operator does with an operand narrower than itself: widen it, so the operation reads a value that was
// actually written rather than the storage beside it.

#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Rux;

namespace {
LirPackage CompileToLir(const std::string &source) {
    Lexer lexer(source, "widening.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "widening.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    HirToLirLowering lowering(AstToHirLowering(model).Generate(), CompileTimeContext{}.target);
    LirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

const LirFunc &RequireFunction(const LirPackage &package, const std::string &name) {
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

/// The type every source of the first instruction with this opcode is held at, in order.
std::vector<TypeRef> OperandTypes(const LirFunc &function, const LirOpcode op) {
    std::unordered_map<LirReg, TypeRef> produced;
    for (const LirParam &param : function.params) {
        produced.emplace(param.reg, param.type);
    }
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == op) {
                std::vector<TypeRef> types;
                for (const LirReg source : instruction.srcs) {
                    const auto found = produced.find(source);
                    types.push_back(found == produced.end() ? TypeRef::MakeOpaque() : found->second);
                }
                return types;
            }
            if (instruction.dst != LirNoReg) {
                produced.insert_or_assign(instruction.dst, instruction.type);
            }
        }
    }
    FAIL("missing instruction " << LirOpcodeName(op));
    throw std::runtime_error("missing instruction");
}
} // namespace

TEST_SUITE("OperandWideningLowering") {
    TEST_CASE("an untyped literal is added at the wide operand's width") {
        const LirPackage package = CompileToLir(R"(
            func Widen(value: int128) -> int128 {
                return value + 1;
            }
        )");

        // Both sides reach the add as int128. Left at `int`, the literal wrote eight bytes of a sixteen-byte operand
        // and the add took in whatever followed them.
        const std::vector<TypeRef> operands = OperandTypes(RequireFunction(package, "Widen"), LirOpcode::Add);
        REQUIRE_EQ(operands.size(), 2);
        CHECK_EQ(operands[0].kind, TypeRef::Kind::Int128);
        CHECK_EQ(operands[1].kind, TypeRef::Kind::Int128);
    }

    TEST_CASE("a narrower expression is widened too, not just a literal") {
        const LirPackage package = CompileToLir(R"(
            func Widen(value: uint256, offset: uint32) -> uint256 {
                return value - offset;
            }
        )");

        const std::vector<TypeRef> operands = OperandTypes(RequireFunction(package, "Widen"), LirOpcode::Sub);
        REQUIRE_EQ(operands.size(), 2);
        CHECK_EQ(operands[0].kind, TypeRef::Kind::UInt256);
        CHECK_EQ(operands[1].kind, TypeRef::Kind::UInt256);
    }

    TEST_CASE("a compound assignment computes at the place's width") {
        const LirPackage package = CompileToLir(R"(
            func Accumulate(value: int512) -> int512 {
                var total = value;
                total += 1;
                return total;
            }
        )");

        const std::vector<TypeRef> operands = OperandTypes(RequireFunction(package, "Accumulate"), LirOpcode::Add);
        REQUIRE_EQ(operands.size(), 2);
        CHECK_EQ(operands[0].kind, TypeRef::Kind::Int512);
        CHECK_EQ(operands[1].kind, TypeRef::Kind::Int512);
    }

    TEST_CASE("a shift count keeps its own width") {
        const LirPackage package = CompileToLir(R"(
            func Shift(value: int128, amount: uint64) -> int128 {
                return value << amount;
            }
        )");

        // A count is not a second value of the shifted type: widening it would move a whole limb of work onto
        // something the back ends already read at its own width.
        const std::vector<TypeRef> operands = OperandTypes(RequireFunction(package, "Shift"), LirOpcode::Shl);
        REQUIRE_EQ(operands.size(), 2);
        CHECK_EQ(operands[0].kind, TypeRef::Kind::Int128);
        CHECK_EQ(operands[1].kind, TypeRef::Kind::UInt64);
    }

    TEST_CASE("pointer arithmetic still offsets by elements") {
        const LirPackage package = CompileToLir(R"(
            func Advance(cursor: *var int32) -> *var int32 {
                return cursor + 1;
            }
        )");

        // The integer operand of a pointer offset is an element count that the back end scales, so it is not widened
        // to the pointer's type.
        const LirFunc &function = RequireFunction(package, "Advance");
        const std::vector<TypeRef> operands = OperandTypes(function, LirOpcode::IndexPtr);
        REQUIRE_EQ(operands.size(), 2);
        CHECK_EQ(operands[0].kind, TypeRef::Kind::Pointer);
    }
}

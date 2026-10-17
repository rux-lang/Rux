// AST-to-HIR conversions between bool widths in patterns, arms and call arguments.
// Analysis accepts a value of one scalar type where another is expected whenever the first is assignable to the
// second: every bool width to every other, an integer to a wider one of the same signedness, float32 to float64. A
// store converts at the place it writes to, but a pattern, an arm and an argument have no place of their own, so the
// conversion has to be in the tree before HIR-to-LIR reads it at the width analysis settled on.

#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
/// Analyze a snippet and hand the accepted model to `lower`, while the module it refers to is still alive.
template <typename Lower>
auto Compile(const std::string &source, Lower lower) {
    Lexer lexer(source, "conversion.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "conversion.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    const SemanticModel model = analyzer.Analyze();
    for (const SemanticDiagnostic &diagnostic : model.diagnostics) {
        MESSAGE(diagnostic.message);
    }
    REQUIRE_FALSE(model.HasErrors());
    return lower(model);
}

HirPackage LowerToHir(const std::string &source) {
    return Compile(source, [](const SemanticModel &model) { return AstToHirLowering(model).Generate(); });
}

LirPackage LowerToLir(const std::string &source) {
    return Compile(source, [](const SemanticModel &model) {
        HirToLirLowering lowering(AstToHirLowering(model).Generate(), CompileTimeContext{}.target);
        LirPackage package = lowering.Generate();
        REQUIRE(lowering.Diagnostics().empty());
        return package;
    });
}

const HirFunc &RequireFunction(const HirPackage &package, const std::string &name) {
    for (const HirModule &module : package.modules) {
        for (const HirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
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

/// The value of the function's last statement, which every case below writes as `return <expression>;`.
const HirExpr &RequireReturned(const HirFunc &function) {
    REQUIRE(function.body.has_value());
    REQUIRE_FALSE(function.body->stmts.empty());
    const auto *statement = dynamic_cast<const HirReturnStmt *>(function.body->stmts.back().get());
    REQUIRE(statement != nullptr);
    REQUIRE(statement->value.has_value());
    return **statement->value;
}

/// The arguments of the call a function returns the result of.
const std::vector<HirExprPtr> &RequireReturnedCallArguments(const HirFunc &function) {
    const auto *call = dynamic_cast<const HirCallExpr *>(&RequireReturned(function));
    REQUIRE(call != nullptr);
    return call->args;
}

/// A conversion of a value of `from` to `to`, which is the shape every converted operand below must have.
void CheckConversion(const HirExpr &expression, const TypeRef &from, const TypeRef &to) {
    const auto *cast = dynamic_cast<const HirCastExpr *>(&expression);
    REQUIRE_MESSAGE(cast != nullptr, "expected a conversion to " << to.ToString() << ", found a value of "
                                                                 << expression.type.ToString());
    CHECK_EQ(cast->type, to);
    CHECK_EQ(cast->targetType, to);
    REQUIRE(cast->operand != nullptr);
    CHECK_EQ(cast->operand->type, from);
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

TypeRef Primitive(const TypeRef::Kind kind) {
    return TypeRef::MakePrimitive(kind);
}
} // namespace

TEST_SUITE("WideBoolLowering") {
    TEST_CASE("a literal pattern is compared at the width of the bool it matches") {
        const HirPackage package = LowerToHir(R"(
            func Score(flag: bool16) -> int32 {
                match flag {
                    true => return 1i32,
                    else => return 0i32
                }
            }
        )");
        const HirFunc &function = RequireFunction(package, "Score");
        REQUIRE(function.body.has_value());
        REQUIRE_FALSE(function.body->stmts.empty());
        const auto *match = dynamic_cast<const HirMatchStmt *>(function.body->stmts.front().get());
        REQUIRE(match != nullptr);
        REQUIRE_FALSE(match->arms.empty());
        const auto *literal = dynamic_cast<const HirLiteralPattern *>(match->arms.front().pattern.get());
        REQUIRE(literal != nullptr);
        CHECK_EQ(literal->value, "true");
        CHECK_EQ(literal->type, Primitive(TypeRef::Kind::Bool16));
    }

    TEST_CASE("the pattern's comparison reads both sides at the subject's width") {
        const LirPackage package = LowerToLir(R"(
            func Score(flag: bool32) -> int32 {
                return match flag {
                    false => 0i32,
                    else => 1i32
                };
            }
        )");

        // Left at `bool`, the constant was one byte and the comparison read four from it.
        const std::vector<TypeRef> operands = OperandTypes(RequireFunction(package, "Score"), LirOpcode::CmpEq);
        REQUIRE_EQ(operands.size(), 2);
        CHECK_EQ(operands[0], Primitive(TypeRef::Kind::Bool32));
        CHECK_EQ(operands[1], Primitive(TypeRef::Kind::Bool32));
    }

    TEST_CASE("a conditional's bool arms are converted to the width it produces") {
        const HirPackage package = LowerToHir(R"(
            func Pick(condition: bool, narrow: bool8, middle: bool32) -> bool64 {
                return condition ? narrow : middle;
            }
        )");
        const auto *ternary = dynamic_cast<const HirTernaryExpr *>(&RequireReturned(RequireFunction(package, "Pick")));
        REQUIRE(ternary != nullptr);
        CHECK_EQ(ternary->type, Primitive(TypeRef::Kind::Bool64));
        CheckConversion(*ternary->thenExpr, Primitive(TypeRef::Kind::Bool8), Primitive(TypeRef::Kind::Bool64));
        CheckConversion(*ternary->elseExpr, Primitive(TypeRef::Kind::Bool32), Primitive(TypeRef::Kind::Bool64));
    }

    TEST_CASE("a conditional's bool arms merge at the width it produces") {
        const LirPackage package = LowerToLir(R"(
            func Pick(condition: bool, narrow: bool8, middle: bool16) -> bool64 {
                return condition ? narrow : middle;
            }
        )");
        const LirFunc &function = RequireFunction(package, "Pick");
        std::unordered_map<LirReg, TypeRef> produced;
        for (const LirParam &parameter : function.params) {
            produced.emplace(parameter.reg, parameter.type);
        }
        for (const LirBlock &block : function.blocks) {
            for (const LirInstr &instruction : block.instrs) {
                if (instruction.dst != LirNoReg) {
                    produced.insert_or_assign(instruction.dst, instruction.type);
                }
            }
        }
        std::size_t merges = 0;
        for (const LirBlock &block : function.blocks) {
            for (const LirInstr &instruction : block.instrs) {
                if (instruction.op != LirOpcode::Phi) {
                    continue;
                }
                ++merges;
                CHECK_EQ(instruction.type, Primitive(TypeRef::Kind::Bool64));
                for (const auto &[reg, predecessor] : instruction.phiPreds) {
                    CAPTURE(predecessor);
                    REQUIRE(produced.contains(reg));
                    CHECK_EQ(produced.at(reg), Primitive(TypeRef::Kind::Bool64));
                }
            }
        }
        CHECK_EQ(merges, 1);
    }

    TEST_CASE("a short-circuit merges a wide right operand as the bool it produces") {
        // `&&` and `||` produce a bool8 whatever their operands, so the right side is narrowed before the phi rather
        // than merged at its own width beside the bool8 constant of the skipping edge.
        const LirPackage package = LowerToLir(R"(
            func Both(left: bool256, right: bool256) -> bool8 {
                return left && right;
            }
            func Either(left: bool512, right: bool128) -> bool8 {
                return left || right;
            }
        )");
        for (const std::string name : {"Both", "Either"}) {
            CAPTURE(name);
            const LirFunc &function = RequireFunction(package, name);
            std::unordered_map<LirReg, TypeRef> produced;
            for (const LirBlock &block : function.blocks) {
                for (const LirInstr &instruction : block.instrs) {
                    if (instruction.dst != LirNoReg) {
                        produced.insert_or_assign(instruction.dst, instruction.type);
                    }
                }
            }
            std::size_t merges = 0;
            for (const LirBlock &block : function.blocks) {
                for (const LirInstr &instruction : block.instrs) {
                    if (instruction.op != LirOpcode::Phi) {
                        continue;
                    }
                    ++merges;
                    CHECK_EQ(instruction.type, Primitive(TypeRef::Kind::Bool8));
                    for (const auto &[reg, predecessor] : instruction.phiPreds) {
                        CAPTURE(predecessor);
                        REQUIRE(produced.contains(reg));
                        CHECK_EQ(produced.at(reg), Primitive(TypeRef::Kind::Bool8));
                    }
                }
            }
            CHECK_EQ(merges, 1);
        }
    }

    TEST_CASE("a match's bool arms are converted to the width it produces") {
        const HirPackage package = LowerToHir(R"(
            func Pick(choice: int32, narrow: bool8, wide: bool64, middle: bool16) -> bool32 {
                return match choice {
                    0 => narrow,
                    1 => wide,
                    2 => middle,
                    else => true
                };
            }
        )");
        const auto *match = dynamic_cast<const HirMatchExpr *>(&RequireReturned(RequireFunction(package, "Pick")));
        REQUIRE(match != nullptr);
        REQUIRE_EQ(match->arms.size(), 4);
        const TypeRef produced = Primitive(TypeRef::Kind::Bool32);
        CHECK_EQ(match->type, produced);
        CheckConversion(*match->arms[0].body, Primitive(TypeRef::Kind::Bool8), produced);
        // A wider arm narrows: every bool width holds the same two values, so analysis accepts it in both directions.
        CheckConversion(*match->arms[1].body, Primitive(TypeRef::Kind::Bool64), produced);
        CheckConversion(*match->arms[2].body, Primitive(TypeRef::Kind::Bool16), produced);
        // A literal is already built at the match's width and needs nothing more.
        CHECK(dynamic_cast<const HirLiteralExpr *>(match->arms[3].body.get()) != nullptr);
        CHECK_EQ(match->arms[3].body->type, produced);
    }

    TEST_CASE("a bool argument is converted to its parameter's width") {
        const HirPackage package = LowerToHir(R"(
            func Take(wide: bool64, narrow: bool16) -> bool64 {
                return wide;
            }
            func Pass(narrow: bool8, wide: bool32) -> bool64 {
                return Take(narrow, wide);
            }
        )");
        const auto &arguments = RequireReturnedCallArguments(RequireFunction(package, "Pass"));
        REQUIRE_EQ(arguments.size(), 2);
        CheckConversion(*arguments[0], Primitive(TypeRef::Kind::Bool8), Primitive(TypeRef::Kind::Bool64));
        CheckConversion(*arguments[1], Primitive(TypeRef::Kind::Bool32), Primitive(TypeRef::Kind::Bool16));
    }

    TEST_CASE("an integer argument is widened to its parameter's width") {
        const HirPackage package = LowerToHir(R"(
            func Take(value: int128, count: uint512) -> int128 {
                return value;
            }
            func Pass(value: int64, count: uint32) -> int128 {
                return Take(value, count);
            }
        )");
        const auto &arguments = RequireReturnedCallArguments(RequireFunction(package, "Pass"));
        REQUIRE_EQ(arguments.size(), 2);
        CheckConversion(*arguments[0], TypeRef::MakeInt64(), Primitive(TypeRef::Kind::Int128));
        CheckConversion(*arguments[1], TypeRef::MakeUInt32(), Primitive(TypeRef::Kind::UInt512));
    }

    TEST_CASE("a float32 argument is widened to a float64 parameter") {
        const HirPackage package = LowerToHir(R"(
            func Take(value: float64) -> float64 {
                return value;
            }
            func Pass(value: float32) -> float64 {
                return Take(value);
            }
        )");
        const auto &arguments = RequireReturnedCallArguments(RequireFunction(package, "Pass"));
        REQUIRE_EQ(arguments.size(), 1);
        CheckConversion(*arguments[0], TypeRef::MakeFloat32(), TypeRef::MakeFloat64());
    }

    TEST_CASE("an argument of the parameter's own type is passed as it is") {
        const HirPackage package = LowerToHir(R"(
            func Take(value: int128, flag: bool16) -> int128 {
                return value;
            }
            func Pass(value: int128, flag: bool16) -> int128 {
                return Take(value, flag);
            }
            func Literal() -> int128 {
                return Take(18446744073709551616, true);
            }
        )");
        for (const char *caller : {"Pass", "Literal"}) {
            CAPTURE(caller);
            const auto &arguments = RequireReturnedCallArguments(RequireFunction(package, caller));
            REQUIRE_EQ(arguments.size(), 2);
            // A literal is built at the parameter's width and a value of that type already is one.
            CHECK(dynamic_cast<const HirCastExpr *>(arguments[0].get()) == nullptr);
            CHECK(dynamic_cast<const HirCastExpr *>(arguments[1].get()) == nullptr);
            CHECK_EQ(arguments[0]->type, Primitive(TypeRef::Kind::Int128));
            CHECK_EQ(arguments[1]->type, Primitive(TypeRef::Kind::Bool16));
        }
    }

    TEST_CASE("a method argument is converted like a function argument") {
        const HirPackage package = LowerToHir(R"(
            struct Gate { open: bool64; }
            extend Gate {
                func Set(self: &var Gate, open: bool64, level: int128) -> int128 {
                    self.open = open;
                    return level;
                }
            }
            func Pass(gate: &var Gate, open: bool8, level: int32) -> int128 {
                return gate.Set(open, level);
            }
        )");
        const auto &arguments = RequireReturnedCallArguments(RequireFunction(package, "Pass"));
        // The receiver comes first and is not a converted scalar.
        REQUIRE_EQ(arguments.size(), 3);
        CheckConversion(*arguments[1], Primitive(TypeRef::Kind::Bool8), Primitive(TypeRef::Kind::Bool64));
        CheckConversion(*arguments[2], TypeRef::MakeInt32(), Primitive(TypeRef::Kind::Int128));
    }

    TEST_CASE("an argument through a function value is converted to the value's parameter type") {
        const HirPackage package = LowerToHir(R"(
            func Apply(callback: func(int128, bool32) -> int128, value: int64, flag: bool8) -> int128 {
                return callback(value, flag);
            }
        )");
        const auto &arguments = RequireReturnedCallArguments(RequireFunction(package, "Apply"));
        REQUIRE_EQ(arguments.size(), 2);
        CheckConversion(*arguments[0], TypeRef::MakeInt64(), Primitive(TypeRef::Kind::Int128));
        CheckConversion(*arguments[1], Primitive(TypeRef::Kind::Bool8), Primitive(TypeRef::Kind::Bool32));
    }

    TEST_CASE("a variant payload is converted to the case's payload type") {
        const HirPackage package = LowerToHir(R"(
            variant Reading {
                Empty,
                Level(int128, bool64)
            }
            func Make(level: int64, valid: bool8) -> Reading {
                return Reading::Level(level, valid);
            }
        )");

        // The payload is stored in a slot sized by its own type, which the case then reads at the payload's.
        const auto *construct =
            dynamic_cast<const HirEnumConstructExpr *>(&RequireReturned(RequireFunction(package, "Make")));
        REQUIRE(construct != nullptr);
        REQUIRE_EQ(construct->payloads.size(), 2);
        CheckConversion(*construct->payloads[0], TypeRef::MakeInt64(), Primitive(TypeRef::Kind::Int128));
        CheckConversion(*construct->payloads[1], Primitive(TypeRef::Kind::Bool8), Primitive(TypeRef::Kind::Bool64));
    }

    TEST_CASE("a default argument is built at its parameter's width") {
        const HirPackage package = LowerToHir(R"(
            const Limit: int64 = 7i64;
            func Take(value: int128 = Limit, flag: bool32 = true) -> int128 {
                return value;
            }
            func Pass() -> int128 {
                return Take();
            }
        )");
        const auto &arguments = RequireReturnedCallArguments(RequireFunction(package, "Pass"));
        REQUIRE_EQ(arguments.size(), 2);
        CheckConversion(*arguments[0], TypeRef::MakeInt64(), Primitive(TypeRef::Kind::Int128));
        CHECK(dynamic_cast<const HirLiteralExpr *>(arguments[1].get()) != nullptr);
        CHECK_EQ(arguments[1]->type, Primitive(TypeRef::Kind::Bool32));
    }

    TEST_CASE("a scalar read through a reference is converted after it is read") {
        const HirPackage package = LowerToHir(R"(
            func Take(value: int128) -> int128 {
                return value;
            }
            func Pass(value: &int64) -> int128 {
                return Take(value);
            }
        )");
        const auto &arguments = RequireReturnedCallArguments(RequireFunction(package, "Pass"));
        REQUIRE_EQ(arguments.size(), 1);
        CheckConversion(*arguments[0], TypeRef::MakeInt64(), Primitive(TypeRef::Kind::Int128));
        const auto &read = *static_cast<const HirCastExpr &>(*arguments[0]).operand;
        CHECK(dynamic_cast<const HirUnaryExpr *>(&read) != nullptr);
    }

    TEST_CASE("a call hands over every argument at its parameter's width") {
        const LirPackage package = LowerToLir(R"(
            func Take(value: int128, flag: bool64, ratio: float64) -> int128 {
                return value;
            }
            func Pass(value: int64, flag: bool8, ratio: float32) -> int128 {
                return Take(value, flag, ratio);
            }
        )");

        // Without a conversion the call took eight bytes of a sixteen-byte argument, one of eight, and the bits of
        // a float32 as if they were a float64.
        const std::vector<TypeRef> operands = OperandTypes(RequireFunction(package, "Pass"), LirOpcode::Call);
        REQUIRE_EQ(operands.size(), 3);
        CHECK_EQ(operands[0], Primitive(TypeRef::Kind::Int128));
        CHECK_EQ(operands[1], Primitive(TypeRef::Kind::Bool64));
        CHECK_EQ(operands[2], TypeRef::MakeFloat64());
    }
}

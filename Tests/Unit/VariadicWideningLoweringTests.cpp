// Conversions of the values passed to a variadic parameter.
// A Rux variadic parameter collects its arguments into a slice of its element type, and analysis accepts each one
// the way it accepts an ordinary argument: an integer narrower than the element of its signedness, any bool width,
// float32 for float64, a narrower scalar-valued character. Each element therefore reaches the slice converted, the way
// a fixed argument reaches its parameter. A C-variadic tail has no parameter at all, and C passes it after the default
// argument promotions: what is narrower than `int` arrives as `int`, a float as a double. The callee reads it at that
// width, on every target, so the caller has to make the conversion.

#include "Driver/BuildTarget.h"
#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Target/TargetTriple.h"

#include <cstdint>
#include <doctest.h>
#include <optional>
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
    Lexer lexer(source, "variadic.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "variadic.rux");
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

LirPackage LowerToLir(const std::string &source, const std::string &triple) {
    return Compile(source, [&triple](const SemanticModel &model) {
        HirToLirLowering lowering(AstToHirLowering(model).Generate(),
                                  Driver::TargetContextForTriple(*Target::TargetTriple::Parse(triple)));
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

/// The elements of the slice built for the variadic parameter of the call the function returns the result of.
const std::vector<HirExprPtr> &RequireVariadicElements(const HirFunc &function) {
    REQUIRE(function.body.has_value());
    REQUIRE_FALSE(function.body->stmts.empty());
    const auto *statement = dynamic_cast<const HirReturnStmt *>(function.body->stmts.back().get());
    REQUIRE(statement != nullptr);
    REQUIRE(statement->value.has_value());
    const auto *call = dynamic_cast<const HirCallExpr *>(statement->value->get());
    REQUIRE(call != nullptr);
    REQUIRE_FALSE(call->args.empty());
    const auto *slice = dynamic_cast<const HirArrayExpr *>(call->args.back().get());
    REQUIRE(slice != nullptr);
    return slice->elements;
}

/// A conversion of a value of `from` to `to`, which is the shape every converted element below must have.
void CheckConversion(const HirExpr &expression, const TypeRef &from, const TypeRef &to) {
    const auto *cast = dynamic_cast<const HirCastExpr *>(&expression);
    REQUIRE_MESSAGE(cast != nullptr, "expected a conversion to " << to.ToString() << ", found a value of "
                                                                 << expression.type.ToString());
    CHECK_EQ(cast->type, to);
    CHECK_EQ(cast->targetType, to);
    REQUIRE(cast->operand != nullptr);
    CHECK_EQ(cast->operand->type, from);
}

/// The type every source of the first call to `callee` is held at, in order.
std::vector<TypeRef> CallOperandTypes(const LirFunc &function, const std::string &callee) {
    std::unordered_map<LirReg, TypeRef> produced;
    for (const LirParam &param : function.params) {
        produced.emplace(param.reg, param.type);
    }
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Call && instruction.strArg == callee) {
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
    FAIL("missing call to " << callee);
    throw std::runtime_error("missing call");
}

TypeRef Primitive(const TypeRef::Kind kind) {
    return TypeRef::MakePrimitive(kind);
}

/// A C-variadic call with one value of every type the default argument promotions touch, followed by values they
/// leave alone.
const char *const CVariadicSource = R"(
    #Link("c")
    extern func Collect(format: *char8, ...) -> int32;

    func Main(format: *char8, small: int8, short: int16, byte: uint8, word: uint16, flag: bool8, wide: bool16,
              unit: char8, pair: char16, single: float32, scalar: char32, count: int64, mask: bool32,
              exact: float64) -> int32 {
        return Collect(format, small, short, byte, word, flag, wide, unit, pair, single, scalar, count, mask, exact);
    }
)";

/// The types C reads the arguments of `CVariadicSource` at, with a double passed as `doubleType`.
std::vector<TypeRef> PromotedOperands(const TypeRef &doubleType) {
    std::vector<TypeRef> types = {TypeRef::MakePointer(Primitive(TypeRef::Kind::Char8))};
    for (int index = 0; index < 8; ++index) {
        types.push_back(Primitive(TypeRef::Kind::Int32));
    }
    types.push_back(doubleType);
    types.push_back(Primitive(TypeRef::Kind::Char32));
    types.push_back(Primitive(TypeRef::Kind::Int64));
    types.push_back(Primitive(TypeRef::Kind::Bool32));
    types.push_back(doubleType);
    return types;
}

void CheckOperands(const std::vector<TypeRef> &actual, const std::vector<TypeRef> &expected) {
    REQUIRE_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        CHECK_MESSAGE(actual[index] == expected[index], "argument " << index << " is passed as "
                                                                    << actual[index].ToString() << ", not as "
                                                                    << expected[index].ToString());
    }
}
} // namespace

TEST_SUITE("VariadicWideningLowering") {
    TEST_CASE("integers reach a wider variadic element converted") {
        const HirPackage package = LowerToHir(R"(
            func Signed(values: int128...) -> int128 {
                return 0i128;
            }

            func Unsigned(values: uint256...) -> uint256 {
                return 0u256;
            }

            func CallSigned(word: int64, half: int32, byte: int8, same: int128) -> int128 {
                return Signed(word, half, byte, same);
            }

            func CallUnsigned(byte: uint8, word: uint64) -> uint256 {
                return Unsigned(byte, word);
            }
        )");
        const auto &signedElements = RequireVariadicElements(RequireFunction(package, "CallSigned"));
        REQUIRE_EQ(signedElements.size(), 4);
        CheckConversion(*signedElements[0], Primitive(TypeRef::Kind::Int64), Primitive(TypeRef::Kind::Int128));
        CheckConversion(*signedElements[1], Primitive(TypeRef::Kind::Int32), Primitive(TypeRef::Kind::Int128));
        CheckConversion(*signedElements[2], Primitive(TypeRef::Kind::Int8), Primitive(TypeRef::Kind::Int128));
        // An element already of the element type is passed as it stands.
        CHECK(dynamic_cast<const HirCastExpr *>(signedElements[3].get()) == nullptr);
        CHECK_EQ(signedElements[3]->type, Primitive(TypeRef::Kind::Int128));

        const auto &unsignedElements = RequireVariadicElements(RequireFunction(package, "CallUnsigned"));
        REQUIRE_EQ(unsignedElements.size(), 2);
        CheckConversion(*unsignedElements[0], Primitive(TypeRef::Kind::UInt8), Primitive(TypeRef::Kind::UInt256));
        CheckConversion(*unsignedElements[1], Primitive(TypeRef::Kind::UInt64), Primitive(TypeRef::Kind::UInt256));
    }

    TEST_CASE("bools, floats and characters reach a wider variadic element converted") {
        const HirPackage package = LowerToHir(R"(
            func Flags(values: bool128...) -> int32 {
                return 0i32;
            }

            func Doubles(values: float64...) -> float64 {
                return 0.0;
            }

            func Characters(values: char128...) -> int32 {
                return 0i32;
            }

            func CallFlags(narrow: bool8, wide: bool64, wider: bool256) -> int32 {
                return Flags(narrow, wide, wider);
            }

            func CallDoubles(single: float32) -> float64 {
                return Doubles(single);
            }

            func CallCharacters(scalar: char32, wide: char64) -> int32 {
                return Characters(scalar, wide);
            }
        )");
        const auto &flags = RequireVariadicElements(RequireFunction(package, "CallFlags"));
        REQUIRE_EQ(flags.size(), 3);
        CheckConversion(*flags[0], Primitive(TypeRef::Kind::Bool8), Primitive(TypeRef::Kind::Bool128));
        CheckConversion(*flags[1], Primitive(TypeRef::Kind::Bool64), Primitive(TypeRef::Kind::Bool128));
        // A wider bool narrows to the element too: only whether it is set crosses.
        CheckConversion(*flags[2], Primitive(TypeRef::Kind::Bool256), Primitive(TypeRef::Kind::Bool128));

        const auto &doubles = RequireVariadicElements(RequireFunction(package, "CallDoubles"));
        REQUIRE_EQ(doubles.size(), 1);
        CheckConversion(*doubles[0], Primitive(TypeRef::Kind::Float32), Primitive(TypeRef::Kind::Float64));

        const auto &characters = RequireVariadicElements(RequireFunction(package, "CallCharacters"));
        REQUIRE_EQ(characters.size(), 2);
        CheckConversion(*characters[0], Primitive(TypeRef::Kind::Char32), Primitive(TypeRef::Kind::Char128));
        CheckConversion(*characters[1], Primitive(TypeRef::Kind::Char64), Primitive(TypeRef::Kind::Char128));
    }

    TEST_CASE("an interface method's variadic elements are converted like a function's") {
        const HirPackage package = LowerToHir(R"(
            interface Adder {
                func Add(seed: int32, values: int128...) -> int128;
            }

            struct Total {
                base: int128;
            }

            extend Total : Adder {
                func Add(self: &Total, seed: int32, values: int128...) -> int128 {
                    return self.base;
                }
            }

            func CallAdd(adder: Adder, word: int64, flag: int8) -> int128 {
                return adder.Add(0i32, word, flag);
            }
        )");
        const HirFunc &function = RequireFunction(package, "CallAdd");
        REQUIRE(function.body.has_value());
        REQUIRE_FALSE(function.body->stmts.empty());
        const auto *statement = dynamic_cast<const HirReturnStmt *>(function.body->stmts.back().get());
        REQUIRE(statement != nullptr);
        REQUIRE(statement->value.has_value());
        const auto *call = dynamic_cast<const HirInterfaceCallExpr *>(statement->value->get());
        REQUIRE(call != nullptr);
        REQUIRE_EQ(call->args.size(), 2);
        const auto *slice = dynamic_cast<const HirArrayExpr *>(call->args.back().get());
        REQUIRE(slice != nullptr);
        REQUIRE_EQ(slice->elements.size(), 2);
        CheckConversion(*slice->elements[0], Primitive(TypeRef::Kind::Int64), Primitive(TypeRef::Kind::Int128));
        CheckConversion(*slice->elements[1], Primitive(TypeRef::Kind::Int8), Primitive(TypeRef::Kind::Int128));
    }

    TEST_CASE("a method's variadic elements are collected and converted like a function's") {
        const HirPackage package = LowerToHir(R"(
            struct Total {
                base: int128;
            }

            extend Total {
                func Sum(self: &Total, values: int128...) -> int128 {
                    return self.base;
                }

                func Add(self: &var Total, label: int32, values: int64...) -> int64 {
                    return 0i64;
                }

                func Pick<T>(self: &Total, index: uint, values: T...) -> T {
                    return values[index];
                }

                func Of(values: int128...) -> Total {
                    return Total { base: 0i128 };
                }
            }

            struct Bag<T> {
                first: T;
            }

            extend Bag<T> {
                func Count(self: &Bag<T>, values: T...) -> uint {
                    return values.length;
                }
            }

            extend int64 {
                func Plus(self: int64, values: int64...) -> int64 {
                    return self;
                }
            }

            func CallSum(total: Total, word: int64, byte: int8) -> int128 {
                return total.Sum(word, byte);
            }

            func CallEmpty(total: Total) -> int128 {
                return total.Sum();
            }

            func CallAdd(word: int32, half: int32) -> int64 {
                var total = Total { base: 0i128 };
                return total.Add(1i32, word, half);
            }

            func CallPick(total: Total) -> int32 {
                return total.Pick(1u, 5i32, 6i32);
            }

            func CallBag(bag: Bag<int128>, word: int64) -> uint {
                return bag.Count(word);
            }

            func CallPlus(start: int64, half: int32) -> int64 {
                return start.Plus(half, 2i64);
            }

            func CallOf(word: int64) -> Total {
                return Total::Of(word);
            }
        )");
        const TypeRef int128 = Primitive(TypeRef::Kind::Int128);
        const TypeRef int64 = Primitive(TypeRef::Kind::Int64);
        const TypeRef int32 = Primitive(TypeRef::Kind::Int32);

        // The receiver comes first and is never an element; every argument after it is.
        const auto &sum = RequireVariadicElements(RequireFunction(package, "CallSum"));
        REQUIRE_EQ(sum.size(), 2);
        CheckConversion(*sum[0], int64, int128);
        CheckConversion(*sum[1], Primitive(TypeRef::Kind::Int8), int128);
        CHECK(RequireVariadicElements(RequireFunction(package, "CallEmpty")).empty());

        // A fixed parameter between the receiver and the variadic one takes the first argument.
        const auto &add = RequireVariadicElements(RequireFunction(package, "CallAdd"));
        REQUIRE_EQ(add.size(), 2);
        CheckConversion(*add[0], int32, int64);
        CheckConversion(*add[1], int32, int64);

        // A method's own type parameter is deduced from its elements, and a generic type's from the receiver.
        const auto &pick = RequireVariadicElements(RequireFunction(package, "CallPick"));
        REQUIRE_EQ(pick.size(), 2);
        CHECK_EQ(pick[0]->type, int32);
        const auto &bag = RequireVariadicElements(RequireFunction(package, "CallBag"));
        REQUIRE_EQ(bag.size(), 1);
        CheckConversion(*bag[0], int64, int128);

        // A primitive's method takes its receiver by value, and an associated function has none.
        const auto &plus = RequireVariadicElements(RequireFunction(package, "CallPlus"));
        REQUIRE_EQ(plus.size(), 2);
        CheckConversion(*plus[0], int32, int64);
        CHECK_EQ(plus[1]->type, int64);
        const auto &of = RequireVariadicElements(RequireFunction(package, "CallOf"));
        REQUIRE_EQ(of.size(), 1);
        CheckConversion(*of[0], int64, int128);
    }

    TEST_CASE("a requirement that spells out its receiver collects the arguments after it") {
        const HirPackage package = LowerToHir(R"(
            interface Adder {
                func Add(self: &Self, seed: int32, values: int128...) -> int128;
            }

            struct Total {
                base: int128;
            }

            extend Total : Adder {
                func Add(self: &Total, seed: int32, values: int128...) -> int128 {
                    return self.base;
                }
            }

            func CallAdd(adder: Adder, word: int64) -> int128 {
                return adder.Add(0i32, word, 3i128);
            }
        )");
        const HirFunc &function = RequireFunction(package, "CallAdd");
        REQUIRE(function.body.has_value());
        const auto *statement = dynamic_cast<const HirReturnStmt *>(function.body->stmts.back().get());
        REQUIRE(statement != nullptr);
        REQUIRE(statement->value.has_value());
        const auto *call = dynamic_cast<const HirInterfaceCallExpr *>(statement->value->get());
        REQUIRE(call != nullptr);
        REQUIRE_EQ(call->args.size(), 2);
        CHECK_EQ(call->args[0]->type, Primitive(TypeRef::Kind::Int32));
        const auto *slice = dynamic_cast<const HirArrayExpr *>(call->args.back().get());
        REQUIRE(slice != nullptr);
        REQUIRE_EQ(slice->elements.size(), 2);
        CheckConversion(*slice->elements[0], Primitive(TypeRef::Kind::Int64), Primitive(TypeRef::Kind::Int128));
    }

    TEST_CASE("a C-variadic tail is passed after the default argument promotions on System V") {
        const LirPackage package = LowerToLir(CVariadicSource, "linux-x86_64");
        CheckOperands(CallOperandTypes(RequireFunction(package, "Main"), "Collect"),
                      PromotedOperands(Primitive(TypeRef::Kind::Float64)));
    }

    TEST_CASE("a C-variadic tail is passed after the default argument promotions on AArch64") {
        for (const char *triple : {"linux-aarch64", "macos-aarch64", "windows-aarch64"}) {
            CAPTURE(triple);
            const LirPackage package = LowerToLir(CVariadicSource, triple);
            CheckOperands(CallOperandTypes(RequireFunction(package, "Main"), "Collect"),
                          PromotedOperands(Primitive(TypeRef::Kind::Float64)));
        }
    }

    TEST_CASE("a Win64 C-variadic double travels as its bits in the integer register file") {
        // A Win64 variadic callee spills rcx, rdx, r8 and r9 to its home area and reads every anonymous argument
        // from there, so a double placed only in an xmm register was never seen. Its bits go where it is read.
        const LirPackage package = LowerToLir(CVariadicSource, "windows-x86_64");
        CheckOperands(CallOperandTypes(RequireFunction(package, "Main"), "Collect"),
                      PromotedOperands(Primitive(TypeRef::Kind::UInt64)));
    }

    TEST_CASE("a fixed float parameter of a C-variadic function is not moved to the integer file") {
        const LirPackage package = LowerToLir(R"(
            #Link("c")
            extern func Scale(factor: float64, ...) -> int32;

            func Main(factor: float64, extra: float32) -> int32 {
                return Scale(factor, extra);
            }
        )",
                                              "windows-x86_64");
        CheckOperands(CallOperandTypes(RequireFunction(package, "Main"), "Scale"),
                      {Primitive(TypeRef::Kind::Float64), Primitive(TypeRef::Kind::UInt64)});
    }

    TEST_CASE("a C-variadic tail called through a function value is promoted too") {
        const LirPackage package = LowerToLir(R"(
            func Forward(callback: func(*char8, ...) -> int32, format: *char8, small: int8, single: float32) -> int32 {
                return callback(format, small, single);
            }
        )",
                                              "linux-x86_64");
        const LirFunc &function = RequireFunction(package, "Forward");
        std::unordered_map<LirReg, TypeRef> produced;
        for (const LirParam &param : function.params) {
            produced.emplace(param.reg, param.type);
        }
        std::vector<TypeRef> operands;
        for (const LirBlock &block : function.blocks) {
            for (const LirInstr &instruction : block.instrs) {
                if (instruction.op == LirOpcode::CallIndirect) {
                    // The boundary is recorded as for a named extern, over the arguments and not the callee.
                    CHECK(instruction.isCVariadic);
                    CHECK_EQ(instruction.cVariadicFixedParamCount, std::optional<std::uint32_t>(1));
                    for (const LirReg source : instruction.srcs) {
                        const auto found = produced.find(source);
                        operands.push_back(found == produced.end() ? TypeRef::MakeOpaque() : found->second);
                    }
                }
                if (instruction.dst != LirNoReg) {
                    produced.insert_or_assign(instruction.dst, instruction.type);
                }
            }
        }
        // The callee itself comes first.
        REQUIRE_EQ(operands.size(), 4);
        CHECK_EQ(operands[1], TypeRef::MakePointer(Primitive(TypeRef::Kind::Char8)));
        CHECK_EQ(operands[2], Primitive(TypeRef::Kind::Int32));
        CHECK_EQ(operands[3], Primitive(TypeRef::Kind::Float64));
    }
}

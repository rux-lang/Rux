// Rewriting software-float operations into helper calls during HIR-to-LIR lowering.

#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Lowering/HirToLir/HirToLirSoftFloat.h"

#include <algorithm>
#include <doctest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::HirToLirDetail;

namespace {
using K = TypeRef::Kind;

TypeRef Primitive(const K kind) {
    return TypeRef::MakePrimitive(kind);
}

HirExprPtr Var(const std::string &name, const K kind) {
    auto var = std::make_unique<HirVarExpr>();
    var->name = name;
    var->type = Primitive(kind);
    return var;
}

HirExprPtr Literal(const std::string &value, const K kind) {
    auto literal = std::make_unique<HirLiteralExpr>();
    literal->value = value;
    literal->type = Primitive(kind);
    return literal;
}

HirExprPtr Binary(const TokenKind op, HirExprPtr left, HirExprPtr right, const K kind) {
    auto binary = std::make_unique<HirBinaryExpr>();
    binary->op = op;
    binary->left = std::move(left);
    binary->right = std::move(right);
    binary->type = Primitive(kind);
    return binary;
}

HirExprPtr Cast(HirExprPtr operand, const K kind) {
    auto cast = std::make_unique<HirCastExpr>();
    cast->operand = std::move(operand);
    cast->targetType = Primitive(kind);
    cast->type = Primitive(kind);
    return cast;
}

/// `func <name>(<params>) -> <result> { return <value>; }`
void AddFunction(HirModule &module, const std::string &name, const std::vector<std::pair<std::string, K>> &params,
                 const K result, HirExprPtr value) {
    HirFunc &function = module.funcs.emplace_back();
    function.name = name;
    function.returnType = Primitive(result);
    for (const auto &[paramName, kind] : params) {
        function.params.push_back({paramName, Primitive(kind), false, 0});
    }
    auto returned = std::make_unique<HirReturnStmt>();
    returned->value = std::move(value);
    function.body.emplace();
    function.body->stmts.push_back(std::move(returned));
}

LirPackage Lower(HirModule module) {
    HirPackage package;
    module.name = "main";
    package.modules.push_back(std::move(module));
    HirToLirLowering lowering(std::move(package), TargetContext::CreateNative());
    LirPackage lowered = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return lowered;
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

std::vector<const LirInstr *> Instructions(const LirFunc &function) {
    std::vector<const LirInstr *> instructions;
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            instructions.push_back(&instruction);
        }
    }
    return instructions;
}

bool HasOpcode(const LirFunc &function, const LirOpcode op) {
    const auto instructions = Instructions(function);
    return std::ranges::any_of(instructions, [&](const LirInstr *instruction) { return instruction->op == op; });
}

/// The instruction that defines `reg`.
const LirInstr &Definition(const LirFunc &function, const LirReg reg) {
    for (const LirInstr *instruction : Instructions(function)) {
        if (instruction->dst == reg) {
            return *instruction;
        }
    }
    FAIL("register " << reg << " is never defined");
    throw std::runtime_error("undefined register");
}

/// The instruction that produces the value a single-block function returns.
const LirInstr &Returned(const LirFunc &function) {
    REQUIRE(function.blocks.back().term.has_value());
    REQUIRE(function.blocks.back().term->retVal.has_value());
    return Definition(function, *function.blocks.back().term->retVal);
}

void CheckHelper(const LirPackage &package, const std::string &symbol, const std::vector<K> &params, const K result) {
    const LirFunc &helper = RequireFunction(package, symbol);
    CHECK_FALSE(helper.isPublic);
    CHECK_EQ(helper.returnType, Primitive(result));
    REQUIRE_EQ(helper.params.size(), params.size());
    for (std::size_t index = 0; index < params.size(); ++index) {
        CHECK_EQ(helper.params[index].type, Primitive(params[index]));
    }
    CHECK(std::ranges::all_of(helper.blocks, [](const LirBlock &block) { return block.term.has_value(); }));
}

Target::FloatLowering X87Float80(const TypeRef::Kind kind, const Target::Arch arch) {
    return kind == K::Float80 ? Target::FloatLowering::X87 : Target::FloatLoweringFor(kind, arch);
}

LirInstr Instruction(const LirOpcode op, const LirReg dst, const K kind, std::vector<LirReg> sources,
                     std::string argument = {}) {
    LirInstr instruction;
    instruction.op = op;
    instruction.dst = dst;
    instruction.type = Primitive(kind);
    instruction.srcs = std::move(sources);
    instruction.strArg = std::move(argument);
    return instruction;
}

/// One function taking two values of `operand` and computing `instruction` from them, as LIR built by hand.
LirPackage HandBuilt(const K operand, LirInstr instruction) {
    LirFunc function;
    function.name = "Leftover";
    function.params = {{0, Primitive(operand), "a"}, {1, Primitive(operand), "b"}};
    function.returnType = instruction.type;
    LirTerminator returned;
    returned.kind = LirTermKind::Return;
    returned.retVal = instruction.dst;
    returned.retType = instruction.type;
    function.blocks.push_back({"entry", {std::move(instruction)}, std::move(returned)});
    LirPackage package;
    package.modules.push_back({});
    package.modules.back().funcs.push_back(std::move(function));
    return package;
}
} // namespace

TEST_CASE("software-float helpers are named for their format and operation") {
    CHECK_EQ(SoftFloatHelperName({SoftFloatOperation::Add, K::Float16}), "__rux_float_f16_add");
    CHECK_EQ(SoftFloatHelperName({SoftFloatOperation::Subtract, K::Float8}), "__rux_float_f8_sub");
    CHECK_EQ(SoftFloatHelperName({SoftFloatOperation::Multiply, K::Float80}), "__rux_float_f80_mul");
    CHECK_EQ(SoftFloatHelperName({SoftFloatOperation::Divide, K::Float256}), "__rux_float_f256_div");
    CHECK_EQ(SoftFloatHelperName({SoftFloatOperation::Remainder, K::Float512}), "__rux_float_f512_rem");
    CHECK_EQ(SoftFloatHelperName({SoftFloatOperation::Compare, K::Float128}), "__rux_float_f128_compare");
    CHECK_EQ(SoftFloatHelperName({SoftFloatOperation::ConvertTo, K::Float128, K::Int32}), "__rux_float_f128_to_int32");
    CHECK_EQ(SoftFloatHelperName({SoftFloatOperation::ConvertFrom, K::Float16, K::Float64}),
             "__rux_float_f16_from_float64");

    CHECK_EQ(SoftFloatHelperOperandKind(K::Int), K::Int64);
    CHECK_EQ(SoftFloatHelperOperandKind(K::UInt), K::UInt64);
    CHECK_EQ(SoftFloatHelperOperandKind(K::Char32), K::UInt32);
    CHECK_EQ(SoftFloatHelperOperandKind(K::Bool8), K::UInt8);
    CHECK_EQ(SoftFloatHelperOperandKind(K::Int128), K::Int128);

    const SoftFloatHelperSignature compare = SoftFloatHelperSignatureOf({SoftFloatOperation::Compare, K::Float80});
    CHECK_EQ(compare.parameters, std::vector{Primitive(K::Float80), Primitive(K::Float80)});
    CHECK_EQ(compare.result, TypeRef::MakeUInt8());
    const SoftFloatHelperSignature from =
        SoftFloatHelperSignatureOf({SoftFloatOperation::ConvertFrom, K::Float16, K::UInt64});
    CHECK_EQ(from.parameters, std::vector{Primitive(K::UInt64)});
    CHECK_EQ(from.result, Primitive(K::Float16));
}

TEST_CASE("float16 addition becomes a call to its synthesized helper") {
    HirModule module;
    AddFunction(module, "Sum", {{"a", K::Float16}, {"b", K::Float16}}, K::Float16,
                Binary(TokenKind::Plus, Var("a", K::Float16), Literal("1.5", K::Float16), K::Float16));
    const LirPackage package = Lower(std::move(module));

    const LirFunc &sum = RequireFunction(package, "Sum");
    CHECK_FALSE(HasOpcode(sum, LirOpcode::Add));
    const LirInstr &call = Returned(sum);
    CHECK_EQ(call.op, LirOpcode::Call);
    CHECK_EQ(call.strArg, "__rux_float_f16_add");
    CHECK_EQ(call.type, Primitive(K::Float16));
    REQUIRE_EQ(call.srcs.size(), 2);
    // The literal reaches LIR as the exact hexadecimal spelling every back end parses to the same bits.
    const LirInstr &constant = Definition(sum, call.srcs[1]);
    CHECK_EQ(constant.op, LirOpcode::Const);
    CHECK_EQ(constant.strArg, "0x1.8p0");

    CheckHelper(package, "__rux_float_f16_add", {K::Float16, K::Float16}, K::Float16);
    CHECK_FALSE(HasOpcode(RequireFunction(package, "__rux_float_f16_add"), LirOpcode::Panic));
}

TEST_CASE("float128 comparisons read the compare helper's ordering code") {
    HirModule module;
    AddFunction(module, "AtMost", {{"a", K::Float128}, {"b", K::Float128}}, K::Bool8,
                Binary(TokenKind::LessEqual, Var("a", K::Float128), Var("b", K::Float128), K::Bool8));
    AddFunction(module, "AtLeast", {{"a", K::Float128}, {"b", K::Float128}}, K::Bool8,
                Binary(TokenKind::GreaterEqual, Var("a", K::Float128), Var("b", K::Float128), K::Bool8));
    const LirPackage package = Lower(std::move(module));

    const LirFunc &atMost = RequireFunction(package, "AtMost");
    const LirInstr &test = Returned(atMost);
    CHECK_EQ(test.op, LirOpcode::CmpLe);
    const LirInstr &code = Definition(atMost, test.srcs.at(0));
    CHECK_EQ(code.op, LirOpcode::Call);
    CHECK_EQ(code.strArg, "__rux_float_f128_compare");
    CHECK_EQ(code.type, TypeRef::MakeUInt8());
    CHECK_EQ(Definition(atMost, test.srcs.at(1)).strArg, "1");

    // Equal or Greater: two tests of the code, joined.
    const LirInstr &either = Returned(RequireFunction(package, "AtLeast"));
    CHECK_EQ(either.op, LirOpcode::Or);

    CheckHelper(package, "__rux_float_f128_compare", {K::Float128, K::Float128}, K::UInt8);
}

TEST_CASE("float512 cast to bool compares against zero through the compare helper") {
    HirModule module;
    AddFunction(module, "Truth", {{"x", K::Float512}}, K::Bool8, Cast(Var("x", K::Float512), K::Bool8));
    const LirPackage package = Lower(std::move(module));

    const LirFunc &truth = RequireFunction(package, "Truth");
    const LirInstr &test = Returned(truth);
    CHECK_EQ(test.op, LirOpcode::CmpNe);
    const LirInstr &code = Definition(truth, test.srcs.at(0));
    CHECK_EQ(code.strArg, "__rux_float_f512_compare");
    const LirInstr &zero = Definition(truth, code.srcs.at(1));
    CHECK_EQ(zero.op, LirOpcode::Const);
    CHECK_EQ(zero.type, Primitive(K::Float512));
    CHECK_EQ(zero.strArg, "0x0p0");
    CHECK_EQ(Definition(truth, test.srcs.at(1)).strArg, "1");
}

TEST_CASE("conversions call a helper keyed by the fixed-width type on the other side") {
    HirModule module;
    AddFunction(module, "Narrow", {{"x", K::Float128}}, K::Int32, Cast(Var("x", K::Float128), K::Int32));
    AddFunction(module, "Widen", {{"n", K::Int}}, K::Float16, Cast(Var("n", K::Int), K::Float16));
    AddFunction(module, "Rescale", {{"h", K::Float16}}, K::Float128, Cast(Var("h", K::Float16), K::Float128));
    const LirPackage package = Lower(std::move(module));

    const LirInstr &narrow = Returned(RequireFunction(package, "Narrow"));
    CHECK_EQ(narrow.op, LirOpcode::Call);
    CHECK_EQ(narrow.strArg, "__rux_float_f128_to_int32");

    // `int` reaches the helper as the int64 it is on every target.
    const LirFunc &widenFunction = RequireFunction(package, "Widen");
    const LirInstr &widen = Returned(widenFunction);
    CHECK_EQ(widen.strArg, "__rux_float_f16_from_int64");
    const LirInstr &restated = Definition(widenFunction, widen.srcs.at(0));
    CHECK_EQ(restated.op, LirOpcode::Cast);
    CHECK_EQ(restated.type, Primitive(K::Int64));

    CHECK_EQ(Returned(RequireFunction(package, "Rescale")).strArg, "__rux_float_f16_to_float128");

    CheckHelper(package, "__rux_float_f128_to_int32", {K::Float128}, K::Int32);
    CheckHelper(package, "__rux_float_f16_from_int64", {K::Int64}, K::Float16);
    CheckHelper(package, "__rux_float_f16_to_float128", {K::Float16}, K::Float128);
}

TEST_CASE("float32 operations stay machine instructions") {
    HirModule module;
    AddFunction(module, "Sum", {{"a", K::Float32}, {"b", K::Float32}}, K::Float32,
                Binary(TokenKind::Plus, Var("a", K::Float32), Var("b", K::Float32), K::Float32));
    AddFunction(module, "Less", {{"a", K::Float32}, {"b", K::Float32}}, K::Bool8,
                Binary(TokenKind::Less, Var("a", K::Float32), Var("b", K::Float32), K::Bool8));
    const LirPackage package = Lower(std::move(module));

    CHECK_EQ(Returned(RequireFunction(package, "Sum")).op, LirOpcode::Add);
    CHECK_EQ(Returned(RequireFunction(package, "Less")).op, LirOpcode::CmpLt);
    CHECK_EQ(package.modules.front().funcs.size(), 2);
}

TEST_CASE("an x87 policy keeps the operations the x87 unit computes") {
    const SoftFloatPolicy x87{Target::Arch::X86_64, &X87Float80};
    // AArch64 has no x87 unit, so the default policy computes float80 in software there.
    const SoftFloatPolicy software{Target::Arch::AArch64};
    const LirInstr add = Instruction(LirOpcode::Add, 2, K::Float80, {0, 1});
    const LirInstr less = Instruction(LirOpcode::CmpLt, 2, K::Bool8, {0, 1});

    CHECK_FALSE(SoftFloatHelperFor(add, K::Float80, x87).has_value());
    CHECK_FALSE(SoftFloatHelperFor(less, K::Float80, x87).has_value());
    CHECK_EQ(SoftFloatHelperFor(add, K::Float80, software), SoftFloatHelperKey{SoftFloatOperation::Add, K::Float80});

    const auto cast = [&](const K from, const K to, const SoftFloatPolicy &policy) {
        return SoftFloatHelperFor(Instruction(LirOpcode::Cast, 1, to, {0}, Primitive(from).ToString()), from, policy);
    };
    // Native float and small signed integer conversions stay with the x87 unit.
    CHECK_FALSE(cast(K::Float80, K::Float64, x87).has_value());
    CHECK_FALSE(cast(K::Float32, K::Float80, x87).has_value());
    CHECK_FALSE(cast(K::Int32, K::Float80, x87).has_value());
    // Saturation, unsigned and wide integers, and the other software widths do not.
    CHECK_EQ(cast(K::Float80, K::Int32, x87), SoftFloatHelperKey{SoftFloatOperation::ConvertTo, K::Float80, K::Int32});
    CHECK_EQ(cast(K::UInt64, K::Float80, x87),
             SoftFloatHelperKey{SoftFloatOperation::ConvertFrom, K::Float80, K::UInt64});
    CHECK_EQ(cast(K::Int128, K::Float80, x87),
             SoftFloatHelperKey{SoftFloatOperation::ConvertFrom, K::Float80, K::Int128});
    CHECK_EQ(cast(K::Float80, K::Float128, x87),
             SoftFloatHelperKey{SoftFloatOperation::ConvertTo, K::Float80, K::Float128});
    CHECK_EQ(cast(K::Float128, K::Float80, x87),
             SoftFloatHelperKey{SoftFloatOperation::ConvertTo, K::Float128, K::Float80});
    // Under the software policy every float80 conversion is a helper; the default policy picks x87 on x86-64.
    CHECK_EQ(cast(K::Int32, K::Float80, software),
             SoftFloatHelperKey{SoftFloatOperation::ConvertFrom, K::Float80, K::Int32});
    CHECK_FALSE(cast(K::Int32, K::Int64, software).has_value());
    CHECK_FALSE(SoftFloatHelperFor(add, K::Float80, SoftFloatPolicy{Target::Arch::X86_64}).has_value());
}

TEST_CASE("the registry synthesizes each requested helper once") {
    SoftFloatHelperRegistry registry;
    const SoftFloatHelperKey key{SoftFloatOperation::Multiply, K::Float256};
    CHECK_EQ(registry.Request(key), "__rux_float_f256_mul");
    CHECK_EQ(registry.Request(key), "__rux_float_f256_mul");

    std::vector<Diagnostic> diagnostics;
    const std::vector<LirFunc> helpers = registry.SynthesizePending(diagnostics);
    CHECK(diagnostics.empty());
    REQUIRE_EQ(helpers.size(), 1);
    CHECK_EQ(helpers.front().name, "__rux_float_f256_mul");
    CHECK_EQ(helpers.front().params.size(), 2);

    // Already synthesized: asking again queues nothing.
    static_cast<void>(registry.Request(key));
    CHECK(registry.SynthesizePending(diagnostics).empty());
}

TEST_CASE("the verifier reports software-float operations left for code generation") {
    std::vector<Diagnostic> diagnostics;
    const SoftFloatPolicy policy{Target::Arch::AArch64};

    VerifySoftFloatLowering(HandBuilt(K::Float128, Instruction(LirOpcode::Add, 2, K::Float128, {0, 1})), policy,
                            diagnostics);
    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_NE(diagnostics.front().message.find("'add' on float128"), std::string::npos);

    diagnostics.clear();
    VerifySoftFloatLowering(HandBuilt(K::Float16, Instruction(LirOpcode::CmpLt, 2, K::Bool8, {0, 1})), policy,
                            diagnostics);
    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_NE(diagnostics.front().message.find("'cmplt' on float16"), std::string::npos);

    diagnostics.clear();
    VerifySoftFloatLowering(HandBuilt(K::Float64, Instruction(LirOpcode::Mul, 2, K::Float64, {0, 1})), policy,
                            diagnostics);
    CHECK(diagnostics.empty());
}

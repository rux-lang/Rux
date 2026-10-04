// What tuple and structure patterns lower to: each part carries the type it has inside the subject, and refutable
// parts branch out before any binding is set.

#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Target/Target.h"

#include <algorithm>
#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
/// The HIR of `source`, which must analyze without errors.
HirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "destructuring.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "destructuring.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    for (const auto &diagnostic : model.diagnostics) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            FAIL_CHECK(diagnostic.message);
        }
    }
    AstToHirLowering lowering(model);
    HirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

/// The LIR of `package`, after checking that both back ends accept it.
LirPackage LowerToLir(HirPackage package) {
    HirToLirLowering lowering(std::move(package), TargetContext::CreateNative());
    LirPackage lir = lowering.Generate();
    CHECK(lowering.Diagnostics().empty());
    const RcuEmitter x86(lir, "test", Target::OS::Windows);
    static_cast<void>(x86.Generate());
    CHECK(x86.Diagnostics().empty());
    const AArch64RcuEmitter aarch64(lir, "test", Target::OS::Linux);
    static_cast<void>(aarch64.Generate());
    CHECK(aarch64.Diagnostics().empty());
    return lir;
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
    FAIL("missing LIR function " << name);
    throw std::runtime_error("missing LIR function");
}

/// The arms of the match a function returns.
const std::vector<HirMatchArm> &ReturnedMatchArms(const HirFunc &function) {
    REQUIRE(function.body.has_value());
    for (const auto &statement : function.body->stmts) {
        if (const auto *returned = dynamic_cast<const HirReturnStmt *>(statement.get()); returned && returned->value) {
            const auto *match = dynamic_cast<const HirMatchExpr *>(returned->value->get());
            REQUIRE(match != nullptr);
            return match->arms;
        }
    }
    FAIL("no returned match");
    throw std::runtime_error("no returned match");
}

bool StartsWith(const std::string &text, const std::string &prefix) {
    return text.rfind(prefix, 0) == 0;
}
} // namespace

TEST_CASE("structure pattern fields carry their substituted types") {
    const std::string source = R"(
        struct Pair<T> { first: T; second: T; tag: uint8; }
        func Second(pair: Pair<int64>) -> int64 {
            return match pair {
                Pair { second: 7, tag: 1 } => 0i64,
                Pair { second: value } => value
            };
        }
    )";
    const HirPackage package = LowerSource(source);
    const auto &arms = ReturnedMatchArms(RequireFunction(package, "Second"));
    REQUIRE_EQ(arms.size(), 2);
    const auto *literal = dynamic_cast<const HirStructPattern *>(arms[0].pattern.get());
    REQUIRE(literal != nullptr);
    REQUIRE_EQ(literal->fields.size(), 2);
    CHECK_EQ(literal->fields[0].name, "second");
    CHECK_EQ(literal->fields[0].type, TypeRef::MakeInt64());
    // A literal is compared at the width of the field it stands in, not at its own default type.
    const auto *seven = dynamic_cast<const HirLiteralPattern *>(literal->fields[0].pattern.get());
    REQUIRE(seven != nullptr);
    CHECK_EQ(seven->type, TypeRef::MakeInt64());
    const auto *one = dynamic_cast<const HirLiteralPattern *>(literal->fields[1].pattern.get());
    REQUIRE(one != nullptr);
    CHECK_EQ(one->type, TypeRef::MakeUInt8());

    const auto *bound = dynamic_cast<const HirStructPattern *>(arms[1].pattern.get());
    REQUIRE(bound != nullptr);
    const auto *value = dynamic_cast<const HirBindingPattern *>(bound->fields[0].pattern.get());
    REQUIRE(value != nullptr);
    CHECK_EQ(value->type, TypeRef::MakeInt64());
    static_cast<void>(LowerToLir(LowerSource(source)));
}

TEST_CASE("a refutable tuple element branches out before the bindings are set") {
    const LirPackage lir = LowerToLir(LowerSource(R"(
        func Second(pair: (int32, int32)) -> int32 {
            return match pair {
                (0, value) => value,
                else => -1i32
            };
        }
    )"));
    const LirFunc &function = RequireFunction(lir, "Second");
    const auto mismatch = std::ranges::find_if(
        function.blocks, [](const LirBlock &block) { return StartsWith(block.label, "destructure.mismatch"); });
    REQUIRE(mismatch != function.blocks.end());
    const auto mismatchIndex = static_cast<std::uint32_t>(mismatch - function.blocks.begin());

    // The block testing the literal leaves for the mismatch block and stores nothing: the binding is written only on
    // the path where every part matched.
    const auto test = std::ranges::find_if(function.blocks, [&](const LirBlock &block) {
        return block.term && block.term->kind == LirTermKind::Branch && block.term->falseTarget == mismatchIndex;
    });
    REQUIRE(test != function.blocks.end());
    CHECK(std::ranges::any_of(test->instrs,
                              [](const LirInstr &instruction) { return instruction.op == LirOpcode::CmpEq; }));
    const LirBlock &matched = function.blocks[test->term->trueTarget];
    CHECK(std::ranges::any_of(matched.instrs,
                              [](const LirInstr &instruction) { return instruction.op == LirOpcode::Store; }));
}

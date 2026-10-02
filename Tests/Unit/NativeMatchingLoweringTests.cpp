// What native matching lowers to: native case, presence, `none`, typed, and sum-member patterns become tag tests on
// the shared layout, the presence suffix lowers exactly like `.Some`, a subset binding narrows the subject's tag,
// borrowed payloads are named where they lie, and `is` tests a selection without consuming its subject.

#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Target/Target.h"

#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
/// The HIR of `source`. Native values are still stopped before compilation by pending diagnostics, which are the only
/// errors the source may produce; lowering reads the facts analysis recorded anyway.
HirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "native.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    for (const auto &diagnostic : model.diagnostics) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            INFO(diagnostic.message);
            REQUIRE(diagnostic.message.contains("is not supported"));
        }
    }
    AstToHirLowering lowering(model);
    HirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
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

const HirEnumPattern &Case(const HirPattern &pattern, const std::string &tag) {
    const auto *enumeration = dynamic_cast<const HirEnumPattern *>(&pattern);
    REQUIRE(enumeration != nullptr);
    CHECK_EQ(enumeration->form, CaseTypeForm::Variant);
    REQUIRE(enumeration->discriminant.has_value());
    CHECK_EQ(*enumeration->discriminant, tag);
    return *enumeration;
}

void RequireBackEndsAccept(HirPackage package) {
    HirToLirLowering lowering(std::move(package), TargetContext::CreateNative());
    const LirPackage lir = lowering.Generate();
    CHECK(lowering.Diagnostics().empty());
    const RcuEmitter x86(lir, "test", Target::OS::Windows);
    static_cast<void>(x86.Generate());
    CHECK(x86.Diagnostics().empty());
    const AArch64RcuEmitter aarch64(lir, "test", Target::OS::Linux);
    static_cast<void>(aarch64.Generate());
    CHECK(aarch64.Diagnostics().empty());
}
} // namespace

TEST_CASE("native case patterns test the level's tag before its payload") {
    HirPackage package = LowerSource(R"(
        struct E { code: int32; }
        func Nested(outcome: (int32 ! E) ! E) -> int32 {
            return match outcome {
                .Success(.Success(value)) => value,
                .Success(.Failure(inner)) => inner.code,
                .Failure(outer) => outer.code
            };
        }
    )");
    const auto &arms = ReturnedMatchArms(RequireFunction(package, "Nested"));
    REQUIRE_EQ(arms.size(), 3);
    const HirEnumPattern &successSuccess = Case(*arms[0].pattern, "0");
    REQUIRE_EQ(successSuccess.args.size(), 1);
    Case(*successSuccess.args.front(), "0");
    Case(*Case(*arms[1].pattern, "0").args.front(), "1");
    Case(*arms[2].pattern, "1");
    RequireBackEndsAccept(std::move(package));
}

TEST_CASE("the presence suffix lowers exactly like its constructor spelling") {
    HirPackage package = LowerSource(R"(
        func Suffix(found: int32??) -> int32 {
            return match found {
                value?? => value,
                none? => 1i32,
                none => 2i32
            };
        }
        func Spelled(found: int32??) -> int32 {
            return match found {
                .Some(.Some(value)) => value,
                .Some(none) => 1i32,
                none => 2i32
            };
        }
    )");
    const auto &suffix = ReturnedMatchArms(RequireFunction(package, "Suffix"));
    const auto &spelled = ReturnedMatchArms(RequireFunction(package, "Spelled"));
    REQUIRE_EQ(suffix.size(), spelled.size());
    for (std::size_t index = 0; index < suffix.size(); ++index) {
        const HirEnumPattern &left =
            Case(*suffix[index].pattern, *Case(*spelled[index].pattern, index == 2 ? "0" : "1").discriminant);
        const auto &right = static_cast<const HirEnumPattern &>(*spelled[index].pattern);
        CHECK_EQ(left.payloadTypes, right.payloadTypes);
        REQUIRE_EQ(left.args.size(), right.args.size());
        if (!left.args.empty()) {
            const auto *innerLeft = dynamic_cast<const HirEnumPattern *>(left.args.front().get());
            const auto *innerRight = dynamic_cast<const HirEnumPattern *>(right.args.front().get());
            REQUIRE((innerLeft != nullptr) == (innerRight != nullptr));
            if (innerLeft) {
                CHECK_EQ(innerLeft->discriminant, innerRight->discriminant);
            }
        }
    }
    RequireBackEndsAccept(std::move(package));
}

TEST_CASE("typed patterns select members, subsets, and one presence level") {
    HirPackage package = LowerSource(R"(
        struct A {}
        struct B {}
        struct C {}
        func Member(value: A | B | C) -> int32 {
            return match value {
                a: A => 1i32,
                any: B | C => 2i32
            };
        }
        func Present(value: (A | B)?) -> int32 {
            return match value {
                a: A => 1i32,
                b: B => 2i32,
                none => 0i32
            };
        }
    )");
    const auto &member = ReturnedMatchArms(RequireFunction(package, "Member"));
    REQUIRE_EQ(member.size(), 2);
    const HirEnumPattern &single = Case(*member[0].pattern, "0");
    REQUIRE_EQ(single.args.size(), 1);
    CHECK(dynamic_cast<const HirBindingPattern *>(single.args.front().get()) != nullptr);

    // `B | C` are members one and two of the subject and zero and one of the subset.
    const auto *subset = dynamic_cast<const HirNativeSubsetPattern *>(member[1].pattern.get());
    REQUIRE(subset != nullptr);
    CHECK_EQ(subset->subsetType.ToString(), "B | C");
    const std::vector<std::pair<std::string, std::string>> tags = {{"1", "0"}, {"2", "1"}};
    CHECK_EQ(subset->tags, tags);
    const auto *binding = dynamic_cast<const HirBindingPattern *>(subset->binding.get());
    REQUIRE(binding != nullptr);
    CHECK_FALSE(binding->alias);

    const auto &present = ReturnedMatchArms(RequireFunction(package, "Present"));
    REQUIRE_EQ(present.size(), 3);
    const HirEnumPattern &presentB = Case(*present[1].pattern, "1");
    REQUIRE_EQ(presentB.args.size(), 1);
    Case(*presentB.args.front(), "1");
    Case(*present[2].pattern, "0");
    RequireBackEndsAccept(std::move(package));
}

TEST_CASE("a borrowed exclusive subject binds its payloads as aliases") {
    HirPackage package = LowerSource(R"(
        struct Count { value: int32; }
        struct B {}
        struct C {}
        func Bump(value: &var (Count | B | C)) {
            match value {
                count: Count => { count.value = count.value + 1i32; },
                view: B | C => {}
            }
        }
    )");
    const HirFunc &bump = RequireFunction(package, "Bump");
    REQUIRE(bump.body.has_value());
    const auto *match = dynamic_cast<const HirMatchStmt *>(bump.body->stmts.front().get());
    REQUIRE(match != nullptr);
    // Canonical order spells `B | C | Count`, so `Count` is member two.
    const HirEnumPattern &count = Case(*match->arms[0].pattern, "2");
    const auto *alias = dynamic_cast<const HirBindingPattern *>(count.args.front().get());
    REQUIRE(alias != nullptr);
    CHECK(alias->alias);
    CHECK_EQ(alias->bindingId, 0);

    const auto *view = dynamic_cast<const HirNativeSubsetPattern *>(match->arms[1].pattern.get());
    REQUIRE(view != nullptr);
    const auto *viewBinding = dynamic_cast<const HirBindingPattern *>(view->binding.get());
    REQUIRE(viewBinding != nullptr);
    CHECK_FALSE(viewBinding->alias);
    CHECK_EQ(viewBinding->bindingId, 0);
    RequireBackEndsAccept(std::move(package));
}

TEST_CASE("is tests a selection without consuming its subject") {
    HirPackage package = LowerSource(R"(
        struct A {}
        struct B {}
        func Test(value: A | B, count: int32?) -> bool {
            return value is A && count is int32;
        }
    )");
    const HirFunc &test = RequireFunction(package, "Test");
    REQUIRE(test.body.has_value());
    const auto *returned = dynamic_cast<const HirReturnStmt *>(test.body->stmts.front().get());
    REQUIRE(returned != nullptr);
    const auto *both = dynamic_cast<const HirBinaryExpr *>(returned->value->get());
    REQUIRE(both != nullptr);
    for (const HirExpr *side : {both->left.get(), both->right.get()}) {
        const auto *match = dynamic_cast<const HirMatchExpr *>(side);
        REQUIRE(match != nullptr);
        CHECK_FALSE(match->subject->consumption.has_value());
        REQUIRE_EQ(match->arms.size(), 2);
        CHECK(dynamic_cast<const HirWildcardPattern *>(match->arms[1].pattern.get()) != nullptr);
    }
    Case(*dynamic_cast<const HirMatchExpr *>(both->left.get())->arms[0].pattern, "0");
    Case(*dynamic_cast<const HirMatchExpr *>(both->right.get())->arms[0].pattern, "1");
    RequireBackEndsAccept(std::move(package));
}

TEST_CASE("guards and sum-member patterns lower onto the same tag tests") {
    HirPackage package = LowerSource(R"(
        variant DecodeError {
            Missing,
            InvalidDigit(int32)
        }
        struct IoError {}
        func Recover(error: DecodeError | IoError, limit: int32) -> int32 {
            return match error {
                DecodeError::InvalidDigit(position) if position > limit => position,
                DecodeError::InvalidDigit(position) => 0i32,
                DecodeError::Missing => 1i32,
                e: IoError => 2i32
            };
        }
    )");
    const auto &arms = ReturnedMatchArms(RequireFunction(package, "Recover"));
    REQUIRE_EQ(arms.size(), 4);
    const auto *guarded = dynamic_cast<const HirGuardedPattern *>(arms[0].pattern.get());
    REQUIRE(guarded != nullptr);
    // `DecodeError` is member zero of `DecodeError | IoError`; its case is matched inside that member.
    const HirEnumPattern &member = Case(*guarded->inner, "0");
    REQUIRE_EQ(member.args.size(), 1);
    const auto *nominal = dynamic_cast<const HirEnumPattern *>(member.args.front().get());
    REQUIRE(nominal != nullptr);
    CHECK_EQ(nominal->path.back(), "InvalidDigit");
    Case(*arms[3].pattern, "1");
    RequireBackEndsAccept(std::move(package));
}

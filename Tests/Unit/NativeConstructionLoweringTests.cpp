// What native construction lowers to: every constructor, contextual conversion, `return;`, falling off the end, and
// `fail` becomes an explicit tagged construction of the destination level, and widening an existing value becomes a
// match that rebuilds each alternative under the destination's tags.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Types/NativeLayout.h"

#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
/// The HIR of `source`. `none` is still stopped before compilation by a pending diagnostic, which is the only error the
/// source may produce; lowering reads the facts analysis recorded anyway.
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
    for (const auto &diagnostic : lowering.Diagnostics()) {
        INFO(diagnostic.message);
        CHECK(diagnostic.severity != Diagnostic::Severity::Error);
    }
    return package;
}

const HirFunc &RequireFunction(const HirPackage &package, const std::string &name) {
    for (const HirModule &module : package.modules) {
        for (const HirFunc &function : module.funcs) {
            if (function.name == name || function.name.starts_with(name + "<") ||
                function.name.starts_with(name + "_")) {
                return function;
            }
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

const HirExpr &LetInit(const HirFunc &function, const std::size_t index) {
    REQUIRE(function.body.has_value());
    std::size_t seen = 0;
    for (const auto &statement : function.body->stmts) {
        if (const auto *let = dynamic_cast<const HirLetStmt *>(statement.get())) {
            if (seen++ == index) {
                REQUIRE(let->init != nullptr);
                return *let->init;
            }
        }
    }
    FAIL("missing let " << index);
    throw std::runtime_error("missing let");
}

/// The value returned by the `index`th return statement in a function body, looking through deferred-cleanup scopes.
const HirExpr &ReturnedValue(const HirFunc &function, const std::size_t index) {
    REQUIRE(function.body.has_value());
    std::vector<const HirReturnStmt *> returns;
    const auto collect = [&](this auto &&self, const HirBlock &block) -> void {
        for (const auto &statement : block.stmts) {
            if (const auto *returned = dynamic_cast<const HirReturnStmt *>(statement.get())) {
                returns.push_back(returned);
            }
            else if (const auto *scope = dynamic_cast<const HirScopeStmt *>(statement.get())) {
                self(scope->block);
            }
            else if (const auto *conditional = dynamic_cast<const HirIfStmt *>(statement.get())) {
                self(conditional->thenBlock);
            }
        }
    };
    collect(*function.body);
    REQUIRE_GT(returns.size(), index);
    REQUIRE(returns[index]->value.has_value());
    return **returns[index]->value;
}

const HirEnumConstructExpr &Construct(const HirExpr &expression, const std::string &tag) {
    const auto *construct = dynamic_cast<const HirEnumConstructExpr *>(&expression);
    REQUIRE(construct != nullptr);
    CHECK_EQ(construct->form, CaseTypeForm::Variant);
    CHECK_EQ(construct->discriminant, tag);
    return *construct;
}

const HirExpr &Payload(const HirEnumConstructExpr &construct) {
    REQUIRE_EQ(construct.payloads.size(), 1);
    return *construct.payloads.front();
}

const std::string kErrors = R"(
    struct ParseError {}
    struct IoError {}
)";
} // namespace

TEST_CASE("constructors select their own level's tag") {
    const HirPackage package = LowerSource(kErrors + R"(
        func Build(value: int32, error: ParseError) {
            let success: int32 ! ParseError = .Success(value);
            let failure: int32 ! ParseError = .Failure(error);
            let present: int32? = .Some(value);
        }
    )");
    const HirFunc &build = RequireFunction(package, "Build");

    const HirEnumConstructExpr &success = Construct(LetInit(build, 0), std::to_string(NativeSuccessTag));
    CHECK_EQ(success.type, TypeRef::MakeFallible(TypeRef::MakeInt32(), TypeRef::MakeNamed("ParseError")));
    CHECK(dynamic_cast<const HirVarExpr *>(&Payload(success)) != nullptr);

    const HirEnumConstructExpr &failure = Construct(LetInit(build, 1), std::to_string(NativeFailureTag));
    CHECK_EQ(Payload(failure).type.ToString(), "ParseError");

    const HirEnumConstructExpr &present = Construct(LetInit(build, 2), std::to_string(NativePresentTag));
    CHECK_EQ(present.type, TypeRef::MakeOptional(TypeRef::MakeInt32()));
}

TEST_CASE("every nested optional state is built directly") {
    const HirPackage package = LowerSource(R"(
        func States() {
            let storedAbsence: int32?? = .Some(none);
            let storedValue: int32?? = .Some(.Some(7i32));
        }
    )");
    const HirFunc &states = RequireFunction(package, "States");

    const HirEnumConstructExpr &storedAbsence = Construct(LetInit(states, 0), "1");
    const HirEnumConstructExpr &innerAbsence = Construct(Payload(storedAbsence), "0");
    CHECK(innerAbsence.payloads.empty());
    CHECK_EQ(innerAbsence.type, TypeRef::MakeOptional(TypeRef::MakeInt32()));

    const HirEnumConstructExpr &storedValue = Construct(LetInit(states, 1), "1");
    const HirEnumConstructExpr &innerValue = Construct(Payload(storedValue), "1");
    CHECK_EQ(Payload(innerValue).type, TypeRef::MakeInt32());
}

TEST_CASE("contextual conversions wrap presence and success around the value") {
    const HirPackage package = LowerSource(kErrors + R"(
        func Wrap(value: int32) {
            let present: int32? = value;
            let successful: int32? ! ParseError = value;
            let presentOuter: int32?? = present;
        }
    )");
    const HirFunc &wrap = RequireFunction(package, "Wrap");

    const HirEnumConstructExpr &present = Construct(LetInit(wrap, 0), "1");
    CHECK(dynamic_cast<const HirVarExpr *>(&Payload(present)) != nullptr);

    const HirEnumConstructExpr &successful = Construct(LetInit(wrap, 1), "0");
    const HirEnumConstructExpr &innerPresent = Construct(Payload(successful), "1");
    CHECK_EQ(Payload(innerPresent).type, TypeRef::MakeInt32());

    // Identity wins at each level, so an optional assigned to an optional of optionals becomes present.
    const HirEnumConstructExpr &presentOuter = Construct(LetInit(wrap, 2), "1");
    CHECK_EQ(Payload(presentOuter).type, TypeRef::MakeOptional(TypeRef::MakeInt32()));
}

TEST_CASE("a member is injected under its canonical tag") {
    const HirPackage package = LowerSource(R"(
        func Inject(flag: bool) {
            let member: int32 | bool = flag;
            let literal: int64 | bool = 5;
        }
    )");
    const HirFunc &inject = RequireFunction(package, "Inject");
    // Canonical order spells `bool8 | int32`, so the boolean is member zero.
    const HirEnumConstructExpr &member = Construct(LetInit(inject, 0), "0");
    CHECK(member.type.IsSum());
    // An unsuffixed integer takes the one integer member and that member's width.
    const HirEnumConstructExpr &literal = Construct(LetInit(inject, 1), "1");
    CHECK_EQ(Payload(literal).type, TypeRef::MakeInt64());
}

TEST_CASE("widening rebuilds each alternative under the destination's tags") {
    const HirPackage package = LowerSource(kErrors + R"(
        struct A {}
        struct B {}
        struct C {}
        func Widen(pair: A | C, outcome: int32 ! ParseError, optional: A?) {
            let wider: A | B | C = pair;
            let channel: int32 ! (ParseError | IoError) = outcome;
            let payload: (A | B)? = optional;
        }
    )");
    const HirFunc &widen = RequireFunction(package, "Widen");

    const auto *wider = dynamic_cast<const HirMatchExpr *>(&LetInit(widen, 0));
    REQUIRE(wider != nullptr);
    REQUIRE_EQ(wider->arms.size(), 2);
    // `C` is member one of `A | C` and member two of `A | B | C`.
    const auto *fromC = dynamic_cast<const HirEnumPattern *>(wider->arms[1].pattern.get());
    REQUIRE(fromC != nullptr);
    CHECK_EQ(fromC->discriminant, "1");
    Construct(*wider->arms[1].body, "2");

    const auto *channel = dynamic_cast<const HirMatchExpr *>(&LetInit(widen, 1));
    REQUIRE(channel != nullptr);
    REQUIRE_EQ(channel->arms.size(), 2);
    const HirEnumConstructExpr &failure = Construct(*channel->arms[1].body, "1");
    // The failure payload is injected into the wider error sum; the success passes through unchanged.
    Construct(Payload(failure), "1");
    CHECK(dynamic_cast<const HirVarExpr *>(&Payload(Construct(*channel->arms[0].body, "0"))) != nullptr);

    const auto *payload = dynamic_cast<const HirMatchExpr *>(&LetInit(widen, 2));
    REQUIRE(payload != nullptr);
    REQUIRE_EQ(payload->arms.size(), 2);
    CHECK(Construct(*payload->arms[0].body, "0").payloads.empty());
    Construct(Payload(Construct(*payload->arms[1].body, "1")), "0");
}

TEST_CASE("unit completion and fail return explicit outer cases") {
    const HirPackage package = LowerSource(kErrors + R"(
        func Early(stop: bool) -> ! ParseError {
            if stop {
                return;
            }
        }
        func Fails(error: ParseError) -> int32 ! ParseError {
            fail error;
        }
        func Unit() -> () {}
    )");
    const HirFunc &early = RequireFunction(package, "Early");
    const HirEnumConstructExpr &explicitReturn = Construct(ReturnedValue(early, 0), "0");
    CHECK(Payload(explicitReturn).type.IsUnit());
    // Falling off the end returns the same completion.
    const HirEnumConstructExpr &fallThrough = Construct(ReturnedValue(early, 1), "0");
    CHECK(Payload(fallThrough).type.IsUnit());

    const HirFunc &fails = RequireFunction(package, "Fails");
    const HirEnumConstructExpr &failure = Construct(ReturnedValue(fails, 0), "1");
    CHECK_EQ(Payload(failure).type.ToString(), "ParseError");

    const HirFunc &unit = RequireFunction(package, "Unit");
    CHECK(ReturnedValue(unit, 0).type.IsUnit());
}

TEST_CASE("conversions follow the substituted types of each instantiation") {
    const HirPackage package = LowerSource(R"(
        func Wrap<T>(value: T) -> T? {
            return value;
        }
        func Choose<T, U>(left: T) -> T | U {
            return left;
        }
        func Use() {
            let wrapped = Wrap<int32>(1i32);
            let same = Choose<int32, int32>(1i32);
            let different = Choose<int64, int32>(1i64);
        }
    )");
    std::vector<const HirFunc *> chooses;
    const HirFunc *wrap = nullptr;
    for (const HirModule &module : package.modules) {
        for (const HirFunc &function : module.funcs) {
            if (function.name.starts_with("Choose") && function.typeParams.empty() && function.body) {
                chooses.push_back(&function);
            }
            if (function.name.starts_with("Wrap") && function.typeParams.empty() && function.body) {
                wrap = &function;
            }
        }
    }
    REQUIRE(wrap != nullptr);
    Construct(ReturnedValue(*wrap, 0), "1");

    REQUIRE_EQ(chooses.size(), 2);
    for (const HirFunc *choose : chooses) {
        const HirExpr &returned = ReturnedValue(*choose, 0);
        if (choose->returnType == TypeRef::MakeInt32()) {
            // `int32 | int32` collapsed to `int32`, so the value returns unchanged.
            CHECK(dynamic_cast<const HirEnumConstructExpr *>(&returned) == nullptr);
        }
        else {
            // `int64 | int32` orders `int32` first, so `T = int64` is member one.
            Construct(returned, "1");
        }
    }
}

TEST_CASE("diverging arm bodies lower to the statement they spell") {
    const HirPackage package = LowerSource(R"(
        func Pick(value: int32) -> int32 {
            match value {
                1i32 => return 1i32,
                else => {}
            }
            return 0i32;
        }
    )");
    const HirFunc &pick = RequireFunction(package, "Pick");
    REQUIRE(pick.body.has_value());
    const auto *match = dynamic_cast<const HirMatchStmt *>(pick.body->stmts.front().get());
    REQUIRE(match != nullptr);
    const auto *body = dynamic_cast<const HirBlockExpr *>(match->arms.front().body.get());
    REQUIRE(body != nullptr);
    REQUIRE_EQ(body->block.stmts.size(), 1);
    CHECK(dynamic_cast<const HirReturnStmt *>(body->block.stmts.front().get()) != nullptr);
}

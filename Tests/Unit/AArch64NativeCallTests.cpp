// Native sums, optionals, fallibles, and the unit cross AArch64 calls as ordinary integer-class aggregates: up to
// sixteen bytes travel in general registers, wider values by reference, a float payload never makes a native level a
// homogeneous float aggregate because its tag is an integer, and whole programs generate code for every AArch64 target.

#include "CodeGen/AArch64/CallLayout.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Layout;

namespace {
const TypeRef kBig = TypeRef::MakeNamed("Big");
const TypeRef kError = TypeRef::MakeNamed("E");

LayoutMap Layouts() {
    LayoutMap layouts;
    layouts["Big"] = StructLayout{.fields = {}, .totalSize = 32, .alignment = 8};
    layouts["E"] = StructLayout{.fields = {}, .totalSize = 4, .alignment = 4};
    return layouts;
}

/// The LIR of `source` for `triple`, which must analyze without errors.
LirPackage LowerFor(const std::string &source, const std::string_view triple) {
    Lexer lexer(source, "native.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    const auto parsedTriple = Target::TargetTriple::Parse(triple);
    REQUIRE(parsedTriple.has_value());
    const auto target = Driver::TargetContextForTriple(*parsedTriple);
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", std::string(triple.substr(0, triple.find('-'))));
    const SemanticModel model = analyzer.Analyze();
    for (const auto &diagnostic : model.diagnostics) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            INFO(diagnostic.message);
            FAIL_CHECK(diagnostic.message);
        }
    }
    AstToHirLowering hirLowering(model);
    HirPackage hir = hirLowering.Generate();
    REQUIRE(hirLowering.Diagnostics().empty());
    HirToLirLowering lirLowering(std::move(hir), target);
    LirPackage lir = lirLowering.Generate();
    REQUIRE(lirLowering.Diagnostics().empty());
    return lir;
}
} // namespace

TEST_CASE("AArch64 returns native values by their size as integer aggregates") {
    static const std::unordered_set<std::string> noInterfaces;
    static const std::vector<LirStructDecl> noDeclarations;
    const LayoutMap layouts = Layouts();
    for (const Target::OS os : {Target::OS::Linux, Target::OS::Windows, Target::OS::MacOS}) {
        CAPTURE(static_cast<int>(os));
        const AArch64CallPlanner planner(layouts, noInterfaces, noDeclarations, os);

        const AArch64ArgumentLocation optional = planner.PlanResult(TypeRef::MakeOptional(TypeRef::MakeInt32()));
        CHECK_EQ(optional.kind, AArch64ArgumentLocation::Kind::General);
        CHECK_EQ(optional.count, 2);

        // The tag is an integer, so a float payload does not make the level a homogeneous float aggregate.
        const AArch64ArgumentLocation floating = planner.PlanResult(TypeRef::MakeOptional(TypeRef::MakeFloat64()));
        CHECK_EQ(floating.kind, AArch64ArgumentLocation::Kind::General);
        CHECK_EQ(floating.count, 2);

        const AArch64ArgumentLocation tagOnly = planner.PlanResult(TypeRef::MakeOptional(TypeRef::MakeUnit()));
        CHECK_EQ(tagOnly.kind, AArch64ArgumentLocation::Kind::General);
        CHECK_EQ(tagOnly.count, 1);

        CHECK_FALSE(planner.ReturnsInMemory(TypeRef::MakeOptional(TypeRef::MakeInt32())));
        CHECK(planner.ReturnsInMemory(TypeRef::MakeFallible(kBig, kError)));
    }
}

TEST_CASE("AArch64 passes native arguments in register pairs and wide ones by reference") {
    static const std::unordered_set<std::string> noInterfaces;
    static const std::vector<LirStructDecl> noDeclarations;
    const LayoutMap layouts = Layouts();
    const AArch64CallPlanner planner(layouts, noInterfaces, noDeclarations, Target::OS::Linux);
    const TypeRef optional = TypeRef::MakeOptional(TypeRef::MakeInt32());
    const AArch64CallLayout layout =
        planner.PlanArguments({optional, optional, optional, optional, optional, TypeRef::MakeFallible(kBig, kError)});
    REQUIRE_EQ(layout.args.size(), 6);
    for (std::size_t index = 0; index < 4; ++index) {
        CHECK_EQ(layout.args[index].kind, AArch64ArgumentLocation::Kind::General);
        CHECK_EQ(layout.args[index].first, index * 2);
        CHECK_EQ(layout.args[index].count, 2);
    }
    // Eight general registers hold four two-word optionals; the fifth goes to the stack whole.
    CHECK_EQ(layout.args[4].kind, AArch64ArgumentLocation::Kind::Stack);
    CHECK_EQ(layout.args[4].bytes, 16);
    // A native value wider than sixteen bytes travels as the address of a caller-owned copy.
    CHECK(layout.args[5].byReference);
}

TEST_CASE("native arguments and results generate code for every AArch64 target") {
    const std::string program = R"(
        struct E { code: int32; }
        struct Big { a: int64; b: int64; c: int64; d: int64; }
        func UnitReturn() -> () {}
        func UnitParam(value: (), next: int32) -> int32 { return next; }
        func Value(value: float64?) -> float64 {
            return match value { v? => v, none => 0.0f64 };
        }
        func Many(a: float64?, b: float64?, c: float64?, d: float64?, e: float64?, f: float64?) -> float64 {
            return Value(a) + Value(b) + Value(c) + Value(d) + Value(e) + Value(f);
        }
        func Total(value: Big ! E) -> int64 {
            return match value { .Success(big) => big.a + big.d, .Failure(e) => e.code as int64 };
        }
        func BigMany(a: Big ! E, b: Big ! E, c: Big ! E, d: Big ! E, e: Big ! E, f: Big ! E, g: Big ! E, h: Big ! E,
                     i: Big ! E) -> int64 {
            return Total(a) + Total(b) + Total(i);
        }
        func Make(seed: int64) -> Big ! E {
            return Big { a: seed, b: seed, c: seed, d: seed };
        }
        func Nest() -> ((int32 ! E)?) ! E {
            return .Success(.Some(.Failure(E { code: 5i32 })));
        }
        func Main() -> int {
            let unit = UnitReturn();
            let unitParam = UnitParam(unit, 4i32);
            let missing: float64? = none;
            let many = Many(1.0f64, 2.0f64, 3.0f64, missing, 5.0f64, 6.0f64);
            let big = BigMany(Make(1i64), Make(2i64), Make(3i64), Make(4i64), Make(5i64), Make(6i64), Make(7i64),
                              Make(8i64), Make(9i64));
            let nested = Nest();
            return 0;
        }
    )";
    for (const std::string_view triple : {"linux-aarch64", "windows-aarch64", "macos-aarch64", "freebsd-aarch64"}) {
        CAPTURE(triple);
        const LirPackage package = LowerFor(program, triple);
        const auto os = Driver::TargetContextForTriple(*Target::TargetTriple::Parse(triple)).os;
        const AArch64RcuEmitter emitter(package, "test", os);
        const auto objects = emitter.Generate();
        CHECK_FALSE(objects.empty());
        CHECK(emitter.Diagnostics().empty());
    }
}

// Native sums, optionals, fallibles, and the unit cross x86-64 calls as ordinary aggregates: a result returns through
// registers or a hidden pointer by the same size rules as a struct of that size on each target, and whole programs
// passing native values through registers and the stack generate code for every x86-64 target.

#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Target/Platform.h"

#include <doctest.h>
#include <string>
#include <unordered_set>
#include <utility>

using namespace Rux;
using namespace Rux::Layout;

namespace {
/// Plans a body-less function returning `returnType`, which is all a hidden-return decision depends on.
X86_64FramePlan PlanReturning(const TypeRef &returnType, const LayoutMap &layouts, const Target::OS os) {
    LirFunc function;
    function.name = "Returns";
    function.callConv = os == Target::OS::Windows ? CallingConvention::Win64 : CallingConvention::SysV;
    function.returnType = returnType;
    LirBlock entry;
    LirTerminator terminator;
    terminator.kind = LirTermKind::Return;
    entry.term = terminator;
    function.blocks.push_back(std::move(entry));
    return PlanX86_64Frame(function, layouts, {}, os);
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

const std::string kCallProgram = R"(
    struct E { code: int32; }
    struct Big { a: int64; b: int64; c: int64; d: int64; }
    func UnitReturn() -> () {}
    func UnitParam(value: (), next: int32) -> int32 { return next; }
    func Value(value: int32?) -> int32 {
        return match value { v? => v, none => 0i32 };
    }
    func Many(a: int32?, b: int32?, c: int32?, d: int32?, e: int32?, f: int32?, g: int32?, h: int32?) -> int32 {
        return Value(a) + Value(b) + Value(c) + Value(d) + Value(e) + Value(f) + Value(g) + Value(h);
    }
    func Total(value: Big ! E) -> int64 {
        return match value { .Success(big) => big.a + big.d, .Failure(e) => e.code as int64 };
    }
    func BigMany(a: Big ! E, b: Big ! E, c: Big ! E, d: Big ! E, e: Big ! E) -> int64 {
        return Total(a) + Total(b) + Total(c) + Total(d) + Total(e);
    }
    func Make(seed: int64) -> Big ! E {
        return Big { a: seed, b: seed, c: seed, d: seed };
    }
    func Tag(flag: bool) -> ()? {
        return flag ? .Some(()) : none;
    }
    func Nest() -> ((int32 ! E)?) ! E {
        return .Success(.Some(.Failure(E { code: 5i32 })));
    }
    func Collapse<T, U>(value: T) -> T | U {
        return value;
    }
    func Main() -> int {
        let unit = UnitReturn();
        let unitParam = UnitParam(unit, 4i32);
        let missing: int32? = none;
        let many = Many(1i32, 2i32, 3i32, 4i32, 5i32, missing, 7i32, 8i32);
        let big = BigMany(Make(1i64), Make(2i64), Make(3i64), Make(4i64), Make(5i64));
        let tag = Tag(true);
        let nested = Nest();
        let collapsed = Collapse<int32, int32>(1i32);
        return 0;
    }
)";
} // namespace

TEST_CASE("a native result returns by the size rules of an aggregate of its size") {
    LayoutMap layouts;
    layouts["Big"] = StructLayout{.fields = {}, .totalSize = 32, .alignment = 8};
    layouts["E"] = StructLayout{.fields = {}, .totalSize = 4, .alignment = 4};
    const TypeRef optional = TypeRef::MakeOptional(TypeRef::MakeInt32());
    const TypeRef tagOnly = TypeRef::MakeOptional(TypeRef::MakeUnit());
    const TypeRef large = TypeRef::MakeFallible(TypeRef::MakeNamed("Big"), TypeRef::MakeNamed("E"));

    // System V returns sixteen bytes in two registers; Win64 returns only 1, 2, 4, or 8 bytes in one.
    CHECK_EQ(PlanReturning(optional, layouts, Target::OS::Linux).HiddenReturnOffset(), 0);
    CHECK_NE(PlanReturning(optional, layouts, Target::OS::Windows).HiddenReturnOffset(), 0);
    // A tag-only native level is one word on every target.
    CHECK_EQ(PlanReturning(tagOnly, layouts, Target::OS::Linux).HiddenReturnOffset(), 0);
    CHECK_EQ(PlanReturning(tagOnly, layouts, Target::OS::Windows).HiddenReturnOffset(), 0);
    // A payload wider than two words returns through a hidden pointer everywhere.
    CHECK_NE(PlanReturning(large, layouts, Target::OS::Linux).HiddenReturnOffset(), 0);
    CHECK_NE(PlanReturning(large, layouts, Target::OS::Windows).HiddenReturnOffset(), 0);
}

TEST_CASE("native arguments and results generate code for every x86-64 target") {
    for (const std::string_view triple : {"windows-x86_64", "linux-x86_64", "macos-x86_64", "freebsd-x86_64"}) {
        CAPTURE(triple);
        const LirPackage package = LowerFor(kCallProgram, triple);
        const auto os = Driver::TargetContextForTriple(*Target::TargetTriple::Parse(triple)).os;
        const RcuEmitter emitter(package, "test", os);
        const auto objects = emitter.Generate();
        CHECK_FALSE(objects.empty());
        CHECK(emitter.Diagnostics().empty());
    }
}

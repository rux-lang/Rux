// Constants computed by calling functions: the value analysis records, the operations a callee may not use, the
// limits, and constants whose callees live in a module whose code the build reuses.

#include "Evaluation/TypedConstant.h"
#include "Lexer/Lexer.h"
#include "Parallel/WorkerPool.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace Rux;

namespace {
/// Parsed modules and the model analyzed from them, which refers into them.
struct Analyzed {
    std::vector<std::unique_ptr<Module>> modules;
    SemanticModel model;

    [[nodiscard]] const ConstDecl &Constant(const std::string &name) const {
        for (const auto &module : modules)
            for (const auto &item : module->items)
                if (const auto *constant = dynamic_cast<const ConstDecl *>(item.get());
                    constant && constant->name == name)
                    return *constant;
        FAIL("no constant named ", name);
        throw;
    }

    [[nodiscard]] const ComputedConstant &Value(const std::string &name) const {
        const ComputedConstant *value = model.TryGetEvaluatedConstant(Constant(name));
        REQUIRE_MESSAGE(value != nullptr, "constant '", name, "' was not computed");
        return *value;
    }

    [[nodiscard]] bool Reports(const std::string &message) const {
        for (const auto &diagnostic : model.diagnostics)
            if (diagnostic.message == message)
                return true;
        return false;
    }
};

std::unique_ptr<Module> Parse(const std::string &source, const std::string &name) {
    Lexer lexer(source, name);
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), name);
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return std::make_unique<Module>(std::move(parsed.module));
}

/// Analyze `source` as a package of its own, or with `dependency` as the package `Dep`, whose code the build reuses
/// when `reuseDependency` is set, on `pool` when one is given.
Analyzed Analyze(const std::string &source, const std::string &dependency = {}, const bool reuseDependency = false,
                 Parallel::WorkerPool *pool = nullptr) {
    Analyzed result{{}, SemanticModel({}, {}, {}, {}, {})};
    std::vector<DepPackage> deps;
    if (!dependency.empty()) {
        result.modules.push_back(Parse(dependency, "dep.rux"));
        DepPackage package;
        package.name = "Dep";
        package.modules.push_back({"Dep", result.modules.back().get()});
        deps.push_back(std::move(package));
    }
    result.modules.push_back(Parse(source, "test.rux"));
    SemanticAnalyzer analyzer({result.modules.back().get()}, std::move(deps), "test", "Windows");
    if (reuseDependency)
        analyzer.SetDeclarationOnlyModules({result.modules.front().get()});
    analyzer.SetWorkerPool(pool);
    result.model = analyzer.Analyze();
    return result;
}
} // namespace

TEST_CASE("a constant that calls a function records the value the call computed") {
    const Analyzed analyzed = Analyze(R"(
        struct Pair { low: int32; high: int32; }

        func Table(seed: uint32) -> uint32[4] {
            var values: uint32[4] = [0u32; 4];
            var index = 0;
            while index < 4 {
                values[index] = seed << (index as uint32);
                index++;
            }
            return values;
        }

        func Ordered(first: int32, second: int32) -> Pair {
            if first < second { return Pair { low: first, high: second }; }
            return Pair { low: second, high: first };
        }

        const Shifted = Table(3u32);
        const Sorted = Ordered(9i32, -2i32);
        const Plain = 4 + 5;
    )");
    REQUIRE(analyzed.model.diagnostics.empty());

    const ComputedConstant &shifted = analyzed.Value("Shifted");
    CHECK(shifted.kind == ComputedConstant::Kind::Array);
    REQUIRE_EQ(shifted.elements.size(), 4);
    CHECK_EQ(shifted.elements[0].literal, "3");
    CHECK_EQ(shifted.elements[3].literal, "24");
    CHECK_EQ(shifted.type.ToString(), "uint32[4]");

    const ComputedConstant &sorted = analyzed.Value("Sorted");
    CHECK(sorted.kind == ComputedConstant::Kind::Struct);
    REQUIRE_EQ(sorted.fieldNames.size(), 2);
    CHECK_EQ(sorted.fieldNames[0], "low");
    CHECK_EQ(sorted.elements[0].literal, "-2");
    CHECK_EQ(sorted.elements[1].literal, "9");

    // A constant that calls nothing is folded as before and records no computed value.
    CHECK(analyzed.model.TryGetEvaluatedConstant(analyzed.Constant("Plain")) == nullptr);
}

TEST_CASE("a constant may not call a function that uses what only the running program has") {
    const Analyzed analyzed = Analyze(R"(
        var Total: int = 0;

        func Untaken(flag: bool) -> int {
            if flag { return Total; }
            return 0;
        }
        func Outer() -> int { return Untaken(false) + 1; }
        func Address() -> int {
            var local = 1;
            let pointer = @local;
            return local;
        }

        const FromBranch = Outer();
        const FromAddress = Address();
    )");
    // The whole body is examined, so a path the arguments never take still disqualifies the function.
    CHECK(analyzed.Reports("constant 'FromBranch' cannot be computed at compile time: 'Untaken' reads module variable "
                           "'Total' (in 'Outer' -> 'Untaken')"));
    CHECK(analyzed.Reports("constant 'FromAddress' cannot be computed at compile time: 'Address' uses a raw pointer of "
                           "type '*var int' (in 'Address')"));
}

TEST_CASE("a constant reports overflow, division by zero and the evaluation limits") {
    const Analyzed analyzed = Analyze(R"(
        func Grow(value: uint16) -> uint16 { return value * 300u16; }
        func Split(value: int64) -> int64 { return value % (value - value); }
        func Forever() -> int {
            var count = 0;
            loop { count = count ^ 1; }
            return count;
        }
        func Deeper(depth: int) -> int { return Deeper(depth + 1); }

        const Grown = Grow(300u16);
        const Remainder = Split(4i64);
        const Endless = Forever();
        const Deepest = Deeper(0);
    )");
    CHECK(analyzed.Reports("constant 'Grown' cannot be computed at compile time: '*' overflows 'uint16' (in 'Grow')"));
    CHECK(
        analyzed.Reports("constant 'Remainder' cannot be computed at compile time: '%' divides by zero (in 'Split')"));
    CHECK(analyzed.Reports("constant 'Endless' cannot be computed at compile time: evaluation took more than "
                           "4000000 steps (in 'Forever')"));
    CHECK(analyzed.Reports("constant 'Deepest' cannot be computed at compile time: calls nest more than 64 deep (in "
                           "'Deeper' -> 'Deeper' -> 'Deeper' -> ... -> 'Deeper' -> 'Deeper', 64 calls deep)"));
}

TEST_CASE("a constant reading a computed one is reported once") {
    const Analyzed analyzed = Analyze(R"(
        func Broken() -> int8 { return 100i8 + 100i8; }
        const First = Broken();
        const Second = First + Broken();
    )");
    // 'Second' fails because 'First' does, and only 'First' says why.
    REQUIRE_EQ(analyzed.model.diagnostics.size(), 1);
    CHECK(analyzed.Reports("constant 'First' cannot be computed at compile time: '+' overflows 'int8' (in 'Broken')"));
    CHECK(analyzed.model.TryGetEvaluatedConstant(analyzed.Constant("Second")) == nullptr);
}

TEST_CASE("a constant calls into a module whose code the build reuses") {
    const std::string dependency = R"(
        pub func Cube(value: int) -> int { return value * value * value; }
    )";
    const std::string source = R"(
        import Dep::Cube;
        const Cubed = Cube(4);
        func Main() -> int { return Cubed; }
    )";
    for (const bool reuse : {false, true}) {
        CAPTURE(reuse);
        const Analyzed analyzed = Analyze(source, dependency, reuse);
        REQUIRE(analyzed.model.diagnostics.empty());
        CHECK_EQ(analyzed.Value("Cubed").literal, "64");
    }
}

TEST_CASE("constants computed while bodies are checked on several threads are computed in module order") {
    Parallel::WorkerPool pool(4);
    const Analyzed analyzed = Analyze(R"(
        func Seven() -> int { return 7; }
        func Use() -> int {
            const Local = Seven() * 2;
            return Local;
        }
        const Global = Seven();
    )",
                                      {}, false, &pool);
    REQUIRE(analyzed.model.diagnostics.empty());
    CHECK_EQ(analyzed.Value("Global").literal, "7");
}

TEST_CASE("checked constant arithmetic traps where the integer model does") {
    using namespace Evaluation;
    const auto int8 = [](const char *text) { return *ParseConstant(text, TypeRef::MakeInt8()); };
    const auto uint8 = [](const char *text) {
        return *ParseConstant(text, TypeRef::MakePrimitive(TypeRef::Kind::UInt8));
    };

    CHECK(EvaluateBinaryChecked(TokenKind::Plus, int8("100"), int8("27")).value->ToLiteral() == "127");
    CHECK(EvaluateBinaryChecked(TokenKind::Plus, int8("100"), int8("28")).failure == CheckedFailure::Overflow);
    CHECK(EvaluateBinaryChecked(TokenKind::Minus, int8("-100"), int8("29")).failure == CheckedFailure::Overflow);
    CHECK(EvaluateBinaryChecked(TokenKind::Minus, uint8("1"), uint8("2")).failure == CheckedFailure::Overflow);
    CHECK(EvaluateBinaryChecked(TokenKind::Star, uint8("16"), uint8("16")).failure == CheckedFailure::Overflow);
    CHECK(EvaluateBinaryChecked(TokenKind::Star, int8("-8"), int8("16")).value->ToLiteral() == "-128");
    CHECK(EvaluateBinaryChecked(TokenKind::Slash, int8("-128"), int8("-1")).failure == CheckedFailure::Overflow);
    CHECK(EvaluateBinaryChecked(TokenKind::Percent, int8("5"), int8("0")).failure == CheckedFailure::DivisionByZero);
    CHECK(EvaluateBinaryChecked(TokenKind::LessLess, uint8("1"), uint8("8")).failure ==
          CheckedFailure::ShiftOutOfRange);
    CHECK(EvaluateBinaryChecked(TokenKind::LessLess, uint8("1"), uint8("7")).value->ToLiteral() == "128");
    CHECK(EvaluateUnaryChecked(TokenKind::Minus, int8("-128")).failure == CheckedFailure::Overflow);
    CHECK(EvaluateUnaryChecked(TokenKind::Minus, uint8("1")).failure == CheckedFailure::Overflow);
    CHECK(EvaluateUnaryChecked(TokenKind::Minus, uint8("0")).value->ToLiteral() == "0");
}

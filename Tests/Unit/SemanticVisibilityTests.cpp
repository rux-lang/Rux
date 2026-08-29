#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
using Source = std::pair<std::string, std::string>;

std::unique_ptr<ParseResult> ParseSource(const Source &source) {
    Lexer lexer(source.second, source.first);
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), source.first);
    auto parsed = std::make_unique<ParseResult>(parser.Parse());
    std::string reports;
    for (const Diagnostic &diagnostic : parsed->diagnostics) {
        reports += (reports.empty() ? "" : " | ") + diagnostic.message;
    }
    REQUIRE_MESSAGE(!parsed->HasErrors(), source.first, ": ", reports);
    return parsed;
}

std::vector<SemanticDiagnostic> AnalyzePackages(const std::vector<Source> &userSources,
                                                const std::vector<Source> &dependencySources = {}) {
    std::vector<std::unique_ptr<ParseResult>> dependencies;
    DepPackage dependency;
    dependency.name = "Library";
    for (const Source &source : dependencySources) {
        dependencies.push_back(ParseSource(source));
        dependency.modules.push_back({source.first, &dependencies.back()->module});
    }

    std::vector<std::unique_ptr<ParseResult>> users;
    std::vector<Module *> userModules;
    for (const Source &source : userSources) {
        users.push_back(ParseSource(source));
        userModules.push_back(&users.back()->module);
    }

    std::vector<DepPackage> inputDependencies;
    if (!dependencySources.empty()) {
        inputDependencies.push_back(std::move(dependency));
    }
    SemanticAnalyzer analyzer(std::move(userModules), std::move(inputDependencies), "Application", "Windows");
    return analyzer.Analyze().diagnostics;
}

bool HasMessage(const std::vector<SemanticDiagnostic> &diagnostics, const std::string_view text) {
    return std::ranges::any_of(
        diagnostics, [text](const SemanticDiagnostic &diagnostic) { return diagnostic.message.contains(text); });
}

const SemanticDiagnostic *FindMessage(const std::vector<SemanticDiagnostic> &diagnostics, const std::string_view text) {
    const auto found = std::ranges::find_if(
        diagnostics, [text](const SemanticDiagnostic &diagnostic) { return diagnostic.message.contains(text); });
    return found == diagnostics.end() ? nullptr : &*found;
}

std::string Messages(const std::vector<SemanticDiagnostic> &diagnostics) {
    std::string result;
    for (const SemanticDiagnostic &diagnostic : diagnostics) {
        result += (result.empty() ? "" : " | ") + diagnostic.message;
    }
    return result;
}
} // namespace

TEST_CASE("private declarations and members are shared by every file in their package") {
    const auto diagnostics = AnalyzePackages({
        {"first.rux", R"(
            struct Secret { value: int; }
            extend Secret {
                func Secret(value: int) -> Secret { return Secret { value: value }; }
                func Read(self: &Secret) -> int { return self.value; }
            }
            func MakeSecret(value: int) -> Secret { return Secret(value); }
        )"},
        {"second.rux", R"(
            func Main() -> int {
                let secret = MakeSecret(7);
                return secret.Read() + secret.value;
            }
        )"},
    });
    CHECK(diagnostics.empty());
}

TEST_CASE("explicit imports diagnose every private declaration kind with its source and remediation") {
    const auto diagnostics = AnalyzePackages({{"main.rux", R"(
            import Library::{ HiddenAlias, HiddenConst, HiddenEnum, HiddenExtern, HiddenFunc, HiddenInterface,
                              HiddenStruct, HiddenUnion, HiddenVariable };
            func Main() {}
        )"}},
                                             {{"api.rux", R"(
            type HiddenAlias = int;
            const HiddenConst: int = 1;
            enum HiddenEnum { Value }
            func HiddenFunc() {}
            interface HiddenInterface {}
            struct HiddenStruct {}
            union HiddenUnion { value: int }
            #Link("system.dll")
            extern {
                func HiddenExtern();
                HiddenVariable: int;
            }
        )"}});

    for (const std::string_view name : {"HiddenAlias", "HiddenConst", "HiddenEnum", "HiddenExtern", "HiddenFunc",
                                        "HiddenInterface", "HiddenStruct", "HiddenUnion", "HiddenVariable"}) {
        const SemanticDiagnostic *diagnostic = FindMessage(diagnostics, std::string(name) + "' is private");
        REQUIRE_MESSAGE(diagnostic != nullptr, name);
        CHECK(diagnostic->sourceName == "main.rux");
        REQUIRE_FALSE(diagnostic->notes.empty());
        CHECK(diagnostic->notes.front().contains("api.rux"));
        REQUIRE(diagnostic->help.has_value());
        CHECK(diagnostic->help->contains("pub"));
    }
}

TEST_CASE("glob imports omit private declarations while retaining public declarations") {
    const auto accepted = AnalyzePackages({{"main.rux", R"(
        import Library::*;
        func Main() -> int { return Visible(); }
    )"}},
                                          {{"api.rux", R"(
        func Hidden() -> int { return 1; }
        pub func Visible() -> int { return 2; }
    )"}});
    CHECK(accepted.empty());

    const auto rejected = AnalyzePackages({{"main.rux", R"(
        import Library::*;
        func Main() -> int { return Hidden(); }
    )"}},
                                          {{"api.rux", R"(
        func Hidden() -> int { return 1; }
        pub func Visible() -> int { return 2; }
    )"}});
    CHECK(HasMessage(rejected, "name 'Hidden' is not defined"));
    CHECK_FALSE(HasMessage(rejected, "private to package"));
}

TEST_CASE("public module paths are required for qualified access") {
    const auto accepted = AnalyzePackages({{"main.rux", R"(
        import Library::Api::Nested::Open;
        func Main() -> int { return Open(); }
    )"}},
                                          {{"api.rux", R"(
        pub module Api::Nested {
            pub func Open() -> int { return 3; }
        }
    )"}});
    CHECK(accepted.empty());

    const auto privateItem =
        AnalyzePackages({{"main.rux", "import Library::Api::Nested::Hidden;\nfunc Main() {}"}}, {{"api.rux", R"(
        pub module Api::Nested { func Hidden() {} }
    )"}});
    CHECK(HasMessage(privateItem, "function 'Hidden' is private to package 'Library'"));

    const auto privateModule =
        AnalyzePackages({{"main.rux", "import Library::Sealed::Marked;\nfunc Main() {}"}}, {{"api.rux", R"(
        module Sealed { pub func Marked() {} }
    )"}});
    CHECK(HasMessage(privateModule, "module 'Sealed' is private to package 'Library'"));
}

TEST_CASE("a public module fragment publishes the shared logical module") {
    const auto diagnostics =
        AnalyzePackages({{"main.rux", R"(
            import Library::Shared::Earlier;
            func Main() -> int { return Earlier(); }
        )"}},
                        {{"earlier.rux", "module Shared { pub func Earlier() -> int { return 1; } }"},
                         {"published.rux", "pub module Shared { pub func Marker() {} }"}});
    CHECK_MESSAGE(diagnostics.empty(), Messages(diagnostics));
}

TEST_CASE("mixed overload imports expose only public overloads") {
    const auto accepted = AnalyzePackages({{"main.rux", R"(
        import Library::Choose;
        func Main() -> int { return Choose(4); }
    )"}},
                                          {{"api.rux", R"(
        pub func Choose(value: int) -> int { return value; }
        func Choose(value: bool) -> int { return 0; }
    )"}});
    CHECK(accepted.empty());

    const auto rejected = AnalyzePackages({{"main.rux", R"(
        import Library::Choose;
        func Main() -> int { return Choose(true); }
    )"}},
                                          {{"api.rux", R"(
        pub func Choose(value: int) -> int { return value; }
        func Choose(value: bool) -> int { return 0; }
    )"}});
    REQUIRE_MESSAGE(HasMessage(rejected, "parameter 'value' requires 'int'"), Messages(rejected));
    for (const SemanticDiagnostic &diagnostic : rejected) {
        for (const std::string &note : diagnostic.notes) {
            CHECK_FALSE(note.contains("value: bool"));
        }
    }
}

TEST_CASE("public aggregate members constructors associated functions methods and operators cross packages") {
    const auto diagnostics = AnalyzePackages({{"main.rux", R"(
        import Library::{ Bits, Counter };
        func Main() -> int {
            let left = Counter(4);
            let right = Counter::Zero();
            let total = left + right;
            let bits = Bits { signed: total.Read() as int32 };
            return bits.signed as int;
        }
    )"}},
                                             {{"api.rux", R"(
        pub struct Counter { pub value: int; hidden: int; }
        extend Counter {
            pub func Counter(value: int) -> Counter { return Counter { value: value, hidden: 0 }; }
            pub func Zero() -> Counter { return Counter(0); }
            pub func Read(self: &Counter) -> int { return self.value; }
            pub func +(self: &Counter, other: Counter) -> Counter { return Counter(self.value + other.value); }
        }
        pub union Bits { pub signed: int32, raw: uint32 }
    )"}});
    CHECK(diagnostics.empty());
}

TEST_CASE("generic instantiations enforce operator visibility from the generic package") {
    const std::string dependency = R"(
        pub func Before<T>(left: T, right: T) -> bool { return left < right; }
    )";

    const auto accepted = AnalyzePackages({{"main.rux", R"(
        import Library::Before;
        pub struct Ranked { value: int; }
        extend Ranked {
            pub func <(self: &Ranked, other: Ranked) -> bool { return self.value < other.value; }
        }
        func Main() -> bool {
            return Before<Ranked>(Ranked { value: 1 }, Ranked { value: 2 });
        }
    )"}},
                                          {{"api.rux", dependency}});
    CHECK_MESSAGE(accepted.empty(), Messages(accepted));

    const auto rejected = AnalyzePackages({{"main.rux", R"(
        import Library::Before;
        pub struct Ranked { value: int; }
        extend Ranked {
            func <(self: &Ranked, other: Ranked) -> bool { return self.value < other.value; }
        }
        func Main() -> bool {
            return Before<Ranked>(Ranked { value: 1 }, Ranked { value: 2 });
        }
    )"}},
                                          {{"api.rux", dependency}});
    CHECK_MESSAGE(HasMessage(rejected, "operator '<' is not defined for 'Ranked'"), Messages(rejected));
}

TEST_CASE("private fields methods constructors associated functions and operators are rejected across packages") {
    const std::string dependency = R"(
        pub struct Secret { pub value: int; hidden: int; }
        extend Secret {
            func Secret(value: int) -> Secret { return Secret { value: value, hidden: 0 }; }
            func Make() -> Secret { return Secret { value: 1, hidden: 0 }; }
            func Read(self: &Secret) -> int { return self.hidden; }
            func +(self: &Secret, other: Secret) -> Secret { return other; }
        }
        pub func Seed() -> Secret { return Secret { value: 1, hidden: 2 }; }
        pub union Bits { pub open: int32, closed: uint32 }
    )";

    const auto field = AnalyzePackages(
        {{"main.rux", "import Library::Seed;\nfunc Main() -> int { let value = Seed(); return value.hidden; }"}},
        {{"api.rux", dependency}});
    CHECK(HasMessage(field, "struct field 'hidden' is private to package 'Library'"));

    const auto method = AnalyzePackages(
        {{"main.rux", "import Library::Seed;\nfunc Main() -> int { let value = Seed(); return value.Read(); }"}},
        {{"api.rux", dependency}});
    CHECK(HasMessage(method, "method 'Read' is private to package 'Library'"));

    const auto constructor = AnalyzePackages(
        {{"main.rux", "import Library::Secret;\nfunc Main() { let value = Secret(1); }"}}, {{"api.rux", dependency}});
    CHECK(HasMessage(constructor, "constructor 'Secret' is private to package 'Library'"));

    const auto associated =
        AnalyzePackages({{"main.rux", "import Library::Secret;\nfunc Main() { let value = Secret::Make(); }"}},
                        {{"api.rux", dependency}});
    CHECK_MESSAGE(HasMessage(associated, "associated function 'Make' is private to package 'Library'"),
                  Messages(associated));

    const auto operation = AnalyzePackages({{"main.rux", R"(
        import Library::Seed;
        func Main() { let left = Seed(); let right = Seed(); let sum = left + right; }
    )"}},
                                           {{"api.rux", dependency}});
    CHECK_MESSAGE(HasMessage(operation, "operator '+' cannot combine"), Messages(operation));

    const auto unionField = AnalyzePackages({{"main.rux", R"(
        import Library::Bits;
        func Main() -> uint32 { let bits = Bits { closed: 1u32 }; return bits.closed; }
    )"}},
                                            {{"api.rux", dependency}});
    CHECK(HasMessage(unionField, "union field 'closed' is private to package 'Library'"));
}

TEST_CASE("public interface dispatch may use a private concrete implementation") {
    const auto dispatched = AnalyzePackages({{"main.rux", R"(
        import Library::{ Counter, CounterView, MakeCounter };
        func Main() -> int {
            let counter = MakeCounter();
            let view: &CounterView = counter;
            return view.Read();
        }
    )"}},
                                            {{"api.rux", R"(
        pub interface CounterView { func Read() -> int; }
        pub struct Counter { pub value: int; }
        extend Counter : CounterView {
            func Read(self: &Counter) -> int { return self.value; }
        }
        pub func MakeCounter() -> Counter { return Counter { value: 5 }; }
    )"}});
    CHECK(dispatched.empty());

    const auto direct = AnalyzePackages({{"main.rux", R"(
        import Library::MakeCounter;
        func Main() -> int { let counter = MakeCounter(); return counter.Read(); }
    )"}},
                                        {{"api.rux", R"(
        pub interface CounterView { func Read() -> int; }
        pub struct Counter { pub value: int; }
        extend Counter : CounterView {
            func Read(self: &Counter) -> int { return self.value; }
        }
        pub func MakeCounter() -> Counter { return Counter { value: 5 }; }
    )"}});
    CHECK(HasMessage(direct, "method 'Read' is private to package 'Library'"));
}

TEST_CASE("generated lifecycle operations and private destructors remain compiler-accessible") {
    const auto diagnostics = AnalyzePackages({{"main.rux", R"(
        import Library::{ MakePlain, MakeTracked };
        func Main() -> int {
            let original = MakePlain();
            let copied = original;
            let tracked = MakeTracked();
            return copied.value + tracked.value;
        }
    )"}},
                                             {{"api.rux", R"(
        pub struct Plain { pub value: int; }
        pub struct Tracked { pub value: int; }
        extend Tracked { func ~Tracked(self: &var Tracked) {} }
        pub func MakePlain() -> Plain { return Plain { value: 2 }; }
        pub func MakeTracked() -> Tracked { return Tracked { value: 3 }; }
    )"}});
    CHECK(diagnostics.empty());
}

TEST_CASE("custom copy and move operations require pub but private capability prohibitions still apply") {
    const std::string dependency = R"(
        pub struct CustomCopy { pub value: int; }
        extend CustomCopy {
            func =(self: &var CustomCopy, other: &CustomCopy) { self.value = other.value; }
        }
        pub struct CustomMove { pub value: int; }
        extend CustomMove {
            func <-(self: &var CustomMove, other: CustomMove) { self.value = other.value; }
        }
        pub struct NoCopy { pub value: int; }
        extend NoCopy { func =(self: &var NoCopy, other: &NoCopy); }
        pub func CopyValue() -> CustomCopy { return CustomCopy { value: 1 }; }
        pub func MoveValue() -> CustomMove { return CustomMove { value: 2 }; }
        pub func NoCopyValue() -> NoCopy { return NoCopy { value: 3 }; }
    )";

    const auto customCopy = AnalyzePackages({{"main.rux", R"(
        import Library::CopyValue;
        func Main() { let value = CopyValue(); let copied = value; }
    )"}},
                                            {{"api.rux", dependency}});
    CHECK_MESSAGE(HasMessage(customCopy, "copy operation '=' is private to package 'Library'"), Messages(customCopy));

    const auto customMove = AnalyzePackages({{"main.rux", R"(
        import Library::MoveValue;
        func Main() { let value = MoveValue(); let moved <- value; }
    )"}},
                                            {{"api.rux", dependency}});
    CHECK_MESSAGE(HasMessage(customMove, "move operation '<-' is private to package 'Library'"), Messages(customMove));

    const auto prohibited = AnalyzePackages({{"main.rux", R"(
        import Library::NoCopyValue;
        func Main() { let value = NoCopyValue(); let copied = value; }
    )"}},
                                            {{"api.rux", dependency}});
    CHECK_MESSAGE(HasMessage(prohibited, "move-only value 'value' requires an explicit '<-'"), Messages(prohibited));
    CHECK_FALSE(HasMessage(prohibited, "private to package"));
}

TEST_CASE("effective public API closure rejects private types in every exposed position") {
    const auto diagnostics = AnalyzePackages({{"api.rux", R"(
        struct Hidden { value: int; }
        interface HiddenBound {}
        pub struct PublicStruct<T: HiddenBound> { pub item: Hidden; hidden: Hidden; }
        pub union PublicUnion { pub item: Hidden, hidden: Hidden }
        pub variant PublicEnum { Tuple(Hidden), Named { item: Hidden; } }
        pub interface PublicInterface { func Make(value: Hidden) -> Hidden; }
        pub type PublicAlias = (Hidden,);
        pub const PublicConstant = Hidden { value: 1 };
        pub func PublicFunction<T: HiddenBound>(value: Hidden) -> Hidden { return value; }
        #Link("system.dll")
        extern {
            pub func PublicExtern(value: Hidden) -> Hidden;
            pub PublicVariable: Hidden;
        }
        extend PublicStruct<int> {
            pub func Exposed(self: &PublicStruct<int>, value: Hidden) -> Hidden { return value; }
        }
    )"}});

    for (const std::string_view subject :
         {"public struct 'PublicStruct'", "public field 'PublicStruct.item'", "public union field 'PublicUnion.item'",
          "public enum variant 'PublicEnum::Tuple'", "public enum field 'PublicEnum::Named.item'",
          "public interface requirement 'PublicInterface.Make'", "public type alias 'PublicAlias'",
          "public constant 'PublicConstant'", "public function 'PublicFunction'",
          "public extern function 'PublicExtern'", "public extern variable 'PublicVariable'",
          "public method 'PublicStruct.Exposed'"}) {
        CHECK_MESSAGE(HasMessage(diagnostics, std::string(subject) + " exposes private"), subject);
    }
}

TEST_CASE("pub nested below a private module remains package-private and is exempt from API closure") {
    const auto diagnostics = AnalyzePackages({{"api.rux", R"(
        struct Hidden {}
        module Internal {
            pub func Leaky(value: Hidden) -> Hidden { return value; }
        }
        extend LatePrivate {
            pub func Reveal(self: &LatePrivate) {}
        }
        struct LatePrivate {}
        func SamePackage(value: Hidden) -> Hidden { return Internal::Leaky(value); }
    )"}});
    CHECK(diagnostics.empty());
}

TEST_CASE("lowering receives effective rather than lexical visibility") {
    auto parsed = ParseSource({"api.rux", R"(
        module Internal { pub func Capped() -> int { return 1; } }
        pub module Api { pub func Exported() -> int { return 2; } }
    )"});
    SemanticAnalyzer analyzer({&parsed->module}, {}, "Library", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const HirPackage package = AstToHirLowering(model).Generate();
    REQUIRE_EQ(package.modules.size(), 1);
    const auto capped = std::ranges::find_if(package.modules.front().funcs,
                                             [](const HirFunc &function) { return function.name == "Capped"; });
    const auto exported = std::ranges::find_if(package.modules.front().funcs,
                                               [](const HirFunc &function) { return function.name == "Exported"; });
    REQUIRE(capped != package.modules.front().funcs.end());
    REQUIRE(exported != package.modules.front().funcs.end());
    CHECK_FALSE(capped->isPublic);
    CHECK(exported->isPublic);
}

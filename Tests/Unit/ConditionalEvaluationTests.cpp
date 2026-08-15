#include "ConditionalCompilationTestSupport.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/ConditionalCompilation.h"

#include <algorithm>
#include <doctest.h>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

namespace {
constexpr auto ParseSource = ParseConditionalSource;
constexpr auto Analyze = [](Module &module, const std::string &targetSystem = "Windows") {
    return AnalyzeConditionalModule(module, targetSystem);
};
constexpr auto AnalyzeNoDeps = AnalyzeConditionalModuleWithoutDependencies;
} // namespace

TEST_CASE("the conditional evaluator returns typed results without mutating the module") {
    auto parsed = ParseSource(R"(
import Core::{ #target, #build, #compiler, #source, #config };

when #target.os == .Windows &&
    #build.profile == "Release" &&
    #config.Has("sqlite") &&
    #compiler.HasFeature("conditional-compilation") &&
    #source.fileName == "test.rux" {
    func Selected() -> int { return 1; }
} else {
    func Selected() -> int { return 0; }
}

when false && #target.HasFeature(.Imaginary) {
    func Unreachable() {}
}
)");

    CompileTimeContext context;
    context.target.os = Target::OS::Windows;
    context.profile = BuildProfile::Release;
    context.config.emplace("sqlite", "enabled");

    const std::vector<Module *> modules = {&parsed.module};
    ConditionalEvaluator evaluator(context, modules);
    evaluator.SetSourceContext(parsed.module.name, "test", "");
    evaluator.SetImports(parsed.module);

    REQUIRE(parsed.module.items.size() == 3);
    auto *selected = dynamic_cast<WhenDecl *>(parsed.module.items[1].get());
    auto *shortCircuited = dynamic_cast<WhenDecl *>(parsed.module.items[2].get());
    REQUIRE(selected != nullptr);
    REQUIRE(shortCircuited != nullptr);
    REQUIRE(selected->branches.size() == 2);
    REQUIRE(shortCircuited->branches.size() == 1);

    const auto selectedResult = evaluator.Evaluate(*selected->branches[0].condition);
    REQUIRE(selectedResult.value.has_value());
    CHECK(std::get<bool>(*selectedResult.value));
    CHECK(selectedResult.diagnostics.empty());

    const auto shortCircuitResult = evaluator.Evaluate(*shortCircuited->branches[0].condition);
    REQUIRE(shortCircuitResult.value.has_value());
    CHECK_FALSE(std::get<bool>(*shortCircuitResult.value));
    CHECK(shortCircuitResult.diagnostics.empty());

    // Evaluation is read-only: neither conditional has been selected or
    // spliced, and both still occupy their original declaration slots.
    CHECK(parsed.module.items.size() == 3);
    CHECK(parsed.module.items[1].get() == selected);
    CHECK(parsed.module.items[2].get() == shortCircuited);
    CHECK(selected->branches.size() == 2);
    CHECK(shortCircuited->branches.size() == 1);
}

TEST_CASE("compiler-initialized constants are ordinary expressions outside a when condition") {
    auto parsed = ParseSource(R"(
struct Target {
    pointerBits: uint;
}

intrinsic #target: Target;

func Do() -> int {
    let bits = #target.pointerBits;
    return 0;
}
)");
    CompileTimeContext context;
    context.target.pointer_size = 8;
    const auto model = AnalyzeNoDeps(parsed.module, std::move(context));
    REQUIRE_FALSE(model.HasErrors());

    AstToHirLowering lowering(model);
    const HirPackage package = lowering.Generate();
    REQUIRE(package.modules.size() == 1);
    REQUIRE(package.modules[0].funcs.size() == 1);
    const auto *let = dynamic_cast<const HirLetStmt *>(package.modules[0].funcs[0].body->stmts[0].get());
    REQUIRE(let != nullptr);
    const auto *literal = dynamic_cast<const HirLiteralExpr *>(let->init.get());
    REQUIRE(literal != nullptr);
    CHECK(literal->value == "64");
}

TEST_CASE("an enum shorthand is an error; the variant must be written in full") {
    auto parsed = ParseSource(R"(
enum Mode { Fast, Small }

func Do() -> Mode {
    return .Fast;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "'.Fast' must be written in full, as in 'Enum::Fast'");
}

TEST_CASE("build profiles derive coherent compile-time metadata") {
    CompileTimeContext context;
    CHECK(context.ProfileName() == "Debug");
    CHECK(context.BuildMode() == Target::BuildMode::Debug);
    CHECK(context.Optimization() == OptimizationMode::None);
    CHECK(context.DebugAssertions());
    CHECK(context.DebugInfo());

    context.profile = BuildProfile::Release;
    CHECK(context.ProfileName() == "Release");
    CHECK(context.BuildMode() == Target::BuildMode::Release);
    CHECK(context.Optimization() == OptimizationMode::Speed);
    CHECK_FALSE(context.DebugAssertions());
    CHECK_FALSE(context.DebugInfo());
}

TEST_CASE("an undeclared intrinsic name is rejected") {
    // `#line` parses as a `#`-prefixed intrinsic value, but no such intrinsic
    // exists (source facts live under `#source`), so it is caught in analysis.
    auto parsed = ParseSource(R"(
func Selected() -> int {
    return #line;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(std::ranges::any_of(model.diagnostics, [](const auto &diagnostic) {
        return diagnostic.message == "name '#line' is not defined in this scope";
    }));
}

TEST_CASE("intrinsic declares a compiler-initialized ordinary constant") {
    auto parsed = ParseSource(R"(
struct Target { pointerBits: uint; }

intrinsic #target: Target;
)");
    REQUIRE(parsed.module.items.size() == 2);
    const auto *decl = dynamic_cast<const ConstDecl *>(parsed.module.items[1].get());
    REQUIRE(decl != nullptr);
    CHECK(decl->name == "#target");
    // The type names the intrinsic, so the constant can be renamed freely.
    CHECK(decl->intrinsicName == "Target");
    CHECK(decl->value == nullptr);
}

TEST_CASE("ordinary intrinsic expressions lower to context literals") {
    auto parsed = ParseSource(R"(
struct Slice<T> { data: *T; length: uint; }

enum TargetFeature { SSE2, SSE3, SSSE3, SSE41, SSE42, AVX, AVX2, AVX512, NEON, SVE, RVV }

struct Target {
    pointerBits: uint;
    triple: Slice<char8>;
}

extend Target {
    intrinsic func HasFeature(self, feature: TargetFeature) -> bool;
}

struct Build {
    profile: Slice<char8>;
    debugAssertions: bool;
    debugInfo: bool;
    isTest: bool;
    timestamp: uint64;
    date: Slice<char8>;
    time: Slice<char8>;
}

struct SemanticVersion {
    major: uint;
    minor: uint;
    patch: uint;
}
struct Compiler { version: SemanticVersion; }
extend Compiler {
    intrinsic func HasFeature(self, feature: Slice<char8>) -> bool;
}

struct Source {
    line: uint;
    column: uint;
    fileName: Slice<char8>;
    filePath: Slice<char8>;
    function: Slice<char8>;
    module: Slice<char8>;
}

struct Config {}
extend Config {
    intrinsic func Get(self, name: Slice<char8>) -> Slice<char8>;
    intrinsic func Has(self, name: Slice<char8>) -> bool;
}

intrinsic #target: Target;
intrinsic #build: Build;
intrinsic #compiler: Compiler;
intrinsic #source: Source;
intrinsic #config: Config;

module Demo {
    func Values() {
        let line = #source.line;
        let column = #source.column;
        let fileName = #source.fileName;
        let filePath = #source.filePath;
        let function = #source.function;
        let moduleName = #source.module;
        let date = #build.date;
        let time = #build.time;
        let timestamp = #build.timestamp;
        let pointerBits = #target.pointerBits;
        let targetTriple = #target.triple;
        let feature = #target.HasFeature(TargetFeature::AVX2);
        let profile = #build.profile;
        let debugAssertions = #build.debugAssertions;
        let debugInfo = #build.debugInfo;
        let isTest = #build.isTest;
        let configValue = #config.Get("allocator");
        let hasConfig = #config.Has("allocator");
        let version = #compiler.version;
        let compilerFeature = #compiler.HasFeature("namespaced-intrinsics");
        let currentTarget = #target;
    }
}
)");
    CompileTimeContext context;
    context.target.pointer_size = 8;
    context.target.cpu_features = Target::CpuFeature::AVX2;
    context.targetTriple = "windows-x86_64";
    context.profile = BuildProfile::Release;
    context.isTest = true;
    context.config["allocator"] = "mimalloc";
    context.buildInfo = BuildInfo("1.2.3-rc.1+build.7", 0);

    const SemanticModel model = AnalyzeNoDeps(parsed.module, std::move(context));
    std::string diagnosticMessages;
    for (const Diagnostic &diagnostic : model.diagnostics) {
        diagnosticMessages += diagnostic.message + "\n";
    }
    INFO(diagnosticMessages);
    REQUIRE_FALSE(model.HasErrors());
    AstToHirLowering lowering(model);
    const HirPackage package = lowering.Generate();
    REQUIRE(package.modules.size() == 1);
    REQUIRE(package.modules[0].funcs.size() == 1);
    REQUIRE(package.modules[0].funcs[0].body.has_value());

    std::unordered_map<std::string, std::string> values;
    for (const auto &stmt : package.modules[0].funcs[0].body->stmts) {
        const auto *let = dynamic_cast<const HirLetStmt *>(stmt.get());
        REQUIRE(let != nullptr);
        if (const auto *literal = dynamic_cast<const HirLiteralExpr *>(let->init.get())) {
            values[let->name] = literal->value;
        }
        else {
            const auto *object = dynamic_cast<const HirStructInitExpr *>(let->init.get());
            REQUIRE(object != nullptr);
            if (let->name == "version") {
                CHECK(object->typeName == "SemanticVersion");
                REQUIRE(object->fields.size() == 3);
                CHECK(dynamic_cast<const HirLiteralExpr *>(object->fields[0].value.get())->value == "1");
                CHECK(dynamic_cast<const HirLiteralExpr *>(object->fields[1].value.get())->value == "2");
                CHECK(dynamic_cast<const HirLiteralExpr *>(object->fields[2].value.get())->value == "3");
            }
            else {
                CHECK(let->name == "currentTarget");
                CHECK(object->typeName == "Target");
                CHECK(object->fields.size() == 2);
            }
        }
    }
    CHECK(values["line"] != "0");
    CHECK(values["column"] != "0");
    CHECK(values["fileName"] == "test.rux");
    CHECK(values["filePath"] == "test.rux");
    CHECK(values["function"] == "Demo::Values");
    CHECK(values["moduleName"] == "test::Demo");
    CHECK(values["date"] == "1970-01-01");
    CHECK(values["time"] == "00:00:00");
    CHECK(values["timestamp"] == "0");
    CHECK(values["pointerBits"] == "64");
    CHECK(values["targetTriple"] == "windows-x86_64");
    CHECK(values["feature"] == "true");
    CHECK(values["profile"] == "Release");
    CHECK(values["debugAssertions"] == "false");
    CHECK(values["debugInfo"] == "false");
    CHECK(values["isTest"] == "true");
    CHECK(values["configValue"] == "mimalloc");
    CHECK(values["hasConfig"] == "true");
    CHECK(values["compilerFeature"] == "true");
}

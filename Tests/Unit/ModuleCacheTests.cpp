#include "CompilerDriverTestSupport.h"
#include "Driver/ModuleCache.h"
#include "Lexer/Lexer.h"
#include "Syntax/Parser/Parser.h"

using namespace Rux;
using namespace Rux::Testing::CompilerDriverTestSupport;

namespace {
ParseResult ParseSource(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return parsed;
}

std::string InterfaceOf(const std::string &source) {
    const auto parsed = ParseSource(source);
    return Driver::ModuleInterface(parsed.module, source);
}
} // namespace

TEST_CASE("a module's interface leaves out the bodies of non-generic functions, comments and layout") {
    const std::string source = R"(pub struct Point { x: int32; y: int32; }

pub func Sum(p: Point) -> int32 {
    return p.x + p.y;
}

pub func Identity<T>(value: T) -> T {
    return value;
}

extend Point {
    pub func Twice(self: &Point) -> int32 {
        return self.x * 2;
    }
}

pub module Inner {
    pub func Answer() -> int32 { return 42; }
}
)";
    const std::string interface = InterfaceOf(source);
    CHECK(interface.contains("Point"));
    CHECK(interface.contains("Twice"));
    CHECK_FALSE(interface.contains("Answer() -> int32 { return 42; }"));
    const auto replaced = [&](std::string_view from, std::string_view to) {
        std::string edited = source;
        edited.replace(edited.find(from), from.size(), to);
        return InterfaceOf(edited);
    };

    // Editing a body leaves the interface as it was, and so does a comment or a change of layout where other modules
    // hold no positions.
    CHECK(replaced("p.x + p.y", "p.y + p.x") == interface);
    CHECK(replaced("self.x * 2", "2 * self.x") == interface);
    CHECK(replaced("extend Point {", "// Doubles.\nextend   Point {") == interface);
    CHECK(replaced("pub module Inner {", "pub   module Inner\n{") == interface);
    // A signature is part of it, and so is where a generic body lies, since other modules' code holds its positions.
    CHECK(replaced("Sum(p: Point)", "Sum(q: Point)") != interface);
    CHECK(replaced("return value;", "return  value;") != interface);
    CHECK(replaced("pub func Sum", "// Adds.\npub func Sum") != interface);
}

TEST_CASE("module packs round-trip their entries and refuse damaged files") {
    const auto directory =
        TempDirectory() /
        ("rux-module-pack-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "Release-test.modules";
    Driver::CachedModule entry;
    entry.key = "key";
    entry.roots = {{Optimization::LirDeclarationKind::Function, "Main"}};
    entry.sourceSize = 123;
    entry.sourceModified = -5;
    entry.sourceDigest = "source";
    entry.interfaceDigest = "interface";
    entry.fails = true;
    entry.object = {1, 2, 3, 0, 255};
    auto &functions =
        entry.references.declarations[static_cast<std::size_t>(Optimization::LirDeclarationKind::Function)];
    functions.push_back({"Main", false, {{Optimization::LirSymbolReference::Scope::Function, "Helper"}}});
    functions.push_back({"Helper", true, {{Optimization::LirSymbolReference::Scope::Data, "Table"}}});

    Driver::ModulePack pack;
    pack.Put("A.rux", entry);
    Driver::CachedModule other;
    other.key = "other";
    pack.Put("B.rux", other);
    REQUIRE(pack.Write(path));

    const auto read = Driver::ModulePack::Read(path);
    const auto *found = read.Find("A.rux", "key");
    REQUIRE(found != nullptr);
    CHECK(found->roots == entry.roots);
    CHECK(found->fails);
    CHECK(found->object == entry.object);
    CHECK(found->references == entry.references);
    CHECK(found->sourceSize == 123);
    CHECK(found->sourceModified == -5);
    CHECK(found->sourceDigest == "source");
    CHECK(found->interfaceDigest == "interface");
    CHECK(read.FindAny("A.rux") == found);
    CHECK(read.Find("A.rux", "stale") == nullptr);
    CHECK(read.Find("B.rux", "other") != nullptr);
    CHECK(read.Find("C.rux", "key") == nullptr);

    // A pack cut short reads as empty rather than as a partial set of entries.
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 3);
    CHECK(Driver::ModulePack::Read(path).Find("A.rux", "key") == nullptr);

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST_CASE("compiler driver reuses unchanged modules and links the same artifact as a full build") {
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;
    for (const BuildProfile profile : {BuildProfile::Debug, BuildProfile::Release}) {
        CAPTURE(ToString(profile));
        auto options = fixture.Options(false, diagnostics);
        options.profile = profile;
        fixture.SetApplicationSource(R"(
import Dependency::Api::Answer;

func Main() -> int {
    return Answer() - 42;
}
)");
        const auto first = CompilerDriver(options).Compile();
        REQUIRE(first.ok);

        // A body edit recompiles the edited module and reuses the dependency's.
        fixture.SetApplicationSource(R"(
import Dependency::Api::Answer;

func Main() -> int {
    return Answer() - 41;
}
)");
        const auto edited = CompilerDriver(options).Compile();
        REQUIRE(edited.ok);
        CHECK_FALSE(edited.stats.upToDate);
        CHECK(edited.stats.localReusedFiles == 0);
        CHECK(edited.stats.dependencyReusedFiles == edited.stats.dependencyFiles);
        const auto incremental = ReadBinaryFile(edited.primaryArtifactPath);

        auto uncached = options;
        uncached.useBuildCache = false;
        const auto full = CompilerDriver(uncached).Compile();
        REQUIRE(full.ok);
        CHECK(full.stats.dependencyReusedFiles == 0);
        const auto expected = ReadBinaryFile(full.primaryArtifactPath);
        CHECK(expected == incremental);
    }
    CHECK(diagnostics.empty());
}

TEST_CASE("compiler driver writes the same artifact for every job count, clean or reusing modules") {
    // Enough modules for analysis, lowering and optimization to split them across threads, each with a type, a
    // method, a generic instantiated at its own use, and a constant declared inside a body.
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;
    const auto part = [](const int index, const int step) {
        return std::format(R"(
module App::Part{0} {{
    pub struct Counter{0} {{
        pub value: uint32;
    }}

    extend Counter{0} {{
        pub func Bump(self: &var Counter{0}, by: uint32) {{
            self.value += by;
        }}
    }}

    pub func Pick{0}<T>(first: T, second: T, takeFirst: bool) -> T {{
        if takeFirst {{
            return first;
        }}
        return second;
    }}

    pub func Use{0}(x: uint32) -> uint32 {{
        const Step: uint32 = {1}u32;
        var counter = Counter{0}{{ value: x }};
        counter.Bump(Pick{0}<uint32>(Step, x, x > 3u32));
        return counter.value * Step;
    }}
}}
)",
                           index, step);
    };
    std::string main;
    std::string sum = "0u32";
    for (int index = 0; index < 6; ++index) {
        fixture.SetApplicationFile(std::format("Part{}.rux", index), part(index, index + 1));
        main += std::format("import App::Part{0}::Use{0};\n", index);
        sum += std::format(" + Use{0}({0}u32)", index);
    }
    fixture.SetApplicationSource(main + std::format(R"(
func Main() -> int {{
    let total = {};
    if total == 12345u32 {{
        return 1;
    }}
    return 0;
}}
)",
                                                    sum));
    for (const BuildProfile profile : {BuildProfile::Debug, BuildProfile::Release}) {
        CAPTURE(ToString(profile));
        std::size_t reused = 0;
        const auto build = [&](const std::size_t jobs, const bool cache) {
            auto options = fixture.Options(false, diagnostics);
            options.profile = profile;
            options.jobs = jobs;
            options.useBuildCache = cache;
            const auto result = CompilerDriver(options).Compile();
            REQUIRE(result.ok);
            reused = result.stats.localReusedFiles;
            return ReadBinaryFile(result.primaryArtifactPath);
        };
        const auto serial = build(1, false);
        CHECK(build(8, false) == serial);
        // The first cached build records every module; the next one, after a body edit, reuses all but that module.
        CHECK(build(8, true) == serial);
        fixture.SetApplicationFile("Part3.rux", part(3, 9));
        const auto edited = build(8, true);
        CHECK(reused == 6);
        CHECK(edited == build(1, false));
        CHECK(edited != serial);
        // A generic body is part of what other modules see of a module, so editing one compiles every module again.
        auto generic = part(3, 9);
        const auto takeSecond = generic.find("return second;");
        REQUIRE(takeSecond != std::string::npos);
        generic.replace(takeSecond, std::string_view("return second;").size(), "return first;");
        fixture.SetApplicationFile("Part3.rux", generic);
        const auto interfaceEdited = build(8, true);
        CHECK(reused == 0);
        CHECK(interfaceEdited == build(1, false));
        fixture.SetApplicationFile("Part3.rux", part(3, 4));
    }
    CHECK(diagnostics.empty());
}

TEST_CASE("compiler driver never keeps a module that holds the build's time") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = TempDirectory() / ("rux-build-time-test-" + std::to_string(nonce));
    const auto appRoot = root / "App";
    std::filesystem::create_directories(appRoot / "Src");
    std::error_code copyError;
    std::filesystem::create_directories(root / "Packages", copyError);
    std::filesystem::copy(std::filesystem::path(RUX_ROOT_DIR) / "Packages" / "Core", root / "Packages" / "Core",
                          std::filesystem::copy_options::recursive, copyError);
    REQUIRE_FALSE(copyError);
    const auto write = [](const std::filesystem::path &path, const std::string_view text) {
        std::ofstream output(path, std::ios::binary);
        output << text;
        REQUIRE(output.good());
    };
    write(appRoot / "Rux.toml", "[Manifest]\nVersion = 1\n\n[Package]\nName = \"App\"\nVersion = \"0.1.0\"\n"
                                "Type = \"Executable\"\n\n[Dependencies]\nCore = { Path = \"../Packages/Core\" }\n");
    write(appRoot / "Src" / "Stamp.rux", "import Core::#build;\n\nmodule App::Stamp {\n"
                                         "    pub func Seconds() -> uint64 {\n        return #build.timestamp;\n"
                                         "    }\n}\n");
    write(appRoot / "Src" / "Main.rux", "import App::Stamp::Seconds;\n\nfunc Main() -> int {\n"
                                        "    return Seconds() > 0u64 ? 0 : 1;\n}\n");
    const auto manifest = Manifest::Load(appRoot / "Rux.toml");
    REQUIRE(manifest.Ok());
    std::vector<Diagnostic> diagnostics;
    const auto build = [&](const std::int64_t timestamp, const bool cache) {
        CompileOptions options;
        options.manifestPath = appRoot / "Rux.toml";
        options.manifest = *manifest.manifest;
        options.target = Target::TargetTriple::Host();
        options.buildTimestamp = timestamp;
        options.useBuildCache = cache;
        options.emitDiagnostic = [&](const Diagnostic &diagnostic, const SourceLineLookup &) {
            diagnostics.push_back(diagnostic);
        };
        const auto result = CompilerDriver(options).Compile();
        REQUIRE(result.ok);
        return std::pair{ReadBinaryFile(result.primaryArtifactPath), result.stats.localReusedFiles};
    };
    static_cast<void>(build(1'000'000'000, true));
    // A later build at another time reuses every module but the one holding the time, and its artifact is the one a
    // full build at that time writes.
    for (const auto &entry : std::filesystem::directory_iterator(appRoot / "Temp" / "Cache"))
        if (entry.path().extension() == ".record")
            std::filesystem::remove(entry.path(), copyError);
    const auto [later, reused] = build(2'000'000'000, true);
    CHECK(reused == 1);
    CHECK(later == build(2'000'000'000, false).first);
    CHECK(diagnostics.empty());
    std::filesystem::remove_all(root, copyError);
}

TEST_CASE("dependency packages come from the shared package cache whatever the project keeps") {
    // The dependency declares an overload no checked code calls, whose name spells a parameter type only resolving
    // the skipped function records; the package is reused only when the names it was kept under are the program's.
    DependencyFixture fixture;
    fixture.SetDependencySource(R"(
pub module Api {
    pub interface Measure {
        func Size() -> int;
    }

    pub func Total(first: int) -> int {
        return first;
    }

    pub func Total(first: int, parts: Measure...) -> int {
        var sum: int = first;
        for part in parts {
            sum += part.Size();
        }
        return sum;
    }

    pub struct Point {
        pub x: int;
    }

    pub func Answer() -> int {
        let point = Point { x: 42 };
        return point.x;
    }
}
)");
    const auto cache = TempDirectory() / ("rux-package-cache-test-" +
                                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.packageCacheDirectory = cache;
    const auto appTemp = options.manifestPath.parent_path() / "Temp";
    const auto build = [&](const bool cached) {
        auto request = options;
        request.useBuildCache = cached;
        const auto result = CompilerDriver(request).Compile();
        REQUIRE(result.ok);
        return std::pair{ReadBinaryFile(result.primaryArtifactPath), result.stats.dependencyReusedFiles};
    };
    fixture.SetApplicationSource("import Dependency::Api::Answer;\n\nfunc Main() -> int {\n"
                                 "    return Answer() - 42;\n}\n");
    static_cast<void>(build(true));
    // A clean project: nothing of it is kept, but the dependency is.
    std::error_code error;
    std::filesystem::remove_all(appTemp, error);
    const auto [clean, reused] = build(true);
    CHECK(reused == 1);
    CHECK(clean == build(false).first);

    // A program that declares a type of the dependency's name gives the dependency's declarations other names, so
    // the kept package does not link into it: it is compiled again, and kept as another variant the next build
    // reuses.
    fixture.SetApplicationSource(R"(
import Dependency::Api::Answer;

struct Point {
    y: int;
}

func Main() -> int {
    let point = Point { y: 42 };
    return Answer() - point.y;
}
)");
    std::filesystem::remove_all(appTemp, error);
    const auto [renamed, renamedReused] = build(true);
    CHECK(renamedReused == 0);
    CHECK(renamed == build(false).first);
    std::filesystem::remove_all(appTemp, error);
    CHECK(build(true).second == 1);
    CHECK(diagnostics.empty());
    std::filesystem::remove_all(cache, error);
}

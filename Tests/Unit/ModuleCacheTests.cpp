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

TEST_CASE("a module's interface leaves out only the bodies of non-generic functions") {
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
    CHECK(interface.contains("pub struct Point { x: int32; y: int32; }"));
    CHECK(interface.contains("pub func Sum(p: Point) -> int32 {}"));
    CHECK(interface.contains("pub func Identity<T>(value: T) -> T {\n    return value;\n}"));
    CHECK(interface.contains("pub func Twice(self: &Point) -> int32 {}"));
    CHECK(interface.contains("pub func Answer() -> int32 {}"));
    CHECK_FALSE(interface.contains("p.x + p.y"));
    CHECK_FALSE(interface.contains("self.x * 2"));

    // Editing a body leaves the interface as it was; editing a signature or a generic body does not.
    std::string edited = source;
    edited.replace(edited.find("p.x + p.y"), 9, "p.y + p.x");
    CHECK(InterfaceOf(edited) == interface);
    std::string signature = source;
    signature.replace(signature.find("Sum(p: Point)"), 13, "Sum(q: Point)");
    CHECK(InterfaceOf(signature) != interface);
    std::string generic = source;
    generic.replace(generic.find("return value;"), 13, "return  value;");
    CHECK(InterfaceOf(generic) != interface);
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

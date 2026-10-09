// Semantic analysis and code generation lay records out by one rule. `sizeof` and `alignof` come from the semantic
// layout, and the field offsets the generated code reads and writes come from the backend's; when the two disagreed
// about a 128-bit field -- eight-byte aligned in one and sixteen in the other -- a struct allocated from its own
// `sizeof` was too small for the fields the code wrote into it.

#include "CodeGen/Layout.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Types/PrimitiveCatalog.h"

#include <doctest.h>
#include <string>
#include <unordered_set>
#include <vector>

using namespace Rux;

namespace {
struct Analyzed {
    std::vector<ResolvedTypeLayout> semantic;
    Layout::LayoutMap backend;
    LirPackage package;
};

/// Analyze and lower `source`, then lay its structs out the way each side does, in the order `names` lists them.
Analyzed Analyze(const std::string &source, const std::vector<std::string> &names) {
    Lexer lexer(source, "layout.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "layout.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    for (const SemanticDiagnostic &diagnostic : model.diagnostics) {
        INFO(diagnostic.message);
    }
    REQUIRE_FALSE(model.HasErrors());

    Analyzed result;
    for (const std::string &name : names) {
        const ResolvedTypeLayout *layout = model.TryGetLayout(TypeRef::MakeNamed(name));
        REQUIRE_MESSAGE(layout != nullptr, name);
        result.semantic.push_back(*layout);
    }

    HirToLirLowering lowering(AstToHirLowering(model).Generate(), TargetContext::CreateNative());
    result.package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    std::vector<LirStructDecl> structs;
    for (const LirModule &module : result.package.modules) {
        structs.insert(structs.end(), module.structs.begin(), module.structs.end());
    }
    Layout::BuildStructLayouts(structs, result.backend, {});
    return result;
}

const std::string kWideStructs = R"(
    struct Int128Field { a: uint8; b: int128; }
    struct Uint256Field { a: uint16; b: uint256; c: uint8; }
    struct Bool128Field { a: bool8; b: bool128; }
    struct Float80Field { a: uint32; b: float80; }
    struct Float128Field { a: uint8; b: float128; c: uint8; }
    struct Char128Field { a: char8; b: char128; }
    struct WideArrayField { a: uint8; b: int128[2]; }
    struct TupleField { a: uint8; b: (uint8, int512); }
    struct Nested { a: uint8; inner: Int128Field; c: uint32; }
    struct Narrow { a: uint8; b: uint64; }
    variant WideMaybe { None, Some(int128) }
    struct VariantField { a: uint8; b: WideMaybe; c: uint8; }
    func Touch(a: &Int128Field, b: &Uint256Field, c: &Bool128Field, d: &Float80Field, e: &Float128Field,
               f: &Char128Field, g: &WideArrayField, h: &TupleField, i: &Nested, j: &Narrow, k: &VariantField) {}
)";

const std::vector<std::string> kWideStructNames = {"Int128Field",   "Uint256Field", "Bool128Field",   "Float80Field",
                                                   "Float128Field", "Char128Field", "WideArrayField", "TupleField",
                                                   "Nested",        "Narrow",       "VariantField"};
} // namespace

TEST_CASE("every primitive takes the catalog's alignment in a semantic layout") {
    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        const TypeRef type = TypeRef::MakePrimitive(primitive.kind);
        CAPTURE(primitive.name);
        CHECK_EQ(type.AlignInBytes(), PrimitiveAlign(primitive.kind, 8));
        CHECK_EQ(Layout::AlignOf(type), static_cast<int>(PrimitiveAlign(primitive.kind, 8).value_or(0)));
    }
    // A tuple and an array take their strictest element's.
    const TypeRef tuple = TypeRef::MakeTuple({TypeRef::MakeUInt8(), TypeRef::MakePrimitive(TypeRef::Kind::Int128)});
    CHECK_EQ(tuple.SizeInBytes(), 32);
    CHECK_EQ(tuple.AlignInBytes(), 16);
    CHECK_EQ(Layout::SizeOf(tuple), 32);
    CHECK_EQ(Layout::AlignOf(tuple), 16);
    CHECK_EQ(TypeRef::MakeArray(TypeRef::MakePrimitive(TypeRef::Kind::Float80), 3).AlignInBytes(), 16);
}

TEST_CASE("a struct with a 128-bit-or-wider field has the sixteen-byte layout in semantic analysis") {
    const Analyzed analyzed = Analyze(kWideStructs, kWideStructNames);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> expected = {
        {32, 16}, {64, 16}, {32, 16}, {32, 16}, {48, 16}, {32, 16}, {48, 16}, {96, 16}, {64, 16}, {16, 8}, {64, 16}};
    REQUIRE_EQ(analyzed.semantic.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CAPTURE(kWideStructNames[index]);
        CHECK_EQ(analyzed.semantic[index].size, expected[index].first);
        CHECK_EQ(analyzed.semantic[index].alignment, expected[index].second);
    }
}

TEST_CASE("code generation lays every struct out at the size and alignment semantic analysis reports") {
    const Analyzed analyzed = Analyze(kWideStructs, kWideStructNames);
    for (std::size_t index = 0; index < kWideStructNames.size(); ++index) {
        const std::string &name = kWideStructNames[index];
        CAPTURE(name);
        const auto found = analyzed.backend.find(name);
        REQUIRE(found != analyzed.backend.end());
        CHECK_EQ(static_cast<std::uint64_t>(found->second.totalSize), analyzed.semantic[index].size);
        CHECK_EQ(static_cast<std::uint64_t>(found->second.alignment), analyzed.semantic[index].alignment);
        // Every field the generated code writes ends inside the bytes `sizeof` promises.
        for (const Layout::FieldLayout &field : found->second.fields) {
            CAPTURE(field.name);
            CHECK_LE(static_cast<std::uint64_t>(field.offset + field.size), analyzed.semantic[index].size);
        }
    }
    const auto offsetOf = [&](const std::string &type, const std::string &field) {
        return Layout::FieldOffsetOf(TypeRef::MakePointer(TypeRef::MakeNamed(type)), field, analyzed.backend, {});
    };
    CHECK_EQ(offsetOf("Int128Field", "b"), 16);
    CHECK_EQ(offsetOf("Uint256Field", "c"), 48);
    CHECK_EQ(offsetOf("Float80Field", "b"), 16);
    CHECK_EQ(offsetOf("Float128Field", "c"), 32);
    CHECK_EQ(offsetOf("Nested", "inner"), 16);
    CHECK_EQ(offsetOf("Nested", "c"), 48);
    CHECK_EQ(offsetOf("TupleField", "b"), 16);
    CHECK_EQ(offsetOf("VariantField", "b"), 16);
    CHECK_EQ(offsetOf("VariantField", "c"), 48);
    CHECK_EQ(offsetOf("Narrow", "b"), 8);
}

TEST_CASE("a variant's storage marker carries its alignment to code generation") {
    const Analyzed analyzed = Analyze(kWideStructs + R"(
        func Make() -> WideMaybe { return WideMaybe::Some(1i128); }
    )",
                                      {"VariantField"});
    for (const LirModule &module : analyzed.package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name != "Make") {
                continue;
            }
            REQUIRE_EQ(function.returnType.inner.size(), 1);
            CHECK_EQ(Layout::SizeOf(function.returnType), 32);
            CHECK_EQ(Layout::AlignOf(function.returnType), 16);
            CHECK_EQ(Layout::RuntimeAlignmentOf(TypeRef::MakeArray(function.returnType, 2), analyzed.backend, {}), 16);
            return;
        }
    }
    FAIL("missing lowered function Make");
}

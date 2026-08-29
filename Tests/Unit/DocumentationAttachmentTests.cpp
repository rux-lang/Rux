#include "Lexer/Lexer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <utility>

using namespace Rux;

namespace {
ParseResult ParseDocumentationSource(std::string source) {
    Lexer lexer(std::move(source), "documentation-attachment.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE(lexed.diagnostics.empty());

    Parser parser(std::move(lexed.tokens), "documentation-attachment.rux");
    auto parsed = parser.Parse();
    REQUIRE(parsed.diagnostics.empty());
    return parsed;
}
} // namespace

TEST_CASE("Syntax documentation exposes presence emptiness and a summary") {
    Syntax::Documentation documentation;
    CHECK(documentation.Empty());
    CHECK_FALSE(documentation.Present());
    CHECK(documentation.Summary().empty());

    documentation.markdown = "First wrapped\nsummary paragraph.\n\nDetails follow.";
    documentation.range = SourceRange{{2, 5, 10}, {4, 8, 63}};
    CHECK_FALSE(documentation.Empty());
    CHECK(documentation.Present());
    CHECK_EQ(documentation.Summary(), "First wrapped\nsummary paragraph.");

    documentation.tags.push_back(Syntax::DocumentationTag{
        Syntax::DocumentationTagKind::See, "Core::Item", "Related item.", {{3, 1, 20}, {3, 31, 50}}});
    CHECK_EQ(documentation.tags.size(), 1);

    Syntax::Documentation tagsOnly;
    tagsOnly.tags.push_back(Syntax::DocumentationTag{Syntax::DocumentationTagKind::Deprecated, {}, "Use Next.", {}});
    CHECK_FALSE(tagsOnly.Empty());
    CHECK_FALSE(tagsOnly.Present());
}

TEST_CASE("Adjacent line and block documentation attach as one source-aware value") {
    auto parsed = ParseDocumentationSource("/// First line.\n"
                                           "/**\n"
                                           " * Second paragraph.\n"
                                           " */\n"
                                           "pub func Read();\n");
    REQUIRE_EQ(parsed.module.items.size(), 1);
    const auto &documentation = parsed.module.items[0]->documentation;
    CHECK(documentation.Present());
    CHECK_FALSE(documentation.Empty());
    CHECK_EQ(documentation.markdown, "First line.\nSecond paragraph.");
    CHECK(documentation.range.start == SourceLocation{1, 1, 0});
    CHECK(documentation.range.end.line == 4);
    CHECK(documentation.range.end.column == 4);
    CHECK(documentation.tags.empty());
    CHECK(documentation.issues.empty());
    CHECK(parsed.module.documentationIssues.empty());
}

TEST_CASE("Documentation attaches through the first declaration attribute") {
    auto parsed = ParseDocumentationSource("/// Warned API.\n"
                                           "#Warn(\"Prefer Next\")\n"
                                           "#Allow(\"docs.missing\")\n"
                                           "pub func Legacy();\n"
                                           "/** Inline block. */ pub struct Inline {}\n");
    REQUIRE_EQ(parsed.module.items.size(), 2);

    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(function != nullptr);
    CHECK_EQ(function->documentation.markdown, "Warned API.");
    CHECK_EQ(function->warnMessage, "Prefer Next");
    CHECK_EQ(function->allowedLints.size(), 1);

    const auto *structure = dynamic_cast<const StructDecl *>(parsed.module.items[1].get());
    REQUIRE(structure != nullptr);
    CHECK_EQ(structure->documentation.markdown, "Inline block.");
    CHECK_EQ(structure->documentation.range.start.line, 5);
    CHECK_EQ(structure->location.line, 5);
}

TEST_CASE("Blank lines ordinary comments and trailing placement detach documentation") {
    auto parsed = ParseDocumentationSource("/// Separated by a blank line.\n"
                                           "\n"
                                           "func Blank();\n"
                                           "/// Separated by ordinary trivia.\n"
                                           "/* divider */\n"
                                           "func Ordinary();\n"
                                           "func First(); /// Trailing comment.\n"
                                           "func AfterTrailing();\n");
    REQUIRE_EQ(parsed.module.items.size(), 4);
    for (const auto &item : parsed.module.items) {
        CHECK_FALSE(item->documentation.Present());
    }

    REQUIRE_EQ(parsed.module.documentationIssues.size(), 3);
    CHECK(parsed.module.documentationIssues[0].kind == Syntax::DocumentationIssueKind::Detached);
    CHECK_EQ(parsed.module.documentationIssues[0].markdown, "Separated by a blank line.");
    CHECK(parsed.module.documentationIssues[1].kind == Syntax::DocumentationIssueKind::Detached);
    CHECK_EQ(parsed.module.documentationIssues[1].markdown, "Separated by ordinary trivia.");
    CHECK(parsed.module.documentationIssues[2].kind == Syntax::DocumentationIssueKind::Trailing);
    CHECK_EQ(parsed.module.documentationIssues[2].markdown, "Trailing comment.");
    CHECK(parsed.module.documentationIssues[2].message.contains("trailing"));
}

TEST_CASE("Only the nearest uninterrupted documentation group attaches") {
    auto parsed = ParseDocumentationSource("/// Old blank-separated group.\n"
                                           "\n"
                                           "/** Nearest A. */\n"
                                           "func A();\n"
                                           "/// Old ordinary-separated group.\n"
                                           "/* divider */\n"
                                           "/// Nearest B.\n"
                                           "func B();\n");
    REQUIRE_EQ(parsed.module.items.size(), 2);
    CHECK_EQ(parsed.module.items[0]->documentation.markdown, "Nearest A.");
    CHECK_EQ(parsed.module.items[1]->documentation.markdown, "Nearest B.");

    REQUIRE_EQ(parsed.module.documentationIssues.size(), 2);
    CHECK_EQ(parsed.module.documentationIssues[0].markdown, "Old blank-separated group.");
    CHECK_EQ(parsed.module.documentationIssues[1].markdown, "Old ordinary-separated group.");
    for (const auto &issue : parsed.module.documentationIssues) {
        CHECK(issue.kind == Syntax::DocumentationIssueKind::Detached);
        CHECK(issue.range.Length() > 0);
        CHECK_EQ(issue.message, "documentation comment is not attached to an item");
    }
}

TEST_CASE("Every declaration form stores normalized documentation") {
    auto parsed = ParseDocumentationSource("/// Function.\n"
                                           "func F();\n"
                                           "/** Structure. */\n"
                                           "struct S {}\n"
                                           "/// Enumeration.\n"
                                           "enum E { A }\n"
                                           "/// Variant.\n"
                                           "variant V { A }\n"
                                           "/// Union.\n"
                                           "union U { value: int }\n"
                                           "/// Interface.\n"
                                           "interface I { func M(); }\n"
                                           "/// Extension.\n"
                                           "extend S { func M(); }\n"
                                           "/// Module.\n"
                                           "module N {}\n"
                                           "/// Import.\n"
                                           "import Core;\n"
                                           "/// Constant.\n"
                                           "const C = 1;\n"
                                           "/// Alias.\n"
                                           "type T = int;\n"
                                           "/// External.\n"
                                           "#Link(\"system\")\n"
                                           "extern func X();\n");
    REQUIRE_EQ(parsed.module.items.size(), 12);

    static constexpr std::string_view expected[] = {
        "Function.",  "Structure.", "Enumeration.", "Variant.",  "Union.", "Interface.",
        "Extension.", "Module.",    "Import.",      "Constant.", "Alias.", "External.",
    };
    for (std::size_t index = 0; index < std::size(expected); ++index) {
        CAPTURE(index);
        CHECK(parsed.module.items[index]->documentation.Present());
        CHECK_EQ(parsed.module.items[index]->documentation.markdown, expected[index]);
    }
    CHECK(parsed.module.documentationIssues.empty());
}

TEST_CASE("Aggregate fields cases and named fields retain documentation") {
    auto parsed = ParseDocumentationSource("struct Record {\n"
                                           "    /// Structure field.\n"
                                           "    pub value: int;\n"
                                           "}\n"
                                           "union Storage {\n"
                                           "    /** Union field. */\n"
                                           "    pub bits: uint64\n"
                                           "}\n"
                                           "enum State {\n"
                                           "    /// Ready case.\n"
                                           "    Ready,\n"
                                           "    /** Failed case. */\n"
                                           "    Failed\n"
                                           "}\n"
                                           "variant Message {\n"
                                           "    /// Record case.\n"
                                           "    Record {\n"
                                           "        /** Code field. */\n"
                                           "        code: int;\n"
                                           "        /// Text field.\n"
                                           "        text: String;\n"
                                           "    }\n"
                                           "}\n");
    REQUIRE_EQ(parsed.module.items.size(), 4);

    const auto *record = dynamic_cast<const StructDecl *>(parsed.module.items[0].get());
    REQUIRE(record != nullptr);
    REQUIRE_EQ(record->fields.size(), 1);
    CHECK_EQ(record->fields[0].documentation.markdown, "Structure field.");

    const auto *storage = dynamic_cast<const UnionDecl *>(parsed.module.items[1].get());
    REQUIRE(storage != nullptr);
    REQUIRE_EQ(storage->fields.size(), 1);
    CHECK_EQ(storage->fields[0].documentation.markdown, "Union field.");

    const auto *state = dynamic_cast<const EnumDecl *>(parsed.module.items[2].get());
    REQUIRE(state != nullptr);
    REQUIRE_EQ(state->variants.size(), 2);
    CHECK_EQ(state->variants[0].documentation.markdown, "Ready case.");
    CHECK_EQ(state->variants[1].documentation.markdown, "Failed case.");

    const auto *message = dynamic_cast<const EnumDecl *>(parsed.module.items[3].get());
    REQUIRE(message != nullptr);
    REQUIRE_EQ(message->variants.size(), 1);
    CHECK_EQ(message->variants[0].documentation.markdown, "Record case.");
    REQUIRE_EQ(message->variants[0].namedFields.size(), 2);
    CHECK_EQ(message->variants[0].namedFields[0].documentation.markdown, "Code field.");
    CHECK_EQ(message->variants[0].namedFields[1].documentation.markdown, "Text field.");
    CHECK(parsed.module.documentationIssues.empty());
}

TEST_CASE("Interface extension and extern members retain documentation") {
    auto parsed = ParseDocumentationSource("interface Reader {\n"
                                           "    /// Required read.\n"
                                           "    func Read();\n"
                                           "}\n"
                                           "extend Reader {\n"
                                           "    /** Implemented read. */\n"
                                           "    #Warn(\"legacy\")\n"
                                           "    pub func Read();\n"
                                           "}\n"
                                           "#Link(\"system\")\n"
                                           "extern {\n"
                                           "    /// Opens a handle.\n"
                                           "    pub func Open(value: int) -> int;\n"
                                           "    /** Shared state. */\n"
                                           "    pub Shared: uint64;\n"
                                           "}\n");
    REQUIRE_EQ(parsed.module.items.size(), 3);

    const auto *interface = dynamic_cast<const InterfaceDecl *>(parsed.module.items[0].get());
    REQUIRE(interface != nullptr);
    REQUIRE_EQ(interface->methods.size(), 1);
    CHECK_EQ(interface->methods[0]->documentation.markdown, "Required read.");

    const auto *extension = dynamic_cast<const ImplDecl *>(parsed.module.items[1].get());
    REQUIRE(extension != nullptr);
    REQUIRE_EQ(extension->methods.size(), 1);
    CHECK_EQ(extension->methods[0]->documentation.markdown, "Implemented read.");
    CHECK_EQ(extension->methods[0]->warnMessage, "legacy");

    const auto *external = dynamic_cast<const ExternBlockDecl *>(parsed.module.items[2].get());
    REQUIRE(external != nullptr);
    REQUIRE_EQ(external->items.size(), 2);
    CHECK_EQ(external->items[0]->documentation.markdown, "Opens a handle.");
    CHECK_EQ(external->items[1]->documentation.markdown, "Shared state.");
    CHECK(parsed.module.documentationIssues.empty());
}

TEST_CASE("Nested declarations attach documentation at their own scope") {
    auto parsed = ParseDocumentationSource("/// Outer module.\n"
                                           "pub module Outer {\n"
                                           "    /// Nested function.\n"
                                           "    pub func Inside();\n"
                                           "    module Inner {\n"
                                           "        /** Deep constant. */\n"
                                           "        const Value = 1;\n"
                                           "    }\n"
                                           "}\n");
    REQUIRE_EQ(parsed.module.items.size(), 1);
    const auto *outer = dynamic_cast<const ModuleDecl *>(parsed.module.items[0].get());
    REQUIRE(outer != nullptr);
    CHECK_EQ(outer->documentation.markdown, "Outer module.");
    REQUIRE_EQ(outer->items.size(), 2);
    CHECK_EQ(outer->items[0]->documentation.markdown, "Nested function.");

    const auto *inner = dynamic_cast<const ModuleDecl *>(outer->items[1].get());
    REQUIRE(inner != nullptr);
    REQUIRE_EQ(inner->items.size(), 1);
    CHECK_EQ(inner->items[0]->documentation.markdown, "Deep constant.");
}

TEST_CASE("Documentation before closing scopes or EOF becomes a tooling issue") {
    auto parsed = ParseDocumentationSource("struct Item {\n"
                                           "    /// No following field.\n"
                                           "}\n"
                                           "module Nested {\n"
                                           "    /** No following declaration. */\n"
                                           "}\n"
                                           "/// No following top-level declaration.\n");
    REQUIRE_EQ(parsed.module.items.size(), 2);
    REQUIRE_EQ(parsed.module.documentationIssues.size(), 3);
    CHECK_EQ(parsed.module.documentationIssues[0].markdown, "No following field.");
    CHECK_EQ(parsed.module.documentationIssues[1].markdown, "No following declaration.");
    CHECK_EQ(parsed.module.documentationIssues[2].markdown, "No following top-level declaration.");
    for (const auto &issue : parsed.module.documentationIssues) {
        CHECK(issue.kind == Syntax::DocumentationIssueKind::Detached);
        CHECK(issue.range.start.line <= issue.range.end.line);
    }
}

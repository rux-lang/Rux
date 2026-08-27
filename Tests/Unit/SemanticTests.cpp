#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/PrimitiveCatalog.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <array>
#include <doctest.h>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {
std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

// Analyze `userSource` with a single dependency package `depName` whose source
// is `depSource`. The parsed modules stay alive for the whole Analyze() call.
std::vector<SemanticDiagnostic> AnalyzeWithDep(const std::string &userSource, const std::string &depName,
                                               const std::string &depSource) {
    Lexer depLexer(depSource, "dep.rux");
    auto depLexed = depLexer.Tokenize();
    REQUIRE_FALSE(depLexed.HasErrors());
    Parser depParser(std::move(depLexed.tokens), "dep.rux");
    auto depParsed = depParser.Parse();
    REQUIRE_FALSE(depParsed.HasErrors());

    Lexer lexer(userSource, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    DepPackage dep;
    dep.name = depName;
    dep.modules.push_back({depName, &depParsed.module});

    SemanticAnalyzer analyzer({&parsed.module}, {std::move(dep)}, "App", "Windows");
    return analyzer.Analyze().diagnostics;
}

template <typename Node>
const TypeRef &ResolvedType(const SemanticModel &model, const Node &node) {
    const TypeRef *type = model.TryGetType(node);
    REQUIRE(type != nullptr);
    return *type;
}

} // namespace

TEST_CASE("semantic analyzer context preserves dependency and diagnostic ordering") {
    Lexer dependencyLexer("func Dependency() { missingDependency; }", "dependency.rux");
    auto dependencyTokens = dependencyLexer.Tokenize();
    REQUIRE_FALSE(dependencyTokens.HasErrors());
    Parser dependencyParser(std::move(dependencyTokens.tokens), "dependency.rux");
    auto dependency = dependencyParser.Parse();
    REQUIRE_FALSE(dependency.HasErrors());

    Lexer userLexer("func Main() { missingUser; }", "user.rux");
    auto userTokens = userLexer.Tokenize();
    REQUIRE_FALSE(userTokens.HasErrors());
    Parser userParser(std::move(userTokens.tokens), "user.rux");
    auto user = userParser.Parse();
    REQUIRE_FALSE(user.HasErrors());

    DepPackage package;
    package.name = "Dependency";
    package.modules.push_back({"Dependency", &dependency.module});
    SemanticAnalyzer analyzer({&user.module}, {std::move(package)}, "App", "Windows");
    const SemanticModel model = analyzer.Analyze();

    REQUIRE_EQ(model.modules.size(), 2);
    CHECK(model.modules[0] == &dependency.module);
    CHECK(model.modules[1] == &user.module);
    REQUIRE_EQ(model.diagnostics.size(), 2);
    CHECK_EQ(model.diagnostics[0].sourceName, "dependency.rux");
    CHECK_EQ(model.diagnostics[0].message, "name 'missingDependency' is not defined in this scope");
    CHECK_EQ(model.diagnostics[1].sourceName, "user.rux");
    CHECK_EQ(model.diagnostics[1].message, "name 'missingUser' is not defined in this scope");
}

TEST_CASE("semantic model retains resolved AST type facts") {
    Lexer lexer(R"(
        struct Box {
            value: int32;
        }

        func Identity<T>(value: T) -> T {
            return value;
        }

        func Main() -> int64 {
            let literal: int32 = 7i32;
            let aggregate = Box { value: literal };
            let field = aggregate.value;
            let casted = field as int64;
            let called = Identity<int64>(casted);
            return match (called, true) {
                (number, _) => number
            };
        }
    )",
                "facts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "facts.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "facts", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    REQUIRE_EQ(parsed.module.items.size(), 3);
    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[2].get());
    REQUIRE(main != nullptr);
    REQUIRE(main->body != nullptr);
    REQUIRE_EQ(main->body->stmts.size(), 6);

    const auto *literal = dynamic_cast<const LetStmt *>(main->body->stmts[0].get());
    const auto *aggregate = dynamic_cast<const LetStmt *>(main->body->stmts[1].get());
    const auto *field = dynamic_cast<const LetStmt *>(main->body->stmts[2].get());
    const auto *casted = dynamic_cast<const LetStmt *>(main->body->stmts[3].get());
    const auto *called = dynamic_cast<const LetStmt *>(main->body->stmts[4].get());
    const auto *returned = dynamic_cast<const ReturnStmt *>(main->body->stmts[5].get());
    REQUIRE(literal != nullptr);
    REQUIRE(aggregate != nullptr);
    REQUIRE(field != nullptr);
    REQUIRE(casted != nullptr);
    REQUIRE(called != nullptr);
    REQUIRE(returned != nullptr);
    REQUIRE(literal->type.has_value());
    REQUIRE(returned->value.has_value());

    CHECK_EQ(ResolvedType(model, *literal->init).ToString(), "int32");
    CHECK_EQ(ResolvedType(model, **literal->type).ToString(), "int32");
    CHECK_EQ(ResolvedType(model, *aggregate->init).ToString(), "Box");
    CHECK_EQ(ResolvedType(model, *field->init).ToString(), "int32");
    CHECK_EQ(ResolvedType(model, *casted->init).ToString(), "int64");
    CHECK_EQ(ResolvedType(model, *called->init).ToString(), "int64");

    const auto *genericCall = dynamic_cast<const CallExpr *>(called->init.get());
    const auto *match = dynamic_cast<const MatchExpr *>((*returned->value).get());
    REQUIRE(genericCall != nullptr);
    REQUIRE_EQ(genericCall->typeArgs.size(), 1);
    REQUIRE(match != nullptr);
    REQUIRE_EQ(match->arms.size(), 1);
    const auto *tuplePattern = dynamic_cast<const TuplePattern *>(match->arms[0].pattern.get());
    REQUIRE(tuplePattern != nullptr);
    REQUIRE_EQ(tuplePattern->elements.size(), 2);

    CHECK_EQ(ResolvedType(model, *genericCall->typeArgs[0]).ToString(), "int64");
    CHECK_EQ(ResolvedType(model, *match->arms[0].pattern).ToString(), "(int64, bool8)");
    CHECK_EQ(ResolvedType(model, *tuplePattern->elements[0]).ToString(), "int64");

    LiteralExpr nodeOutsideAnalyzedModules;
    CHECK(model.TryGetType(nodeOutsideAnalyzedModules) == nullptr);
}

TEST_CASE("semantic model omits unresolved type facts") {
    Lexer lexer("func Main() { let value: Missing = absent; }", "unresolved.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "unresolved.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "unresolved", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE(model.HasErrors());

    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(main != nullptr);
    REQUIRE(main->body != nullptr);
    const auto *binding = dynamic_cast<const LetStmt *>(main->body->stmts[0].get());
    REQUIRE(binding != nullptr);
    REQUIRE(binding->type.has_value());
    CHECK(model.TryGetType(**binding->type) == nullptr);
    CHECK(model.TryGetType(*binding->init) == nullptr);
}

TEST_CASE("semantic model retains declared self parameter type facts") {
    Lexer lexer(R"(
        struct Number { value: int32; }
        extend Number {
            func Value(self: &Number) -> int32 { return self.value; }
        }
    )",
                "self_type.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "self_type.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "self_type", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto *implementation = dynamic_cast<const ImplDecl *>(parsed.module.items[1].get());
    REQUIRE(implementation != nullptr);
    REQUIRE_EQ(implementation->methods.size(), 1);
    REQUIRE_EQ(implementation->methods[0]->params.size(), 1);
    const TypeRef *selfType = model.TryGetType(*implementation->methods[0]->params[0].type);
    REQUIRE(selfType != nullptr);
    CHECK_EQ(selfType->ToString(), "&Number");
}

TEST_CASE("semantic model retains validated compile-time layouts and folded sizeof values") {
    Lexer lexer(R"(
        struct Slice<T> { data: *T; length: uint; }
        struct Box<T> { value: T; }
        enum Choice<T> {
            None,
            Some(T),
            Pair { left: T; right: uint8; }
        }
        union Storage {
            word: uint32,
            bytes: uint8[3]
        }

        func Main() {
            let primitive = sizeof(int32);
            let pointer = sizeof(*uint8);
            let structure = sizeof(Box<uint16>);
            let enumeration = sizeof(Choice<uint16>);
            let unionValue = sizeof(Storage);
            let tuple = sizeof((uint8, uint64));
            let array = sizeof(uint16[3]);
            let slice = sizeof(Slice<uint8>);
        }
    )",
                "layouts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "layouts.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    CompileTimeContext context;
    context.target.arch = Target::Arch::AArch64;
    SemanticAnalyzer analyzer({&parsed.module}, {}, "layouts", std::move(context));
    const SemanticModel model = analyzer.Analyze();
    for (const auto &diagnostic : model.diagnostics) {
        INFO(diagnostic.message);
    }
    REQUIRE_FALSE(model.HasErrors());

    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[4].get());
    REQUIRE(main != nullptr);
    REQUIRE(main->body != nullptr);
    REQUIRE_EQ(main->body->stmts.size(), 8);

    const std::array expected{
        ResolvedTypeLayout{4, 4}, ResolvedTypeLayout{8, 8},  ResolvedTypeLayout{2, 2}, ResolvedTypeLayout{16, 8},
        ResolvedTypeLayout{4, 4}, ResolvedTypeLayout{16, 8}, ResolvedTypeLayout{6, 2}, ResolvedTypeLayout{16, 8},
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto *binding = dynamic_cast<const LetStmt *>(main->body->stmts[i].get());
        REQUIRE(binding != nullptr);
        const auto *sizeOf = dynamic_cast<const TypeQueryExpr *>(binding->init.get());
        REQUIRE(sizeOf != nullptr);

        const std::uint64_t *value = model.TryGetTypeQueryValue(*sizeOf);
        const ResolvedTypeLayout *layout = model.TryGetLayout(*sizeOf->type);
        REQUIRE(value != nullptr);
        REQUIRE(layout != nullptr);
        CHECK_EQ(*value, expected[i].size);
        CHECK_EQ(layout->size, expected[i].size);
        CHECK_EQ(layout->alignment, expected[i].alignment);
    }

    const ResolvedTypeLayout *substituted = model.TryGetLayout(TypeRef::MakeNamed("Box<uint16>"));
    REQUIRE(substituted != nullptr);
    CHECK_EQ(substituted->size, 2);
    CHECK_EQ(substituted->alignment, 2);

    TypeQueryExpr nodeOutsideAnalyzedModules;
    CHECK(model.TryGetTypeQueryValue(nodeOutsideAnalyzedModules) == nullptr);
}

TEST_CASE("AST-to-HIR consumes required semantic type and sizeof facts") {
    Lexer lexer("func Main(value: int32) { let size = sizeof(int32); }", "lowering_facts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "lowering_facts.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(main != nullptr);
    REQUIRE_EQ(main->params.size(), 1);
    REQUIRE(main->body != nullptr);
    REQUIRE_EQ(main->body->stmts.size(), 1);
    const auto *binding = dynamic_cast<const LetStmt *>(main->body->stmts[0].get());
    REQUIRE(binding != nullptr);
    const auto *sizeOf = dynamic_cast<const TypeQueryExpr *>(binding->init.get());
    REQUIRE(sizeOf != nullptr);

    std::unordered_map<const Expr *, TypeRef> expressionTypes{{sizeOf, TypeRef::MakeUInt64()}};
    std::unordered_map<const TypeExpr *, TypeRef> typeNodeTypes{{main->params[0].type.get(), TypeRef::MakeUInt16()},
                                                                {sizeOf->type.get(), TypeRef::MakeInt32()}};
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> symbolIdentities{{main, {"Main"}}};
    std::unordered_map<std::string, ResolvedTypeLayout> typeLayouts{{"int32", {4, 4}}};
    std::unordered_map<const TypeQueryExpr *, std::uint64_t> typeQueryValues{{sizeOf, 37}};

    SemanticModel model{{},
                        {},
                        {&parsed.module},
                        CompileTimeContext{},
                        std::move(expressionTypes),
                        std::move(typeNodeTypes),
                        {},
                        {},
                        {},
                        {},
                        {},
                        {},
                        std::move(symbolIdentities),
                        {},
                        {},
                        {},
                        {},
                        std::move(typeLayouts),
                        {},
                        {},
                        std::move(typeQueryValues)};
    const HirPackage package = AstToHirLowering(model).Generate();

    REQUIRE_EQ(package.modules.size(), 1);
    REQUIRE_EQ(package.modules[0].funcs.size(), 1);
    const HirFunc &loweredMain = package.modules[0].funcs[0];
    REQUIRE_EQ(loweredMain.params.size(), 1);
    CHECK_EQ(loweredMain.params[0].type, TypeRef::MakeUInt16());
    REQUIRE(loweredMain.body.has_value());
    REQUIRE_EQ(loweredMain.body->stmts.size(), 1);
    const auto *loweredBinding = dynamic_cast<const HirLetStmt *>(loweredMain.body->stmts[0].get());
    REQUIRE(loweredBinding != nullptr);
    const auto *literal = dynamic_cast<const HirLiteralExpr *>(loweredBinding->init.get());
    REQUIRE(literal != nullptr);
    CHECK_EQ(literal->type, TypeRef::MakeUInt64());
    CHECK_EQ(literal->value, "37");
}

TEST_CASE("AST-to-HIR basic expressions consume semantic type facts") {
    Lexer lexer(R"(
        func Main() {
            let sum = 1 + 2;
            let converted = sum as int64;
        }
    )",
                "basic_lowering_facts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "basic_lowering_facts.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(main != nullptr);
    REQUIRE(main->body != nullptr);
    REQUIRE_EQ(main->body->stmts.size(), 2);
    const auto *sum = dynamic_cast<const LetStmt *>(main->body->stmts[0].get());
    const auto *converted = dynamic_cast<const LetStmt *>(main->body->stmts[1].get());
    REQUIRE(sum != nullptr);
    REQUIRE(converted != nullptr);
    const auto *binary = dynamic_cast<const BinaryExpr *>(sum->init.get());
    const auto *cast = dynamic_cast<const CastExpr *>(converted->init.get());
    REQUIRE(binary != nullptr);
    REQUIRE(cast != nullptr);

    std::unordered_map<const Expr *, TypeRef> expressionTypes{
        {binary->left.get(), TypeRef::MakeInt8()},
        {binary->right.get(), TypeRef::MakeInt16()},
        {binary, TypeRef::MakeUInt32()},
        {cast->operand.get(), TypeRef::MakeUInt64()},
        {cast, TypeRef::MakeUInt16()},
    };
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> symbolIdentities{{main, {"Main"}}};
    SemanticModel model{{},
                        {},
                        {&parsed.module},
                        CompileTimeContext{},
                        std::move(expressionTypes),
                        {},
                        {},
                        {},
                        {},
                        {},
                        {},
                        {},
                        std::move(symbolIdentities),
                        {},
                        {},
                        {},
                        {},
                        {},
                        {},
                        {},
                        {}};

    const HirPackage package = AstToHirLowering(model).Generate();

    REQUIRE_EQ(package.modules.size(), 1);
    REQUIRE_EQ(package.modules[0].funcs.size(), 1);
    REQUIRE(package.modules[0].funcs[0].body.has_value());
    const HirBlock &body = *package.modules[0].funcs[0].body;
    REQUIRE_EQ(body.stmts.size(), 2);
    const auto *loweredSum = dynamic_cast<const HirLetStmt *>(body.stmts[0].get());
    const auto *loweredConverted = dynamic_cast<const HirLetStmt *>(body.stmts[1].get());
    REQUIRE(loweredSum != nullptr);
    REQUIRE(loweredConverted != nullptr);
    const auto *loweredBinary = dynamic_cast<const HirBinaryExpr *>(loweredSum->init.get());
    const auto *loweredCast = dynamic_cast<const HirCastExpr *>(loweredConverted->init.get());
    REQUIRE(loweredBinary != nullptr);
    REQUIRE(loweredCast != nullptr);
    CHECK_EQ(loweredBinary->left->type, TypeRef::MakeInt8());
    CHECK_EQ(loweredBinary->right->type, TypeRef::MakeInt16());
    CHECK_EQ(loweredBinary->type, TypeRef::MakeUInt32());
    CHECK_EQ(loweredCast->operand->type, TypeRef::MakeUInt64());
    CHECK_EQ(loweredCast->targetType, TypeRef::MakeUInt16());
}

TEST_CASE("null pointer expressions retain contextual semantic types") {
    Lexer lexer(R"(
        func IsNull(value: *int) -> bool { return value == null; }
        func CastNull() -> *int { return null as *int; }
    )",
                "null_expression.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "null_expression.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "null_expression", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(function != nullptr);
    REQUIRE(function->body != nullptr);
    const auto *returned = dynamic_cast<const ReturnStmt *>(function->body->stmts[0].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *comparison = dynamic_cast<const BinaryExpr *>((*returned->value).get());
    REQUIRE(comparison != nullptr);
    CHECK_EQ(ResolvedType(model, *comparison), TypeRef::MakeBool());

    const HirPackage package = AstToHirLowering(model).Generate();
    REQUIRE_EQ(package.modules.size(), 1);
    REQUIRE_EQ(package.modules[0].funcs.size(), 2);
    REQUIRE(package.modules[0].funcs[0].body.has_value());
    const auto *loweredReturn = dynamic_cast<const HirReturnStmt *>(package.modules[0].funcs[0].body->stmts[0].get());
    REQUIRE(loweredReturn != nullptr);
    REQUIRE(loweredReturn->value.has_value());
    const auto *loweredComparison = dynamic_cast<const HirBinaryExpr *>((*loweredReturn->value).get());
    REQUIRE(loweredComparison != nullptr);
    CHECK_EQ(loweredComparison->type, TypeRef::MakeBool());
    CHECK_EQ(loweredComparison->right->type, TypeRef::MakePointer(TypeRef::MakeInt()));

    REQUIRE(package.modules[0].funcs[1].body.has_value());
    const auto *castReturn = dynamic_cast<const HirReturnStmt *>(package.modules[0].funcs[1].body->stmts[0].get());
    REQUIRE(castReturn != nullptr);
    REQUIRE(castReturn->value.has_value());
    const auto *loweredCast = dynamic_cast<const HirCastExpr *>((*castReturn->value).get());
    REQUIRE(loweredCast != nullptr);
    CHECK_EQ(loweredCast->operand->type, TypeRef::MakePointer(TypeRef::MakeInt()));
}

TEST_CASE("semantic model omits recursive and invalid compile-time layouts") {
    Lexer lexer(R"(
        struct Recursive { next: Recursive; }
        func Main() {
            let recursive = sizeof(Recursive);
            let unsized = sizeof(uint8[]);
            let overflow = sizeof(uint64[2305843009213693952]);
        }
    )",
                "invalid_layouts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "invalid_layouts.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "invalid_layouts", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE(model.HasErrors());
    CHECK(std::ranges::any_of(model.diagnostics, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message == "cannot determine the size of type 'Recursive'";
    }));
    CHECK(std::ranges::any_of(model.diagnostics, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message == "cannot determine the size of type 'uint64[2305843009213693952]'";
    }));

    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[1].get());
    REQUIRE(main != nullptr);
    REQUIRE(main->body != nullptr);
    REQUIRE_EQ(main->body->stmts.size(), 3);
    for (const auto &statement : main->body->stmts) {
        const auto *binding = dynamic_cast<const LetStmt *>(statement.get());
        REQUIRE(binding != nullptr);
        const auto *sizeOf = dynamic_cast<const TypeQueryExpr *>(binding->init.get());
        REQUIRE(sizeOf != nullptr);
        CHECK(model.TryGetTypeQueryValue(*sizeOf) == nullptr);
        CHECK(model.TryGetLayout(*sizeOf->type) == nullptr);
    }
    CHECK(model.TryGetLayout(TypeRef::MakeNamed("Recursive")) == nullptr);
}

TEST_CASE("semantic model retains facts for dependency modules") {
    Lexer depLexer("func DependencyValue() -> int32 { return 7i32; }", "dependency.rux");
    auto depLexed = depLexer.Tokenize();
    REQUIRE_FALSE(depLexed.HasErrors());
    Parser depParser(std::move(depLexed.tokens), "dependency.rux");
    auto depParsed = depParser.Parse();
    REQUIRE_FALSE(depParsed.HasErrors());

    Lexer userLexer("func Main() {}", "main.rux");
    auto userLexed = userLexer.Tokenize();
    REQUIRE_FALSE(userLexed.HasErrors());
    Parser userParser(std::move(userLexed.tokens), "main.rux");
    auto userParsed = userParser.Parse();
    REQUIRE_FALSE(userParsed.HasErrors());

    DepPackage dependency;
    dependency.name = "Dependency";
    dependency.modules.push_back({"Dependency", &depParsed.module});
    SemanticAnalyzer analyzer({&userParsed.module}, {std::move(dependency)}, "main", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    REQUIRE_EQ(model.modules.size(), 2);
    CHECK(model.modules[0] == &depParsed.module);

    const auto *function = dynamic_cast<const FuncDecl *>(depParsed.module.items[0].get());
    REQUIRE(function != nullptr);
    REQUIRE(function->body != nullptr);
    const auto *returned = dynamic_cast<const ReturnStmt *>(function->body->stmts[0].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    CHECK_EQ(ResolvedType(model, **returned->value).ToString(), "int32");
}

TEST_CASE("semantic model retains resolved callable bindings") {
    Lexer lexer(R"(
        func Choose(value: int32) -> int32 { return value; }
        func Choose(value: bool) -> bool { return value; }
        func Identity<T>(value: T) -> T { return value; }
        func Gather(head: int32, tail: int32...) -> int32 { return head; }

        interface Reader {
            func Read() -> int32;
        }

        struct Number { value: int32; }

        extend Number : Reader {
            func Read(self: &Number) -> int32 { return self.value; }
        }

        #Abi(.C)
        #Link("native.dll", "native_actual")
        extern func Native(value: int32, ...) -> int32;

        func Main() {
            let number = Number { value: 7i32 };
            let reader: Reader = number;
            let chosen = Choose(1i32);
            let generic = Identity<int64>(2i64);
            let gathered = Gather(1i32, 2i32, 3i32);
            let method = number.Read();
            let dispatched = reader.Read();
            let external = Native(1i32, true);
        }
    )",
                "bindings.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "bindings.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "bindings", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto *firstOverload = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    const auto *genericDecl = dynamic_cast<const FuncDecl *>(parsed.module.items[2].get());
    const auto *variadicDecl = dynamic_cast<const FuncDecl *>(parsed.module.items[3].get());
    const auto *interfaceDecl = dynamic_cast<const InterfaceDecl *>(parsed.module.items[4].get());
    const auto *implDecl = dynamic_cast<const ImplDecl *>(parsed.module.items[6].get());
    const auto *externDecl = dynamic_cast<const ExternFuncDecl *>(parsed.module.items[7].get());
    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[8].get());
    REQUIRE(firstOverload != nullptr);
    REQUIRE(genericDecl != nullptr);
    REQUIRE(variadicDecl != nullptr);
    REQUIRE(interfaceDecl != nullptr);
    REQUIRE_EQ(interfaceDecl->methods.size(), 1);
    REQUIRE(implDecl != nullptr);
    REQUIRE_EQ(implDecl->methods.size(), 1);
    REQUIRE(externDecl != nullptr);
    REQUIRE(main != nullptr);
    REQUIRE(main->body != nullptr);
    REQUIRE_EQ(main->body->stmts.size(), 8);

    const auto callAt = [&](const std::size_t statementIndex) -> const CallExpr & {
        const auto *binding = dynamic_cast<const LetStmt *>(main->body->stmts[statementIndex].get());
        REQUIRE(binding != nullptr);
        const auto *call = dynamic_cast<const CallExpr *>(binding->init.get());
        REQUIRE(call != nullptr);
        return *call;
    };
    const auto bindingFor = [&](const CallExpr &call) -> const ResolvedCallableBinding & {
        const ResolvedCallableBinding *binding = model.TryGetCallableBinding(call);
        REQUIRE(binding != nullptr);
        return *binding;
    };

    const auto &overload = bindingFor(callAt(2));
    CHECK_EQ(overload.dispatch, ResolvedCallableBinding::DispatchKind::Direct);
    CHECK(overload.selectedDeclaration == firstOverload);
    CHECK(overload.substitutions.empty());

    const auto &generic = bindingFor(callAt(3));
    CHECK(generic.selectedDeclaration == genericDecl);
    REQUIRE_EQ(generic.substitutions.size(), 1);
    CHECK_EQ(generic.substitutions.at("T").ToString(), "int64");

    const auto &variadic = bindingFor(callAt(4));
    CHECK(variadic.selectedDeclaration == variadicDecl);
    REQUIRE(variadic.variadicBoundary.has_value());
    CHECK_EQ(*variadic.variadicBoundary, 1);

    const auto &method = bindingFor(callAt(5));
    CHECK_EQ(method.dispatch, ResolvedCallableBinding::DispatchKind::Method);
    CHECK(method.selectedDeclaration == implDecl->methods[0].get());
    REQUIRE(method.receiverType.has_value());
    CHECK_EQ(method.receiverType->ToString(), "Number");

    const auto &interfaceCall = bindingFor(callAt(6));
    CHECK_EQ(interfaceCall.dispatch, ResolvedCallableBinding::DispatchKind::Interface);
    CHECK(interfaceCall.selectedDeclaration == interfaceDecl->methods[0].get());
    REQUIRE(interfaceCall.receiverType.has_value());
    CHECK_EQ(interfaceCall.receiverType->ToString(), "Reader");

    const auto &external = bindingFor(callAt(7));
    CHECK_EQ(external.dispatch, ResolvedCallableBinding::DispatchKind::Direct);
    CHECK(external.selectedDeclaration == externDecl);
    CHECK_EQ(external.callingConvention, CallingConvention::C);
    CHECK_EQ(external.importedSymbolOverride, "native_actual");
    REQUIRE(external.variadicBoundary.has_value());
    CHECK_EQ(*external.variadicBoundary, 1);

    const HirPackage package = AstToHirLowering(model).Generate();
    REQUIRE_EQ(package.modules.size(), 1);
    const auto loweredMain =
        std::ranges::find_if(package.modules[0].funcs, [](const HirFunc &function) { return function.name == "Main"; });
    REQUIRE(loweredMain != package.modules[0].funcs.end());
    REQUIRE(loweredMain->body.has_value());

    const auto loweredInitializerAt = [&](const std::size_t statementIndex) -> const HirExpr & {
        const auto *statement = dynamic_cast<const HirLetStmt *>(loweredMain->body->stmts[statementIndex].get());
        REQUIRE(statement != nullptr);
        return *statement->init;
    };
    const auto loweredCalleeAt = [&](const std::size_t statementIndex) -> const HirVarExpr & {
        const auto *call = dynamic_cast<const HirCallExpr *>(&loweredInitializerAt(statementIndex));
        REQUIRE(call != nullptr);
        const auto *callee = dynamic_cast<const HirVarExpr *>(call->callee.get());
        REQUIRE(callee != nullptr);
        return *callee;
    };

    CHECK_EQ(loweredCalleeAt(2).name, "Choose__int32");
    CHECK_EQ(loweredCalleeAt(3).name, "Identity_int64");
    CHECK_EQ(loweredCalleeAt(4).name, "Gather");
    CHECK_EQ(loweredCalleeAt(5).name, "Number::Read");
    CHECK_EQ(loweredCalleeAt(7).name, "Native");
    const auto *interfaceDispatch = dynamic_cast<const HirInterfaceCallExpr *>(&loweredInitializerAt(6));
    REQUIRE(interfaceDispatch != nullptr);
    CHECK_EQ(interfaceDispatch->methodIdx, 0);

    const auto *variadicCall = dynamic_cast<const HirCallExpr *>(&loweredInitializerAt(4));
    REQUIRE(variadicCall != nullptr);
    REQUIRE_EQ(variadicCall->args.size(), 2);
    const auto *packedArguments = dynamic_cast<const HirArrayExpr *>(variadicCall->args[1].get());
    REQUIRE(packedArguments != nullptr);
    CHECK_EQ(packedArguments->elements.size(), 2);
    CHECK(std::ranges::any_of(package.modules[0].funcs,
                              [](const HirFunc &function) { return function.name == "Identity_int64"; }));
}

TEST_CASE("semantic model omits bindings for rejected calls") {
    Lexer lexer(R"(
        func Select(value: int32) -> int32 { return value; }
        func Select(value: bool) -> bool { return value; }
        func Main() { let rejected = Select("wrong"); }
    )",
                "rejected_call.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "rejected_call.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "rejected_call", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE(model.HasErrors());
    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[2].get());
    REQUIRE(main != nullptr);
    REQUIRE(main->body != nullptr);
    const auto *binding = dynamic_cast<const LetStmt *>(main->body->stmts[0].get());
    REQUIRE(binding != nullptr);
    const auto *call = dynamic_cast<const CallExpr *>(binding->init.get());
    REQUIRE(call != nullptr);
    CHECK(model.TryGetCallableBinding(*call) == nullptr);
}

TEST_CASE("semantic model records final linker symbol identities") {
    Lexer lexer(R"(
        module Alpha {
            func Hidden(value: int32) -> int32 { return value; }
            func Pick(value: int32) -> int32 { return value; }
            func Pick(value: bool) -> bool { return value; }
        }
        module Beta {
            func Hidden(value: int32) -> int32 { return value; }
        }

        func Identity<T>(value: T) -> T { return value; }

        struct Number { value: int32; }
        extend Number {
            func Convert(self: &Number, value: int32) -> int32 { return value; }
            func Convert(self: &Number, value: bool) -> bool { return value; }
        }

        struct Box<T> { value: T; }
        extend Box<T> {
            func Get(self: &Box<T>) -> T { return self.value; }
        }

        interface Reader {
            func Read() -> int32;
        }
        struct File { value: int32; }
        extend File : Reader {
            func Read(self: &File) -> int32 { return self.value; }
        }

        #Abi(.C)
        #Link("native.dll", "native_actual")
        extern func Native(value: int32) -> int32;

        func Main() {
            let number = Number { value: 1i32 };
            let box = Box<int64> { value: 2i64 };
            let generic = Identity<int64>(2i64);
            let converted = number.Convert(3i32);
            let item = box.Get();
            let external = Native(4i32);
        }
    )",
                "symbols.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "symbols.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "symbols", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto *alpha = dynamic_cast<const ModuleDecl *>(parsed.module.items[0].get());
    const auto *beta = dynamic_cast<const ModuleDecl *>(parsed.module.items[1].get());
    const auto *numberImpl = dynamic_cast<const ImplDecl *>(parsed.module.items[4].get());
    const auto *boxImpl = dynamic_cast<const ImplDecl *>(parsed.module.items[6].get());
    const auto *fileImpl = dynamic_cast<const ImplDecl *>(parsed.module.items[9].get());
    const auto *external = dynamic_cast<const ExternFuncDecl *>(parsed.module.items[10].get());
    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[11].get());
    REQUIRE(alpha != nullptr);
    REQUIRE(beta != nullptr);
    REQUIRE(numberImpl != nullptr);
    REQUIRE(boxImpl != nullptr);
    REQUIRE(fileImpl != nullptr);
    REQUIRE(external != nullptr);
    REQUIRE(main != nullptr);
    REQUIRE(main->body != nullptr);

    const auto symbolName = [&](const Decl &declaration) -> const std::string & {
        const ResolvedSymbolIdentity *identity = model.TryGetSymbolIdentity(declaration);
        REQUIRE(identity != nullptr);
        return identity->linkerName;
    };
    CHECK_EQ(symbolName(*alpha->items[0]), "symbols::Alpha::Hidden");
    CHECK_EQ(symbolName(*alpha->items[1]), "Pick__int32");
    CHECK_EQ(symbolName(*alpha->items[2]), "Pick__bool8");
    CHECK_EQ(symbolName(*beta->items[0]), "symbols::Beta::Hidden");
    CHECK_EQ(symbolName(*numberImpl->methods[0]), "Number::Convert__int32");
    CHECK_EQ(symbolName(*numberImpl->methods[1]), "Number::Convert__bool8");
    CHECK_EQ(symbolName(*fileImpl->methods[0]), "File::Read");
    CHECK_EQ(symbolName(*external), "native_actual");
    CHECK(model.TryGetSymbolIdentity(*boxImpl->methods[0]) == nullptr);

    const ResolvedVtableIdentity *vtable = model.TryGetVtableIdentity(*fileImpl);
    REQUIRE(vtable != nullptr);
    CHECK_EQ(vtable->linkerName, "__vtable__File__Reader");
    REQUIRE_EQ(vtable->entries.size(), 1);
    CHECK_EQ(vtable->entries[0], "File::Read");
    CHECK(model.TryGetVtableIdentity(*numberImpl) == nullptr);

    const auto callAt = [&](const std::size_t statementIndex) -> const ResolvedCallableBinding & {
        const auto *binding = dynamic_cast<const LetStmt *>(main->body->stmts[statementIndex].get());
        REQUIRE(binding != nullptr);
        const auto *call = dynamic_cast<const CallExpr *>(binding->init.get());
        REQUIRE(call != nullptr);
        const ResolvedCallableBinding *resolved = model.TryGetCallableBinding(*call);
        REQUIRE(resolved != nullptr);
        return *resolved;
    };
    CHECK_EQ(callAt(2).linkerName, "Identity_int64");
    CHECK_EQ(callAt(3).linkerName, "Number::Convert__int32");
    CHECK_EQ(callAt(4).linkerName, "Box::Get_int64");
    CHECK_EQ(callAt(5).linkerName, "native_actual");

    const HirPackage package = AstToHirLowering(model).Generate();
    REQUIRE_EQ(package.modules.size(), 1);
    const auto hasFunction = [&](const std::string_view name) {
        return std::ranges::any_of(package.modules[0].funcs,
                                   [&](const HirFunc &function) { return function.name == name; });
    };
    CHECK(hasFunction("symbols::Alpha::Hidden"));
    CHECK(hasFunction("Pick__int32"));
    CHECK(hasFunction("Pick__bool8"));
    CHECK(hasFunction("symbols::Beta::Hidden"));
    CHECK(hasFunction("Box::Get_int64"));

    const auto loweredMain =
        std::ranges::find_if(package.modules[0].funcs, [](const HirFunc &function) { return function.name == "Main"; });
    REQUIRE(loweredMain != package.modules[0].funcs.end());
    REQUIRE(loweredMain->body.has_value());
    const auto calleeAt = [&](const std::size_t statementIndex) -> const HirVarExpr & {
        const auto *statement = dynamic_cast<const HirLetStmt *>(loweredMain->body->stmts[statementIndex].get());
        REQUIRE(statement != nullptr);
        const auto *call = dynamic_cast<const HirCallExpr *>(statement->init.get());
        REQUIRE(call != nullptr);
        const auto *callee = dynamic_cast<const HirVarExpr *>(call->callee.get());
        REQUIRE(callee != nullptr);
        return *callee;
    };
    CHECK_EQ(calleeAt(2).name, "Identity_int64");
    CHECK_EQ(calleeAt(3).name, "Number::Convert__int32");
    CHECK_EQ(calleeAt(4).name, "Box::Get_int64");
    CHECK_EQ(calleeAt(5).name, "Native");
    REQUIRE_EQ(package.modules[0].externFuncs.size(), 1);
    CHECK_EQ(package.modules[0].externFuncs[0].symbolName, "native_actual");

    FuncDecl nodeOutsideAnalyzedModules;
    CHECK(model.TryGetSymbolIdentity(nodeOutsideAnalyzedModules) == nullptr);
}

TEST_CASE("AST-to-HIR instantiates symbolic method bindings for each generic receiver") {
    Lexer lexer(R"(
        struct Box<T> { value: T; }
        extend Box<T> {
            func Read(self: &Box<T>) -> T { return self.value; }
            func Forward(self: &Box<T>) { self.Read(); }
        }

        func Main() -> int64 {
            let box = Box<int64> { value: 7i64 };
            box.Forward();
            return 0i64;
        }
    )",
                "generic_binding.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "generic_binding.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "generic_binding", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto *implementation = dynamic_cast<const ImplDecl *>(parsed.module.items[1].get());
    REQUIRE(implementation != nullptr);
    REQUIRE_EQ(implementation->methods.size(), 2);
    REQUIRE(implementation->methods[1]->body != nullptr);
    const auto *statement = dynamic_cast<const ExprStmt *>(implementation->methods[1]->body->stmts[0].get());
    REQUIRE(statement != nullptr);
    const auto *symbolicCall = dynamic_cast<const CallExpr *>(statement->expr.get());
    REQUIRE(symbolicCall != nullptr);
    const ResolvedCallableBinding *symbolicBinding = model.TryGetCallableBinding(*symbolicCall);
    REQUIRE(symbolicBinding != nullptr);
    CHECK_EQ(symbolicBinding->linkerName, "Box::Read_T");
    CHECK_EQ(symbolicBinding->linkerNameBase, "Box::Read");

    const HirPackage package = AstToHirLowering(model).Generate();
    REQUIRE_EQ(package.modules.size(), 1);
    const auto read = std::ranges::find_if(package.modules[0].funcs,
                                           [](const HirFunc &function) { return function.name == "Box::Read_int64"; });
    const auto forward = std::ranges::find_if(
        package.modules[0].funcs, [](const HirFunc &function) { return function.name == "Box::Forward_int64"; });
    REQUIRE(read != package.modules[0].funcs.end());
    REQUIRE(forward != package.modules[0].funcs.end());
    REQUIRE(forward->body.has_value());
    const auto *loweredStatement = dynamic_cast<const HirExprStmt *>(forward->body->stmts[0].get());
    REQUIRE(loweredStatement != nullptr);
    const auto *loweredCall = dynamic_cast<const HirCallExpr *>(loweredStatement->expr.get());
    REQUIRE(loweredCall != nullptr);
    const auto *callee = dynamic_cast<const HirVarExpr *>(loweredCall->callee.get());
    REQUIRE(callee != nullptr);
    CHECK_EQ(callee->name, "Box::Read_int64");
}

TEST_CASE("let and var independently control binding and pointee mutability") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let immutable = 10;
            var mutable = 20;

            let readOnly: *int = @immutable;
            let writable: *var int = @mutable;
            let weakened: *int = @mutable;

            immutable = 11;
            *readOnly = 12;
            *writable = 21;
            mutable = 22;

            let bad: *var int = @immutable;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    CHECK_EQ(diagnostics[0].message, "cannot modify immutable variable 'immutable'");
    CHECK_EQ(diagnostics[1].message, "cannot modify data through read-only pointer '*int'");
    CHECK_EQ(diagnostics[2].message, "cannot assign '*int' to '*var int': '@immutable' yields a read-only '*T'; "
                                     "declare 'immutable' with 'var' for a '*var T'");
}

TEST_CASE("function parameters are immutable") {
    const auto diagnostics = AnalyzeSource(R"(
        func Immutable(x: int, ptr: *var int) {
            x = 1;
            ptr = ptr;
            *ptr = 2;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "cannot modify immutable variable 'x'");
    CHECK_EQ(diagnostics[1].message, "cannot modify immutable variable 'ptr'");
}

TEST_CASE("pointer binding mutability is independent of pointee mutability") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let a = 10;
            var b = 20;

            let immutableReadOnly: *int = @a;
            let immutableWritable: *var int = @b;
            var mutableReadOnly: *int = @a;
            var mutableWritable: *var int = @b;

            immutableReadOnly = mutableReadOnly;
            immutableWritable = mutableWritable;
            mutableReadOnly = immutableReadOnly;
            mutableWritable = immutableWritable;

            *immutableWritable = 21;
            *mutableWritable = 22;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "cannot modify immutable variable 'immutableReadOnly'");
    CHECK_EQ(diagnostics[1].message, "cannot modify immutable variable 'immutableWritable'");
}

TEST_CASE("byte is a canonical alias of uint8") {
    const auto diagnostics = AnalyzeSource(R"(
        func Read(value: uint8) -> byte {
            return value;
        }

        func Main() {
            let raw: byte = 255u8;
            let numeric: uint8 = raw;
            var storage: byte[2] = [raw, numeric];
            let ptr: *var byte = @storage[0];
            *ptr = Read(1u8);
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("extern function call attributes emit direct and qualified diagnostics") {
    const auto diagnostics = AnalyzeSource(R"(
        #Error("direct extern call is forbidden")
        #Link("Kernel32.dll")
        extern func Beep(freq: uint32, duration: uint32) -> bool32;

        module Native {
            #Warn("qualified extern call is discouraged")
            #Link("Kernel32.dll")
            extern func Sleep(milliseconds: uint32);
        }

        func Main() -> int {
            Beep(1000u32, 500u32);
            Native::Sleep(1u32);
            return 0;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].severity, Diagnostic::Severity::Error);
    CHECK_EQ(diagnostics[0].message, "direct extern call is forbidden");
    CHECK_EQ(diagnostics[1].severity, Diagnostic::Severity::Warning);
    CHECK_EQ(diagnostics[1].message, "qualified extern call is discouraged");
}

TEST_CASE("one-argument Link applies a library to every function in an extern block") {
    const auto diagnostics = AnalyzeSource(R"(
        #Link("Kernel32.dll")
        extern {
            func Beep(freq: uint32, duration: uint32) -> bool32;
            func Sleep(milliseconds: uint32);
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("duplicate free-function signatures are rejected") {
    const auto diagnostics = AnalyzeSource(R"(
        func Do() {}
        func Do() {}

        func Convert(value: int) {}
        func Convert(value: uint) {}
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].severity, Diagnostic::Severity::Error);
    CHECK_EQ(diagnostics[0].location.line, 3);
    CHECK_EQ(diagnostics[0].message, "function 'Do' has the same parameter signature as an earlier overload");
}

TEST_CASE("duplicate method signatures are rejected") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Item {}

        extend Item {
            func Run(self: &Item) {}
            func Run(self: &Item) {}
            func Run(self: &Item, value: int) {}
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].severity, Diagnostic::Severity::Error);
    CHECK_EQ(diagnostics[0].location.line, 6);
    CHECK_EQ(diagnostics[0].message, "function 'Run' has the same parameter signature as an earlier overload");
}

TEST_CASE("same function signature in distinct modules remains valid") {
    const auto diagnostics = AnalyzeSource(R"(
        module First {
            func Do() {}
        }
        module Second {
            func Do() {}
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("documented primitive names report when their implementation is unavailable") {
    // Taken from the catalog rather than repeated, so implementing a width moves it out of this test by itself.
    std::vector<std::string_view> types;
    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        if (!primitive.implemented) {
            types.push_back(primitive.name);
        }
    }
    REQUIRE_FALSE(types.empty());

    std::string source;
    for (std::size_t i = 0; i < types.size(); ++i) {
        source += "struct Holder" + std::to_string(i) + " { value: " + std::string(types[i]) + "; }\n";
    }

    const auto diagnostics = AnalyzeSource(source);
    REQUIRE_EQ(diagnostics.size(), types.size());
    for (std::size_t i = 0; i < types.size(); ++i) {
        CAPTURE(types[i]);
        CHECK_EQ(diagnostics[i].severity, Diagnostic::Severity::Error);
        CHECK_EQ(diagnostics[i].message, "primitive type '" + std::string(types[i]) +
                                             "' is reserved but is not implemented in this compiler version");
    }
}

TEST_CASE("unimplemented primitive names cannot be declared as user types") {
    const auto diagnostics = AnalyzeSource("struct int128 {}");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "type 'int128' is already declared in this scope");
}

TEST_CASE("ordinary unknown types keep the unknown-type diagnostic") {
    const auto diagnostics = AnalyzeSource("struct Holder { value: CustomInteger; }");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "type 'CustomInteger' is not defined in this scope");
}

TEST_CASE("a flexible array is accepted only as the final struct field") {
    CHECK(AnalyzeSource(R"(
        struct Packet {
            length: uint;
            data: uint8[];
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource(R"(
        struct NotTail {
            data: uint8[];
            length: uint;
        }
        union NotStruct { data: uint8[] }
        func Invalid(value: uint8[]) -> uint8[] {
            var local: uint8[];
            return value;
        }
    )");

    CHECK_EQ(std::ranges::count_if(diagnostics,
                                   [](const SemanticDiagnostic &diagnostic) {
                                       return diagnostic.message ==
                                              "flexible array type is only allowed as the final field of a struct";
                                   }),
             5);
}

TEST_CASE("fixed arrays require matching literal extents") {
    CHECK(AnalyzeSource(R"(
        const Bytes: uint8[3] = [1u8, 2u8, 3u8];
        func Main() {
            var values: uint16[2] = [10u16, 20u16];
            values[1] = 30u16;
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource("const Bytes: uint8[2] = [1u8, 2u8, 3u8];");
    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK(diagnostics.front().message.find("cannot assign") != std::string::npos);
}

TEST_CASE("contextual enum patterns infer generic subject types") {
    const auto diagnostics = AnalyzeSource(R"(
        enum Result<T, E> {
            Success(T),
            Error(E)
        }

        enum ParseError {
            Invalid
        }

        func Unwrap(result: Result<float64, ParseError>) -> float64 {
            return match result {
                .Success(value) => value,
                .Error(_) => 0.0
            };
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("generic arithmetic is checked after type substitution") {
    CHECK(AnalyzeSource(R"(
        func Div<T>(x: T, y: T) -> T {
            return x / y;
        }

        func Forward<T>(x: T, y: T) -> T {
            return Div<T>(x, y);
        }

        func Main() {
            let quotient = Forward<float>(10.0, 2.0);
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource(R"(
        func Div<T>(x: T, y: T) -> T {
            return x / y;
        }

        func Forward<T>(x: T, y: T) -> T {
            return Div<T>(x, y);
        }

        func Main() {
            let quotient = Forward<bool>(true, false);
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message,
             "operator '/' cannot combine left operand 'bool8' with right operand 'bool8'");
}

TEST_CASE("contextual enum patterns diagnose unknown variants") {
    const auto diagnostics = AnalyzeSource(R"(
        enum Option {
            Some(int),
            None
        }

        func Read(option: Option) -> int {
            return match option {
                .Missing => 0,
                else => 1
            };
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "enum 'Option' has no variant 'Missing'");
}

TEST_CASE("prefix operators bind more tightly than casts") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let value = 10;
            let pointer = @value;
            let address: uint = @value as uint;
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("logical right shift requires a signed integer left operand") {
    CHECK(AnalyzeSource(R"(
        func Main() {
            let value: int8 = -8;
            let shifted: int8 = value >>> 2;
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let value: uint8 = 248;
            let shifted = value >>> 2;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "operator '>>>' requires a signed integer left operand, but found 'uint8'");

    const auto compoundDiagnostics = AnalyzeSource(R"(
        func Main() {
            var value: uint8 = 248;
            value >>>= 2;
        }
    )");

    REQUIRE_EQ(compoundDiagnostics.size(), 1);
    CHECK_EQ(compoundDiagnostics.front().message,
             "operator '>>>=' requires a signed integer left operand, but found 'uint8'");
}

TEST_CASE("pointer and array type syntax preserves grouping") {
    CHECK(AnalyzeSource(R"(
        func Main() {
            let values: uint[4] = [255u, 127u, 10u, 0u];
            let pointerToArray: *(uint[4]) = @values;
            let arrayOfPointers: (*uint)[2] = [@values[0], @values[1]];
            let oneTuple: (uint,) = (1u,);
        }
    )")
              .empty());

    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let values: uint[4] = [255u, 127u, 10u, 0u];
            let wrong: (*uint)[4] = @values;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "cannot assign '*(uint[4])' to '(*uint)[4]'");
}

TEST_CASE("bare package import binds the eponymous module for qualified access") {
    const auto diagnostics = AnalyzeWithDep(R"(
        import Platform;

        func Main() -> int {
            return Platform::Now();
        }
    )",
                                            "Platform", R"(
        pub module Platform {
            pub func Now() -> int { return 7; }
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("bare package import without an eponymous module is an error") {
    const auto diagnostics = AnalyzeWithDep(R"(
        import Utils;

        func Main() -> int { return 0; }
    )",
                                            "Utils", R"(
        module Helpers {
            func Ping() -> int { return 1; }
        }
    )");

    const bool reported = std::ranges::any_of(diagnostics, [](const SemanticDiagnostic &d) {
        return d.severity == Diagnostic::Severity::Error && d.message == "import 'Utils' does not name a module" &&
               d.help.has_value() && *d.help == "import an item instead, for example 'import Utils::Name'";
    });
    CHECK(reported);
}

TEST_CASE("importing a module's item without naming the module is an error") {
    const auto diagnostics = AnalyzeWithDep(R"(
        import Foo::Bar;

        func Main() -> int { return 0; }
    )",
                                            "Foo", R"(
        pub module Foo {
            pub func Bar() -> int { return 7; }
        }
    )");

    const bool reported = std::ranges::any_of(diagnostics, [](const SemanticDiagnostic &d) {
        return d.severity == Diagnostic::Severity::Error && d.message == "name 'Bar' was not found in package 'Foo'" &&
               d.help.has_value() && *d.help == "did you mean 'import Foo::Foo::Bar'?";
    });
    CHECK(reported);
}

TEST_CASE("importing a module's item through its full path resolves") {
    const auto diagnostics = AnalyzeWithDep(R"(
        import Foo::Foo::Bar;

        func Main() -> int {
            return Bar();
        }
    )",
                                            "Foo", R"(
        pub module Foo {
            pub func Bar() -> int { return 7; }
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("AST-to-HIR uses the recorded binding for an imported function") {
    Lexer dependencyLexer(R"(
        pub module Foo {
            pub func Bar() -> int { return 7; }
        }
    )",
                          "dependency.rux");
    auto dependencyTokens = dependencyLexer.Tokenize();
    REQUIRE_FALSE(dependencyTokens.HasErrors());
    Parser dependencyParser(std::move(dependencyTokens.tokens), "dependency.rux");
    auto dependency = dependencyParser.Parse();
    REQUIRE_FALSE(dependency.HasErrors());

    Lexer userLexer(R"(
        import Foo::Foo::Bar;
        func Main() -> int { return Bar(); }
    )",
                    "main.rux");
    auto userTokens = userLexer.Tokenize();
    REQUIRE_FALSE(userTokens.HasErrors());
    Parser userParser(std::move(userTokens.tokens), "main.rux");
    auto user = userParser.Parse();
    REQUIRE_FALSE(user.HasErrors());

    DepPackage packageDependency;
    packageDependency.name = "Foo";
    packageDependency.modules.push_back({"Foo", &dependency.module});
    SemanticAnalyzer analyzer({&user.module}, {std::move(packageDependency)}, "App", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const HirPackage package = AstToHirLowering(model).Generate();
    const auto userModule =
        std::ranges::find_if(package.modules, [](const HirModule &module) { return module.name == "main.rux"; });
    REQUIRE(userModule != package.modules.end());
    REQUIRE_EQ(userModule->funcs.size(), 1);
    REQUIRE(userModule->funcs[0].body.has_value());
    const auto *returned = dynamic_cast<const HirReturnStmt *>(userModule->funcs[0].body->stmts[0].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *call = dynamic_cast<const HirCallExpr *>(returned->value->get());
    REQUIRE(call != nullptr);
    const auto *callee = dynamic_cast<const HirVarExpr *>(call->callee.get());
    REQUIRE(callee != nullptr);
    CHECK_EQ(callee->name, "Bar");
}

TEST_CASE("all six range expressions type-check for collection slicing") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let values: int[6] = [10, 20, 30, 40, 50, 60];
            let bounded = values[2..4];
            let inclusive = values[2..=4];
            let from = values[2..];
            let to = values[..3];
            let toInclusive = values[..=3];
            let full = values[..];
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("ranges without a start are not independently iterable") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            for value in ..3 {}
            for value in ..=3 {}
            for value in .. {}
        }
    )");

    CHECK_EQ(std::ranges::count_if(diagnostics,
                                   [](const SemanticDiagnostic &diagnostic) {
                                       return diagnostic.message.find("has no initial value and is not iterable") !=
                                              std::string::npos;
                                   }),
             3);
}

TEST_CASE("constant ranges reject a start greater than the end") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let values: int[3] = [10, 20, 30];
            let exclusive = values[2..0];
            let inclusive = values[2..=0];
        }
    )");

    CHECK_EQ(std::ranges::count_if(diagnostics,
                                   [](const SemanticDiagnostic &diagnostic) {
                                       return diagnostic.message == "range start cannot be greater than its end";
                                   }),
             2);
}

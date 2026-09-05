#include "SemanticTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

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

TEST_CASE("semantic model records resolved enum and variant case patterns") {
    Lexer lexer(R"(
        enum Mode { Fast, Slow }
        variant Choice<T> {
            Empty,
            Pair(T, bool),
            Named { value: T; flag: bool; }
        }
        func Inspect(choice: &Choice<int>, mode: Mode) {
            match choice {
                Choice::Empty => {},
                .Pair(value, _) => {},
                .Named { value, flag } => {}
            }
            match mode {
                Mode::Fast => {},
                .Slow => {}
            }
        }
    )",
                "case-pattern-facts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "case-pattern-facts.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "facts", "Windows");
    const SemanticModel model = analyzer.Analyze();
    for (const auto &diagnostic : model.diagnostics) {
        INFO(diagnostic.message);
    }
    REQUIRE_FALSE(model.HasErrors());
    REQUIRE_EQ(parsed.module.items.size(), 3);

    const auto *mode = dynamic_cast<const EnumDecl *>(parsed.module.items[0].get());
    const auto *choice = dynamic_cast<const EnumDecl *>(parsed.module.items[1].get());
    const auto *inspect = dynamic_cast<const FuncDecl *>(parsed.module.items[2].get());
    REQUIRE(mode != nullptr);
    REQUIRE(choice != nullptr);
    REQUIRE(inspect != nullptr);
    REQUIRE(inspect->body != nullptr);
    REQUIRE_EQ(inspect->body->stmts.size(), 2);

    const auto *choiceMatch = dynamic_cast<const MatchStmt *>(inspect->body->stmts[0].get());
    const auto *modeMatch = dynamic_cast<const MatchStmt *>(inspect->body->stmts[1].get());
    REQUIRE(choiceMatch != nullptr);
    REQUIRE(modeMatch != nullptr);
    REQUIRE_EQ(choiceMatch->arms.size(), 3);
    REQUIRE_EQ(modeMatch->arms.size(), 2);

    const auto *emptyPattern = dynamic_cast<const EnumPattern *>(choiceMatch->arms[0].pattern.get());
    const auto *pairPattern = dynamic_cast<const EnumPattern *>(choiceMatch->arms[1].pattern.get());
    const auto *namedPattern = dynamic_cast<const EnumPattern *>(choiceMatch->arms[2].pattern.get());
    const auto *fastPattern = dynamic_cast<const EnumPattern *>(modeMatch->arms[0].pattern.get());
    const auto *slowPattern = dynamic_cast<const EnumPattern *>(modeMatch->arms[1].pattern.get());
    REQUIRE(emptyPattern != nullptr);
    REQUIRE(pairPattern != nullptr);
    REQUIRE(namedPattern != nullptr);
    REQUIRE(fastPattern != nullptr);
    REQUIRE(slowPattern != nullptr);

    const ResolvedCasePattern *empty = model.TryGetCasePattern(*emptyPattern);
    const ResolvedCasePattern *pair = model.TryGetCasePattern(*pairPattern);
    const ResolvedCasePattern *named = model.TryGetCasePattern(*namedPattern);
    const ResolvedCasePattern *fast = model.TryGetCasePattern(*fastPattern);
    const ResolvedCasePattern *slow = model.TryGetCasePattern(*slowPattern);
    REQUIRE(empty != nullptr);
    REQUIRE(pair != nullptr);
    REQUIRE(named != nullptr);
    REQUIRE(fast != nullptr);
    REQUIRE(slow != nullptr);

    CHECK(empty->declaration == choice);
    CHECK(empty->selectedCase == &choice->variants[0]);
    CHECK(empty->form == EnumDecl::Form::Variant);
    CHECK_EQ(empty->subjectType.ToString(), "Choice<int>");
    REQUIRE_EQ(empty->substitutions.size(), 1);
    CHECK_EQ(empty->substitutions.at("T").ToString(), "int");

    CHECK(pair->declaration == choice);
    CHECK(pair->selectedCase == &choice->variants[1]);
    CHECK_EQ(pair->substitutions.at("T").ToString(), "int");
    CHECK(named->declaration == choice);
    CHECK(named->selectedCase == &choice->variants[2]);
    CHECK(named->form == EnumDecl::Form::Variant);

    CHECK(fast->declaration == mode);
    CHECK(fast->selectedCase == &mode->variants[0]);
    CHECK(fast->form == EnumDecl::Form::Enumeration);
    CHECK_EQ(fast->subjectType.ToString(), "Mode");
    CHECK(fast->substitutions.empty());
    CHECK(slow->declaration == mode);
    CHECK(slow->selectedCase == &mode->variants[1]);
    CHECK(slow->form == EnumDecl::Form::Enumeration);

    EnumPattern outside;
    CHECK(model.TryGetCasePattern(*emptyPattern) == empty);
    CHECK(model.TryGetCasePattern(outside) == nullptr);
}

TEST_CASE("semantic model records reusable structural variant equality plans") {
    Lexer lexer(R"(
        struct Label { value: int; }
        extend Label {
            func ==(self: &Label, other: Label) -> bool { return self.value == other.value; }
        }
        variant Inner {
            None,
            Number(int)
        }
        variant Outer<T> {
            Unit,
            Pair(T, T),
            Named { inner: Inner; label: Label; },
            Composite((int, bool), int[2]),
            Link(*Outer<T>)
        }
        func Equal(left: Outer<int>, right: Outer<int>) -> bool { return left == right; }
        func Different(left: Outer<int>, right: Outer<int>) -> bool { return left != right; }
    )",
                "variant-equality-facts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "variant-equality-facts.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "facts", "Windows");
    const SemanticModel model = analyzer.Analyze();
    for (const auto &diagnostic : model.diagnostics) {
        INFO(diagnostic.message);
    }
    REQUIRE_FALSE(model.HasErrors());

    const auto findFunction = [&](const std::string_view name) -> const FuncDecl * {
        for (const auto &declaration : parsed.module.items) {
            const auto *function = dynamic_cast<const FuncDecl *>(declaration.get());
            if (function && function->name == name) {
                return function;
            }
        }
        return nullptr;
    };
    const auto returnedBinary = [](const FuncDecl &function) -> const BinaryExpr * {
        if (!function.body || function.body->stmts.empty()) {
            return nullptr;
        }
        const auto *returned = dynamic_cast<const ReturnStmt *>(function.body->stmts.front().get());
        return returned && returned->value ? dynamic_cast<const BinaryExpr *>((*returned->value).get()) : nullptr;
    };

    const FuncDecl *equalFunction = findFunction("Equal");
    const FuncDecl *differentFunction = findFunction("Different");
    REQUIRE(equalFunction != nullptr);
    REQUIRE(differentFunction != nullptr);
    const BinaryExpr *equalExpression = returnedBinary(*equalFunction);
    const BinaryExpr *differentExpression = returnedBinary(*differentFunction);
    REQUIRE(equalExpression != nullptr);
    REQUIRE(differentExpression != nullptr);

    const ResolvedVariantEquality *equal = model.TryGetVariantEquality(*equalExpression);
    const ResolvedVariantEquality *different = model.TryGetVariantEquality(*differentExpression);
    REQUIRE(equal != nullptr);
    REQUIRE(different != nullptr);
    CHECK_EQ(equal->type.ToString(), "Outer<int>");
    CHECK_FALSE(equal->negated);
    CHECK_EQ(different->type.ToString(), "Outer<int>");
    CHECK(different->negated);

    const VariantEqualityPlan *outer = model.TryGetVariantEqualityPlan(equal->type);
    REQUIRE(outer != nullptr);
    REQUIRE(outer->declaration != nullptr);
    CHECK_EQ(outer->declaration->name, "Outer");
    REQUIRE_EQ(outer->cases.size(), 5);
    CHECK_EQ(outer->cases[0].name, "Unit");
    CHECK_EQ(outer->cases[0].discriminant, "0");
    CHECK(outer->cases[0].payloads.empty());

    REQUIRE_EQ(outer->cases[1].payloads.size(), 2);
    CHECK_EQ(outer->cases[1].payloads[0].index, 0);
    CHECK_EQ(outer->cases[1].payloads[0].type.ToString(), "int");
    CHECK(outer->cases[1].payloads[0].operation == VariantEqualityPayload::Operation::Builtin);
    CHECK_EQ(outer->cases[1].payloads[1].index, 1);
    CHECK_EQ(outer->cases[1].payloads[1].type.ToString(), "int");

    REQUIRE_EQ(outer->cases[2].payloads.size(), 2);
    CHECK_EQ(outer->cases[2].payloads[0].name, "inner");
    CHECK(outer->cases[2].payloads[0].operation == VariantEqualityPayload::Operation::Variant);
    CHECK_EQ(outer->cases[2].payloads[0].nestedVariantType, "Inner");
    CHECK_EQ(outer->cases[2].payloads[1].name, "label");
    CHECK(outer->cases[2].payloads[1].operation == VariantEqualityPayload::Operation::Custom);
    REQUIRE(outer->cases[2].payloads[1].customEquality != nullptr);
    CHECK_EQ(outer->cases[2].payloads[1].customEquality->name, "==");

    REQUIRE_EQ(outer->cases[3].payloads.size(), 2);
    CHECK(outer->cases[3].payloads[0].operation == VariantEqualityPayload::Operation::Tuple);
    CHECK_EQ(outer->cases[3].payloads[0].elements.size(), 2);
    CHECK(outer->cases[3].payloads[1].operation == VariantEqualityPayload::Operation::Array);
    CHECK_EQ(outer->cases[3].payloads[1].elements.size(), 1);

    REQUIRE_EQ(outer->cases[4].payloads.size(), 1);
    CHECK(outer->cases[4].payloads[0].operation == VariantEqualityPayload::Operation::Builtin);
    CHECK_EQ(outer->cases[4].payloads[0].type.ToString(), "*Outer<int>");

    const VariantEqualityPlan *inner = model.TryGetVariantEqualityPlan(TypeRef::MakeNamed("Inner"));
    REQUIRE(inner != nullptr);
    CHECK_EQ(inner->cases.size(), 2);
    CHECK_EQ(inner->cases[1].name, "Number");

    BinaryExpr outside;
    CHECK(model.TryGetVariantEquality(outside) == nullptr);
    CHECK(model.TryGetVariantEqualityPlan(TypeRef::MakeNamed("Missing")) == nullptr);
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
        variant Choice<T> {
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

    SemanticFacts facts;
    facts.expressionTypes = std::move(expressionTypes);
    facts.typeNodeTypes = std::move(typeNodeTypes);
    facts.symbolIdentities = std::move(symbolIdentities);
    facts.typeLayouts = std::move(typeLayouts);
    facts.typeQueryValues = std::move(typeQueryValues);
    SemanticModel model{{}, {}, {&parsed.module}, CompileTimeContext{}, std::move(facts)};
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
    SemanticFacts facts;
    facts.expressionTypes = std::move(expressionTypes);
    facts.symbolIdentities = std::move(symbolIdentities);
    SemanticModel model{{}, {}, {&parsed.module}, CompileTimeContext{}, std::move(facts)};

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

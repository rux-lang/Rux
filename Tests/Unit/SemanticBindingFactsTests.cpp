#include "SemanticTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

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

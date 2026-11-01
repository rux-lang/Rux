// The short spellings of the extended type and its fields: receivers written `&self`, `Self` inside `extend`, struct
// literal fields written as a name alone, and attributes written without empty parentheses. The parser writes each one
// out as the long form it stands for, so these cases check the tree it builds and the diagnostics where a short form
// has nothing to stand for.

#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
ParseResult ParseSource(const std::string &source) {
    Lexer lexer(source, "self-shorthand.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "self-shorthand.rux");
    return parser.Parse();
}

std::vector<SemanticDiagnostic> Analyze(const std::string &source) {
    auto parsed = ParseSource(source);
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    return analyzer.Analyze().diagnostics;
}

const Diagnostic *Find(const std::vector<Diagnostic> &diagnostics, const std::string_view message) {
    const auto found = std::ranges::find_if(diagnostics, [&](const Diagnostic &diagnostic) {
        return diagnostic.severity == Diagnostic::Severity::Error && diagnostic.message.contains(message);
    });
    return found == diagnostics.end() ? nullptr : &*found;
}

std::string Messages(const std::vector<Diagnostic> &diagnostics) {
    std::string messages;
    for (const Diagnostic &diagnostic : diagnostics) {
        messages += diagnostic.message + " | ";
    }
    return messages;
}

const ImplDecl &OnlyExtension(const ParseResult &parsed) {
    for (const auto &item : parsed.module.items) {
        if (const auto *extension = dynamic_cast<const ImplDecl *>(item.get())) {
            return *extension;
        }
    }
    FAIL("no extend block");
    throw;
}

/// The named type a receiver or return type is written as, through one reference or pointer.
const NamedTypeExpr *NamedThrough(const TypeExpr *type, bool &writable) {
    writable = false;
    if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(type)) {
        writable = reference->pointeeMut;
        type = reference->pointee.get();
    }
    else if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(type)) {
        writable = pointer->pointeeMut;
        type = pointer->pointee.get();
    }
    return dynamic_cast<const NamedTypeExpr *>(type);
}

const Stmt &FirstStatement(const FuncDecl &function) {
    REQUIRE(function.body != nullptr);
    REQUIRE_FALSE(function.body->stmts.empty());
    return *function.body->stmts.front();
}
} // namespace

TEST_CASE("each receiver shorthand names the extended type with its type arguments") {
    const auto parsed = ParseSource(R"(
        struct Vector<T> { item: T; }
        extend Vector<T> {
            func Read(&self) -> T { return self.item; }
            func Write(&var self) {}
            func Take(self) {}
            func Peek(*self) {}
            func Poke(*var self, value: T) {}
            func ~Vector(&var self) {}
            func Long(self: &Vector<T>) {}
        }
    )");
    REQUIRE_FALSE(parsed.HasErrors());
    const ImplDecl &extension = OnlyExtension(parsed);
    REQUIRE_EQ(extension.methods.size(), 7);

    struct Expected {
        std::string_view name;
        bool reference;
        bool pointer;
        bool writable;
        bool shorthand;
    };

    const Expected expected[] = {
        {"Read", true, false, false, true},  {"Write", true, false, true, true}, {"Take", false, false, false, true},
        {"Peek", false, true, false, true},  {"Poke", false, true, true, true},  {"~Vector", true, false, true, true},
        {"Long", true, false, false, false},
    };
    for (std::size_t index = 0; index < extension.methods.size(); ++index) {
        const FuncDecl &method = *extension.methods[index];
        const Expected &want = expected[index];
        CAPTURE(method.name);
        CHECK_EQ(method.name, want.name);
        const Param *receiver = method.Receiver();
        REQUIRE(receiver != nullptr);
        CHECK_EQ(receiver->shorthandReceiver, want.shorthand);
        CHECK_EQ(dynamic_cast<const ReferenceTypeExpr *>(receiver->type.get()) != nullptr, want.reference);
        CHECK_EQ(dynamic_cast<const PointerTypeExpr *>(receiver->type.get()) != nullptr, want.pointer);
        bool writable = false;
        const NamedTypeExpr *named = NamedThrough(receiver->type.get(), writable);
        REQUIRE(named != nullptr);
        CHECK_EQ(writable, want.writable);
        CHECK_EQ(named->name, "Vector");
        REQUIRE_EQ(named->typeArgs.size(), 1);
        const auto *argument = dynamic_cast<const NamedTypeExpr *>(named->typeArgs.front().get());
        REQUIRE(argument != nullptr);
        CHECK_EQ(argument->name, "T");
    }
    // The receiver's type points at the receiver, not at the header it was read from.
    const Param &read = extension.methods[0]->params.front();
    CHECK_EQ(read.type->location.line, read.location.line);
}

TEST_CASE("a receiver shorthand in an interface requirement names Self") {
    const auto parsed = ParseSource(R"(
        interface Dial {
            func Level(&self) -> int32;
            func Turn(&var self, amount: int32);
        }
    )");
    REQUIRE_FALSE(parsed.HasErrors());
    const auto *dial = dynamic_cast<const InterfaceDecl *>(parsed.module.items.front().get());
    REQUIRE(dial != nullptr);
    REQUIRE_EQ(dial->methods.size(), 2);
    bool writable = false;
    const NamedTypeExpr *level = NamedThrough(dial->methods[0]->params.front().type.get(), writable);
    REQUIRE(level != nullptr);
    CHECK_EQ(level->name, "Self");
    CHECK_FALSE(writable);
    const NamedTypeExpr *turn = NamedThrough(dial->methods[1]->params.front().type.get(), writable);
    REQUIRE(turn != nullptr);
    CHECK_EQ(turn->name, "Self");
    CHECK(writable);
}

TEST_CASE("a receiver shorthand outside extend and interfaces is refused") {
    const auto parsed = ParseSource(R"(
        func Free(&self) {}
        struct Point { x: int32; }
        extend Point {
            func Method(&self) {
                func Nested(*var self) {}
            }
        }
    )");
    CAPTURE(Messages(parsed.diagnostics));
    const Diagnostic *free =
        Find(parsed.diagnostics, "receiver '&self' is only allowed in an 'extend' block or an interface requirement");
    REQUIRE(free != nullptr);
    REQUIRE(free->help.has_value());
    CHECK(free->help->contains("'name: Type'"));
    CHECK(Find(parsed.diagnostics, "receiver '*var self' is only allowed") != nullptr);
    CHECK_EQ(
        std::ranges::count_if(parsed.diagnostics,
                              [](const Diagnostic &diagnostic) { return diagnostic.message.contains("receiver"); }),
        2);
}

TEST_CASE("Self inside extend is the extended type in signatures, literals, calls and patterns") {
    const auto parsed = ParseSource(R"(
        struct Pair<T> { first: T; second: T; }
        extend Pair<T> {
            func Pair(first: T, second: T) -> Self { return Self { first, second }; }
            func Twin(value: T) -> Self { return Self(value, value); }
            func Swapped(&self) -> Self { return Self::Of(self.second, self.first); }
            func Of(first: T, second: T) -> Self { return Self::Of(first, second); }
        }
    )");
    REQUIRE_FALSE(parsed.HasErrors());
    const ImplDecl &extension = OnlyExtension(parsed);
    REQUIRE_EQ(extension.methods.size(), 4);

    for (const auto &method : extension.methods) {
        CAPTURE(method->name);
        REQUIRE(method->returnType.has_value());
        const auto *returned = dynamic_cast<const NamedTypeExpr *>(method->returnType->get());
        REQUIRE(returned != nullptr);
        CHECK_EQ(returned->name, "Pair");
        CHECK_EQ(returned->typeArgs.size(), 1);
    }

    const auto *literalReturn = dynamic_cast<const ReturnStmt *>(&FirstStatement(*extension.methods[0]));
    REQUIRE(literalReturn != nullptr);
    REQUIRE(literalReturn->value.has_value());
    const auto *literal = dynamic_cast<const StructInitExpr *>(literalReturn->value->get());
    REQUIRE(literal != nullptr);
    CHECK_EQ(literal->typeName, "Pair");
    CHECK_EQ(literal->typeArgs.size(), 1);
    REQUIRE_EQ(literal->fields.size(), 2);
    CHECK(literal->fields[0].shorthand);
    const auto *firstValue = dynamic_cast<const IdentExpr *>(literal->fields[0].value.get());
    REQUIRE(firstValue != nullptr);
    CHECK_EQ(firstValue->name, "first");

    const auto *constructorReturn = dynamic_cast<const ReturnStmt *>(&FirstStatement(*extension.methods[1]));
    REQUIRE(constructorReturn != nullptr);
    REQUIRE(constructorReturn->value.has_value());
    const auto *constructor = dynamic_cast<const CallExpr *>(constructorReturn->value->get());
    REQUIRE(constructor != nullptr);
    const auto *constructorName = dynamic_cast<const IdentExpr *>(constructor->callee.get());
    REQUIRE(constructorName != nullptr);
    CHECK_EQ(constructorName->name, "Pair");
    CHECK_EQ(constructor->typeArgs.size(), 1);

    // A member call carries the block's type arguments first, as `Pair::Of<T>(...)` would be written.
    const auto *memberReturn = dynamic_cast<const ReturnStmt *>(&FirstStatement(*extension.methods[2]));
    REQUIRE(memberReturn != nullptr);
    REQUIRE(memberReturn->value.has_value());
    const auto *member = dynamic_cast<const CallExpr *>(memberReturn->value->get());
    REQUIRE(member != nullptr);
    const auto *path = dynamic_cast<const PathExpr *>(member->callee.get());
    REQUIRE(path != nullptr);
    CHECK_EQ(path->segments, std::vector<std::string>{"Pair", "Of"});
    CHECK_EQ(member->typeArgs.size(), 1);
}

TEST_CASE("Self outside extend and interfaces is refused") {
    const auto parsed = ParseSource(R"(
        struct Node { next: *Self; }
        func Make() -> int32 { return Self::Zero; }
        struct Point { x: int32; }
        extend Point {
            func Method() {
                func Nested() -> Self {}
            }
        }
    )");
    CAPTURE(Messages(parsed.diagnostics));
    CHECK_EQ(std::ranges::count_if(parsed.diagnostics,
                                   [](const Diagnostic &diagnostic) {
                                       return diagnostic.message ==
                                              "'Self' is only available inside an 'extend' block or an interface";
                                   }),
             3);
    CHECK_EQ(parsed.diagnostics.size(), 3);
}

TEST_CASE("Self and receiver shorthand check as the type written out") {
    const auto diagnostics = Analyze(R"(
        struct Counter { count: int32; }
        extend Counter {
            func Counter(count: int32) -> Self { return Self { count }; }
            func Bump(&var self) { self.count += 1; }
            func Read(&self) -> int32 { return self.count; }
            func Twice(&self) -> Self { return Self::Counter(self.count * 2); }
        }
        func Main() -> int {
            var counter = Counter(1);
            counter.Bump();
            let doubled = counter.Twice();
            return doubled.Read() - 4;
        }
    )");
    CHECK_MESSAGE(diagnostics.empty(), Messages(diagnostics));
}

TEST_CASE("a read-only shorthand receiver refuses writes as the long form does") {
    const auto diagnostics = Analyze(R"(
        struct Counter { count: int32; }
        extend Counter {
            func Bump(&self) { self.count += 1; }
        }
        func Main() -> int { return 0; }
    )");
    CHECK_MESSAGE(!diagnostics.empty(), Messages(diagnostics));
}

TEST_CASE("field shorthand of a move-only binding asks for the move written out") {
    const auto diagnostics = Analyze(R"(
        struct Buffer { size: int32; }
        extend Buffer {
            func =(&var self, other: &Buffer);
            func ~Buffer(&var self) {}
        }
        struct Holder { buffer: Buffer; size: int32; }
        func Hold(buffer: Buffer, size: int32) -> Holder {
            return Holder { buffer, size };
        }
        func Main() -> int { return 0; }
    )");
    CAPTURE(Messages(diagnostics));
    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics.front().message, "move-only value 'buffer' requires an explicit '<-' in field shorthand");
    REQUIRE(diagnostics.front().help.has_value());
    CHECK_EQ(*diagnostics.front().help, "write 'buffer: <-buffer'");
}

TEST_CASE("field shorthand of an unknown name reports the name") {
    const auto diagnostics = Analyze(R"(
        struct Point { x: int32; y: int32; }
        func Make(x: int32) -> Point { return Point { x, y }; }
        func Main() -> int { return 0; }
    )");
    CHECK_MESSAGE(Find(diagnostics, "name 'y' is not defined in this scope") != nullptr, Messages(diagnostics));
}

TEST_CASE("attributes without arguments are written without parentheses") {
    const auto parsed = ParseSource(R"(
        #NoReturn
        func Stop() { while true {} }

        #Unsendable
        struct Local { value: int32; }

        #NoReturn()
        func Older() { while true {} }

        func Show(#Format format: char8[..], values: int32...) {}

        #Abi
        func Missing() {}
    )");
    CAPTURE(Messages(parsed.diagnostics));
    const auto *stop = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    const auto *local = dynamic_cast<const StructDecl *>(parsed.module.items[1].get());
    const auto *older = dynamic_cast<const FuncDecl *>(parsed.module.items[2].get());
    const auto *show = dynamic_cast<const FuncDecl *>(parsed.module.items[3].get());
    REQUIRE(stop != nullptr);
    REQUIRE(local != nullptr);
    REQUIRE(older != nullptr);
    REQUIRE(show != nullptr);
    CHECK(stop->isNoReturn);
    CHECK(local->isUnsendable);
    CHECK(older->isNoReturn);
    REQUIRE_EQ(show->params.size(), 2);
    CHECK(show->params[0].isFormat);
    // An attribute that takes arguments still needs its parentheses.
    CHECK(Find(parsed.diagnostics, "expected '(' after '#Abi'") != nullptr);
    CHECK(std::ranges::all_of(parsed.diagnostics, [](const Diagnostic &diagnostic) {
        return diagnostic.location.line >= 13; // only the '#Abi' declaration reports
    }));
}

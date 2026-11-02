// The unsafe boundary: `unsafe { ... }` as a value block and as a statement, `unsafe func`, where `unsafe` is
// rejected, which operations need an unsafe context wherever they are written, what stays safe, and the warning for a
// block that needs none. The diagnostic text of each operation is pinned by the Unsafe* goldens; these cases cover the
// positions and contexts a golden would need one file each for.

#include "SemanticTestSupport.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
ParseResult ParseSource(const std::string_view source) {
    Lexer lexer(std::string(source), "unsafe.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "unsafe.rux");
    return parser.Parse();
}

ParseResult ParseClean(const std::string_view source) {
    ParseResult result = ParseSource(source);
    for (const auto &diagnostic : result.diagnostics) {
        CAPTURE(diagnostic.message);
    }
    REQUIRE_FALSE(result.HasErrors());
    return result;
}

bool HasParseError(const ParseResult &result, const std::string_view text) {
    return std::ranges::any_of(result.diagnostics,
                               [&](const ParserDiagnostic &diagnostic) { return diagnostic.message.contains(text); });
}

const FuncDecl &Function(const ParseResult &result, const std::string_view name) {
    for (const auto &item : result.module.items) {
        if (const auto *function = dynamic_cast<const FuncDecl *>(item.get()); function && function->name == name) {
            return *function;
        }
    }
    FAIL("missing function " << name);
    throw std::runtime_error("missing function");
}

std::vector<std::string> Messages(const std::vector<SemanticDiagnostic> &diagnostics,
                                  const SemanticDiagnostic::Severity severity) {
    std::vector<std::string> messages;
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.severity == severity) {
            messages.push_back(diagnostic.message);
        }
    }
    return messages;
}

std::vector<std::string> Errors(const std::string &source) {
    return Messages(AnalyzeSource(source), SemanticDiagnostic::Severity::Error);
}

std::vector<std::string> Warnings(const std::string &source) {
    return Messages(AnalyzeSource(source), SemanticDiagnostic::Severity::Warning);
}

std::size_t CountContaining(const std::vector<std::string> &messages, const std::string_view text) {
    return static_cast<std::size_t>(
        std::ranges::count_if(messages, [&](const std::string &message) { return message.contains(text); }));
}
} // namespace

TEST_SUITE("UnsafeBoundary") {
    TEST_CASE("an unsafe block is a value block in expression position and ends a statement at its brace") {
        const ParseResult result = ParseClean(R"(
func Read(pointer: *int) -> int {
    let value = unsafe { *pointer };
    unsafe {
        let again = *pointer;
    }
    unsafe { let third = *pointer; };
    return unsafe { *pointer + value };
}
)");
        const FuncDecl &read = Function(result, "Read");
        REQUIRE(read.body);
        REQUIRE(read.body->stmts.size() == 4);

        const auto *let = dynamic_cast<const LetStmt *>(read.body->stmts[0].get());
        REQUIRE(let);
        const auto *valueBlock = dynamic_cast<const BlockExpr *>(let->init.get());
        REQUIRE(valueBlock);
        CHECK(valueBlock->isUnsafe);
        REQUIRE(valueBlock->block->stmts.size() == 1);
        const auto *tail = dynamic_cast<const ExprStmt *>(valueBlock->block->stmts.front().get());
        REQUIRE(tail);
        CHECK(tail->yieldsValue);

        for (const std::size_t index : {1u, 2u}) {
            const auto *statement = dynamic_cast<const ExprStmt *>(read.body->stmts[index].get());
            REQUIRE(statement);
            const auto *block = dynamic_cast<const BlockExpr *>(statement->expr.get());
            REQUIRE(block);
            CHECK(block->isUnsafe);
            CHECK_FALSE(statement->yieldsValue);
        }
        // The ';' after a statement block is allowed and recorded, as after any expression statement.
        CHECK(dynamic_cast<const ExprStmt *>(read.body->stmts[2].get())->semicolon.has_value());
    }

    TEST_CASE("an unsafe block that ends a value block and yields is that block's value") {
        const ParseResult result = ParseClean(R"(
func Pick(flag: bool, pointer: *int) -> int {
    return if flag {
        unsafe { *pointer }
    } else {
        0
    };
}
)");
        const FuncDecl &pick = Function(result, "Pick");
        const auto *returned = dynamic_cast<const ReturnStmt *>(pick.body->stmts.front().get());
        REQUIRE(returned);
        const auto *conditional = dynamic_cast<const MatchExpr *>(returned->value->get());
        REQUIRE(conditional);
        const auto *branch = dynamic_cast<const BlockExpr *>(conditional->arms.front().body.get());
        REQUIRE(branch);
        const auto *last = dynamic_cast<const ExprStmt *>(branch->block->stmts.back().get());
        REQUIRE(last);
        CHECK(last->yieldsValue);
        const auto *inner = dynamic_cast<const BlockExpr *>(last->expr.get());
        REQUIRE(inner);
        CHECK(inner->isUnsafe);
    }

    TEST_CASE("unsafe func is parsed at module level, in extend blocks, as an intrinsic and nested in a body") {
        const ParseResult result = ParseClean(R"(
struct Cell { value: int; }

pub unsafe func Load(pointer: *int) -> int {
    return *pointer;
}

extend Cell {
    pub unsafe func Read(*self) -> int {
        return self.value;
    }
}

pub intrinsic unsafe func AtomicLoad<T>(address: *T, order: int) -> T;

func Outer() {
    unsafe func Inner(pointer: *int) -> int {
        return *pointer;
    }
}
)");
        const FuncDecl &load = Function(result, "Load");
        CHECK(load.isUnsafe);
        CHECK(load.isPublic);
        CHECK_EQ(load.unsafeLocation.line, 4u);
        CHECK_EQ(load.unsafeLocation.column, 5u);
        const FuncDecl &atomic = Function(result, "AtomicLoad");
        CHECK(atomic.isUnsafe);
        CHECK_FALSE(atomic.intrinsicName.empty());
        const auto *extension = dynamic_cast<const ImplDecl *>(result.module.items[2].get());
        REQUIRE(extension);
        REQUIRE(extension->methods.size() == 1);
        CHECK(extension->methods.front()->isUnsafe);
        const FuncDecl &outer = Function(result, "Outer");
        const auto *nested = dynamic_cast<const DeclStmt *>(outer.body->stmts.front().get());
        REQUIRE(nested);
        const auto *inner = dynamic_cast<const FuncDecl *>(nested->decl.get());
        REQUIRE(inner);
        CHECK(inner->isUnsafe);
    }

    TEST_CASE("unsafe is a reserved keyword and is rejected where nothing can carry it") {
        CHECK(HasParseError(ParseSource("func Main() { let unsafe = 1; }"), "expected"));
        CHECK(
            HasParseError(ParseSource("unsafe struct Cell { value: int; }"), "expected 'func' or '{' after 'unsafe'"));
        CHECK(HasParseError(ParseSource("#Link(\"libc.so.6\")\nunsafe extern func abs(value: int32) -> int32;"),
                            "'unsafe' cannot mark an extern declaration"));
        CHECK(HasParseError(ParseSource("unsafe asm func Nothing() {\n    ret\n}"),
                            "'unsafe' cannot mark an 'asm' function"));
        CHECK(HasParseError(ParseSource("func Main() -> int { return unsafe 1; }"), "after 'unsafe'"));
        CHECK(HasParseError(ParseSource("interface Shape { unsafe func Area(&self) -> int; }"),
                            "interface requirement 'Area' cannot be 'unsafe'"));
        CHECK(HasParseError(ParseSource(R"(
struct Holder { value: int; }
extend Holder {
    unsafe func ~Holder(&var self) {}
    unsafe func [](&self, index: uint) -> int { return self.value; }
}
)"),
                            "destructor '~Holder' cannot be 'unsafe'"));
    }

    TEST_CASE("every raw operation needs an unsafe context in every expression position") {
        const std::vector<std::string> errors = Errors(R"(
struct Node { value: int; next: *var Node; }

func Use(value: int) -> int { return value; }

func Main() -> int {
    var node = Node { value: 1, next: null };
    let pointer: *var Node = @node;
    var values: int[2] = [1, 2];
    let first: *var int = @values[0];
    let fromCall = Use(*first);
    let fromCondition = if first[1] == 2 { 1 } else { 0 };
    var loops = 0;
    while pointer.value < 3 {
        pointer.value += 1;
        loops += 1;
    }
    let fromMatch = match loops {
        0 => *first,
        _ => first[0],
    };
    defer *first = 0;
    return fromCall + fromCondition + fromMatch;
}
)");
        CHECK_EQ(CountContaining(errors, "dereferencing a raw pointer requires 'unsafe'"), 3u);
        CHECK_EQ(CountContaining(errors, "indexing a raw pointer requires 'unsafe'"), 2u);
        CHECK_EQ(CountContaining(errors, "accessing field 'value' through a raw pointer requires 'unsafe'"), 2u);
    }

    TEST_CASE("the same operations are accepted inside unsafe blocks, unsafe functions and every block arm") {
        const std::string source = R"(
struct Node { value: int; }

// Safety: `pointer` must address a live `Node`.
unsafe func Bump(pointer: *var Node) {
    pointer.value += 1;
}

func Generic<T>(pointer: *T) -> T {
    // Safety: `pointer` addresses a live `T`, as callers promise.
    return unsafe { *pointer };
}

func Main() -> int {
    var node = Node { value: 1 };
    let pointer: *var Node = @node;
    var loops = 0;
    while loops < 2 {
        // Safety: `pointer` is `@node`, a live local.
        unsafe {
            Bump(pointer);
            if pointer.value > 5 {
                break;
            }
        }
        loops += 1;
    }
    match loops {
        2 => unsafe { pointer.value = 0; },
        _ => {}
    }
    // Safety: `pointer` is `@node`, a live local.
    defer unsafe { pointer.value = 0; }
    return Generic(@node.value) + loops;
}
)";
        const auto diagnostics = AnalyzeSource(source);
        for (const auto &diagnostic : diagnostics) {
            CAPTURE(diagnostic.message);
        }
        CHECK(diagnostics.empty());
    }

    TEST_CASE("addresses, pointer copies, comparisons and weakening casts stay safe") {
        const auto diagnostics = AnalyzeSource(R"(
#Link("libc.so.6")
extern opterr: int32;

func Main() -> int {
    var value = 1;
    let pointer: *var int = @value;
    let copy = pointer;
    let readable: *int = pointer as *int;
    let untyped = pointer as *opaque;
    let address = pointer as uint;
    let same = copy == pointer && pointer != null;
    let external = @opterr;
    return if same { (address + (readable as uint) + (untyped as uint) + (external as uint)) as int } else { 0 };
}
)");
        for (const auto &diagnostic : diagnostics) {
            CAPTURE(diagnostic.message);
        }
        CHECK(diagnostics.empty());
    }

    TEST_CASE("an unsafe function of another package needs an unsafe context at its call") {
        const std::string dependency = R"(
/// Reads the value at `pointer`.
/// Safety: `pointer` must address a live `int`.
pub unsafe func Load(pointer: *int) -> int {
    return *pointer;
}
)";
        const auto unmarked = Messages(AnalyzeWithDep(R"(
import Raw::Load;

func Main() -> int {
    var value = 1;
    return Load(@value);
}
)",
                                                      "Raw", dependency),
                                       SemanticDiagnostic::Severity::Error);
        REQUIRE(unmarked.size() == 1);
        CHECK_EQ(unmarked.front(), "call to unsafe function 'Load' requires 'unsafe'");

        const auto marked = AnalyzeWithDep(R"(
import Raw::Load;

func Main() -> int {
    var value = 1;
    // Safety: `value` is a live local.
    return unsafe { Load(@value) };
}
)",
                                           "Raw", dependency);
        for (const auto &diagnostic : marked) {
            CAPTURE(diagnostic.message);
        }
        CHECK(marked.empty());
    }

    TEST_CASE("the diagnostic for an operation outside unsafe names why and how to fix it") {
        const auto diagnostics = AnalyzeSource(R"(
func Main() -> int {
    var value = 1;
    let pointer: *int = @value;
    return *pointer;
}
)");
        REQUIRE(diagnostics.size() == 1);
        const SemanticDiagnostic &diagnostic = diagnostics.front();
        CHECK_EQ(diagnostic.message, "dereferencing a raw pointer requires 'unsafe'");
        CHECK_EQ(diagnostic.location.line, 5u);
        CHECK_EQ(diagnostic.location.column, 12u);
        REQUIRE(diagnostic.help.has_value());
        CHECK(diagnostic.help->contains("unsafe { ... }"));
        CHECK(diagnostic.help->contains("// Safety:"));
        CHECK(std::ranges::any_of(
            diagnostic.notes, [](const std::string &note) { return note.contains("raw pointer addresses a live"); }));
    }

    TEST_CASE("a needless unsafe block is a warning, and one whose contents fail to check is not") {
        const std::vector<std::string> warnings = Warnings(R"(
func Main() -> int {
    var value = 1;
    // Safety: nothing here needs it.
    let sum = unsafe { value + 1 };
    // Safety: `value` is a live local.
    let read = unsafe {
        let pointer: *int = @value;
        // Safety: the same.
        unsafe { *pointer }
    };
    return sum + read;
}
)");
        REQUIRE(warnings.size() == 2);
        CHECK_EQ(warnings[0], "unnecessary 'unsafe' block");
        CHECK_EQ(warnings[1], "unnecessary 'unsafe' block");

        const auto failing = AnalyzeSource(R"(
func Main() -> int {
    // Safety: never reached.
    return unsafe { Missing() };
}
)");
        CHECK(Messages(failing, SemanticDiagnostic::Severity::Warning).empty());
        CHECK_FALSE(Messages(failing, SemanticDiagnostic::Severity::Error).empty());
    }

    TEST_CASE("an unsafe statement block that returns or breaks leaves as its statements would") {
        const auto diagnostics = AnalyzeSource(R"(
func First(pointer: *int) -> int {
    // Safety: `pointer` addresses a live `int`, as callers promise.
    unsafe {
        return *pointer;
    }
}

func Count(pointer: *int) -> int {
    var count = 0;
    loop {
        // Safety: `pointer` addresses a live `int`, as callers promise.
        unsafe {
            if *pointer == count {
                break;
            }
        }
        count += 1;
    }
    return count;
}

func Main() -> int {
    var value = 2;
    return First(@value) + Count(@value);
}
)");
        for (const auto &diagnostic : diagnostics) {
            CAPTURE(diagnostic.message);
        }
        CHECK(diagnostics.empty());
    }
}

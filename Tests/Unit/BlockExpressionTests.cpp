// Plain blocks: `{ ... }` as a value block and as a statement, the removed `unsafe` keyword, and raw operations, which
// need no mark wherever they are written.

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
    Lexer lexer(std::string(source), "block.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "block.rux");
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

std::vector<std::string> Errors(const std::string &source) {
    std::vector<std::string> messages;
    for (const auto &diagnostic : AnalyzeSource(source)) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            messages.push_back(diagnostic.message);
        }
    }
    return messages;
}
} // namespace

TEST_SUITE("BlockExpression") {
    TEST_CASE("a block is a value block in expression position and ends a statement at its brace") {
        const ParseResult result = ParseClean(R"(
func Read(pointer: *int) -> int {
    let value = { *pointer };
    {
        let again = *pointer;
    }
    { let third = *pointer; };
    return { *pointer + value };
}
)");
        const FuncDecl &read = Function(result, "Read");
        REQUIRE(read.body);
        REQUIRE(read.body->stmts.size() == 4);

        const auto *let = dynamic_cast<const LetStmt *>(read.body->stmts[0].get());
        REQUIRE(let);
        const auto *valueBlock = dynamic_cast<const BlockExpr *>(let->init.get());
        REQUIRE(valueBlock);
        REQUIRE(valueBlock->block->stmts.size() == 1);
        const auto *tail = dynamic_cast<const ExprStmt *>(valueBlock->block->stmts.front().get());
        REQUIRE(tail);
        CHECK(tail->yieldsValue);

        for (const std::size_t index : {1u, 2u}) {
            const auto *statement = dynamic_cast<const ExprStmt *>(read.body->stmts[index].get());
            REQUIRE(statement);
            CHECK(dynamic_cast<const BlockExpr *>(statement->expr.get()));
            CHECK_FALSE(statement->yieldsValue);
        }
        // The ';' after a statement block is allowed and recorded, as after any expression statement.
        CHECK(dynamic_cast<const ExprStmt *>(read.body->stmts[2].get())->semicolon.has_value());
    }

    TEST_CASE("a block that ends a value block and yields is that block's value") {
        const ParseResult result = ParseClean(R"(
func Pick(flag: bool, pointer: *int) -> int {
    return if flag {
        { *pointer }
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
        CHECK(dynamic_cast<const BlockExpr *>(last->expr.get()));
    }

    TEST_CASE("a brace that opens a body is never read as a block operand") {
        const ParseResult result = ParseClean(R"(
func Count(limit: int) -> int {
    var count = 0;
    while count < limit {
        count += 1;
    }
    if count == limit {
        return count;
    }
    return 0;
}
)");
        CHECK(Function(result, "Count").body->stmts.size() == 4);
    }

    TEST_CASE("unsafe is reserved and rejected wherever it is written, and what follows is read as written") {
        const ParseResult blocks =
            ParseSource("func Main() -> int { let value = unsafe { 1 }; unsafe { }; return 0; }");
        CHECK(HasParseError(blocks, "'unsafe' is not supported"));
        CHECK(std::ranges::count_if(blocks.diagnostics, [](const ParserDiagnostic &diagnostic) {
                  return diagnostic.message.contains("'unsafe' is not supported");
              }) == 2);
        const ParseResult function = ParseSource("pub unsafe func Load(pointer: *int) -> int { return *pointer; }");
        CHECK(HasParseError(function, "'unsafe' is not supported"));
        CHECK(Function(function, "Load").isPublic);
        CHECK(HasParseError(ParseSource("unsafe struct Cell { value: int; }"), "'unsafe' is not supported"));
        CHECK(HasParseError(ParseSource("func Main() { let unsafe = 1; }"), "expected"));
    }

    TEST_CASE("raw pointer operations, extern access and pointer casts need no mark in any position") {
        const auto errors = Errors(R"(
#Link("libc.so.6")
extern func abs(value: int32) -> int32;

struct Node { value: int; next: *var Node; }

func Walk(node: *var Node, raw: *opaque) -> int {
    var total = (*node).value + node.value;
    let typed = raw as *var Node;
    let next = node + 1;
    total += typed.value + next.value + node[0].value;
    total += abs(-3) as int;
    return match node.value {
        0 => (*node.next).value,
        _ => total,
    };
}
)");
        const std::string first = errors.empty() ? std::string{} : errors.front();
        INFO(first);
        CHECK(errors.empty());
    }

    TEST_CASE("a statement block that yields an unused value is reported at its last expression") {
        const auto errors = Errors(R"(
func Main() -> int {
    {
        41
    }
    return 0;
}
)");
        CHECK(std::ranges::any_of(errors, [](const std::string &message) {
            return message.contains("this block yields a value of type 'int' that is not used");
        }));
    }

    TEST_CASE("a statement block that returns or breaks leaves as its statements would") {
        const auto diagnostics = AnalyzeSource(R"(
func First(pointer: *int) -> int {
    {
        return *pointer;
    }
}

func Count(pointer: *int) -> int {
    var count = 0;
    loop {
        {
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

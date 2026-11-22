// What analysis decides for typed throws: which calls are throw sites, where each one's error goes and what that
// target records, the handler's error type, the diagnostics for an unmarked call, a `try` with nothing to mark, an
// error with nowhere to go or one that does not fit, and the barrier a deferred statement or destructor puts up. The
// older `-> T ! E` spelling and its operators keep working beside the new one.

#include "SemanticTestSupport.h"

#include <memory>
#include <optional>
#include <stdexcept>

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
std::vector<std::string> Messages(const std::string &source, const SemanticDiagnostic::Severity severity) {
    std::vector<std::string> messages;
    for (const auto &diagnostic : AnalyzeSource(source)) {
        if (diagnostic.severity == severity) {
            messages.push_back(diagnostic.message);
        }
    }
    return messages;
}

std::vector<std::string> Errors(const std::string &source) {
    return Messages(source, SemanticDiagnostic::Severity::Error);
}

bool AnyContains(const std::vector<std::string> &messages, const std::string_view text) {
    return std::ranges::any_of(messages, [&](const std::string &message) { return message.contains(text); });
}

constexpr std::string_view kDeclarations = R"(
    variant DigitError { Blank, NotADigit(char) }
    struct IoError { code: int32; }
    func ReadDigit(c: char) -> int ! DigitError {
        if c == ' ' { throw .Blank; }
        return 1;
    }
    func OldDigit(c: char) -> int ! DigitError { return 1; }
    func ReadFile(code: int32) -> int32 ! IoError { return code; }
)";

std::string WithDeclarations(const std::string_view body) {
    return std::string(kDeclarations) + std::string(body);
}

/// A parsed and analyzed source kept alive for the facts its model holds.
struct Analyzed {
    ParseResult parsed;
    std::unique_ptr<SemanticModel> model;
};

Analyzed Analyze(const std::string &source) {
    Lexer lexer(source, "throws.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "throws.rux");
    Analyzed analyzed{parser.Parse(), nullptr};
    REQUIRE_FALSE(analyzed.parsed.HasErrors());
    SemanticAnalyzer analyzer({&analyzed.parsed.module}, {}, "test", "Windows");
    analyzed.model = std::make_unique<SemanticModel>(analyzer.Analyze());
    for (const auto &diagnostic : analyzed.model->diagnostics) {
        CAPTURE(diagnostic.message);
    }
    REQUIRE_FALSE(analyzed.model->HasErrors());
    return analyzed;
}

const FuncDecl &Function(const Analyzed &analyzed, const std::string_view name) {
    for (const auto &item : analyzed.parsed.module.items) {
        if (const auto *function = dynamic_cast<const FuncDecl *>(item.get()); function && function->name == name) {
            return *function;
        }
    }
    FAIL("missing function " << name);
    throw std::runtime_error("missing function");
}

const Expr &InitializerOf(const FuncDecl &function, const std::size_t index) {
    const auto *let = dynamic_cast<const LetStmt *>(function.body->stmts.at(index).get());
    REQUIRE(let != nullptr);
    return *let->init;
}
} // namespace

TEST_CASE("an unmarked call is a fallible value, an unused one is an error, and a marked one is its success") {
    const std::string keptSource = WithDeclarations(R"(
        func Main() -> int {
            let a = ReadDigit('1');
            return a !! 0;
        }
    )");
    CHECK(Errors(keptSource).empty());
    const Analyzed kept = Analyze(keptSource);
    CHECK(ResolvedType(*kept.model, InitializerOf(Function(kept, "Main"), 0)).DisplayString() == "int ! DigitError");
    const auto errors = Errors(WithDeclarations(R"(
        func Main() -> int {
            ReadDigit('1');
            return 0;
        }
    )"));
    REQUIRE(errors.size() == 1);
    CHECK(errors.front() == "'ReadDigit' can fail with 'DigitError', and the result is not used");

    const Analyzed analyzed = Analyze(WithDeclarations(R"(
        func Main() -> int ! DigitError {
            let a = ReadDigit('1')! + ReadDigit('2')!;
            return a;
        }
    )"));
    const auto *sum = dynamic_cast<const BinaryExpr *>(&InitializerOf(Function(analyzed, "Main"), 0));
    REQUIRE(sum != nullptr);
    for (const Expr *operand : {sum->left.get(), sum->right.get()}) {
        const auto *marked = dynamic_cast<const ErrorFormExpr *>(operand);
        REQUIRE(marked != nullptr);
        CHECK(ResolvedType(*analyzed.model, *marked) == TypeRef::MakeInt());
        const ResolvedThrowSite *site = analyzed.model->TryGetThrowSite(*marked->operand);
        REQUIRE(site != nullptr);
        CHECK(site->payloadType == TypeRef::MakeInt());
        CHECK(site->errorType.ToString() == "DigitError");
        CHECK(site->LeavesFunction());
        CHECK(site->markExpression == marked);
    }
}

TEST_CASE("a handler takes only its own operand's error") {
    const Analyzed analyzed = Analyze(WithDeclarations(R"(
        func Main() -> int {
            let maybe = ReadDigit('1') !! none;
            let value = ReadDigit('2') catch { _ => 0 };
            return value;
        }
    )"));
    const FuncDecl &main = Function(analyzed, "Main");
    const auto *maybe = dynamic_cast<const ErrorFormExpr *>(&InitializerOf(main, 0));
    REQUIRE(maybe != nullptr);
    CHECK(ResolvedType(*analyzed.model, *maybe).ToString() == "int?");
    const ResolvedThrowSite *optional = analyzed.model->TryGetThrowSite(*maybe->operand);
    REQUIRE(optional != nullptr);
    CHECK(optional->handlerExpression == maybe);

    const auto *value = dynamic_cast<const ErrorFormExpr *>(&InitializerOf(main, 1));
    REQUIRE(value != nullptr);
    const ResolvedThrowHandler *handler = analyzed.model->TryGetThrowHandler(*value);
    REQUIRE(handler != nullptr);
    CHECK(handler->errorType.ToString() == "DigitError");
    CHECK(handler->valueType == TypeRef::MakeInt());

    // An `x!` inside a handled operand leaves the function: the handler sees only its operand's own error.
    CHECK(AnyContains(Errors(WithDeclarations(R"(
        func Main() -> int {
            return (ReadDigit('1')! + 1) catch { _ => 0 };
        }
    )")),
                      "call to 'ReadDigit' can fail with 'DigitError', but 'Main' cannot fail"));
}

TEST_CASE("a throw leaves the function, from an arm as from anywhere else") {
    const Analyzed analyzed = Analyze(WithDeclarations(R"(
        func Main() -> int ! IoError {
            let value = ReadDigit('1') catch { _ => throw IoError { code: 1i32 } };
            return value;
        }
    )"));
    const auto *value = dynamic_cast<const ErrorFormExpr *>(&InitializerOf(Function(analyzed, "Main"), 0));
    REQUIRE(value != nullptr);
    const auto *rethrow = dynamic_cast<const DivergeExpr *>(value->arms.front().body.get());
    REQUIRE(rethrow != nullptr);
    const ResolvedThrowSite *outside = analyzed.model->TryGetThrowSite(*rethrow);
    REQUIRE(outside != nullptr);
    CHECK(outside->LeavesFunction());
}

TEST_CASE("an error with nowhere to go, or one that does not fit, is reported") {
    CHECK(AnyContains(Errors(WithDeclarations("func Main() -> int { return ReadDigit('1')!; }")),
                      "call to 'ReadDigit' can fail with 'DigitError', but 'Main' cannot fail"));
    CHECK(AnyContains(Errors(WithDeclarations("func Load() -> int ! IoError { return ReadDigit('1')!; }")),
                      "call to 'ReadDigit' can fail with 'DigitError', but 'Load' fails with 'IoError'"));
    // Injection into a sum and widening into a larger one are accepted.
    CHECK(Errors(WithDeclarations(R"(
        func Either() -> int ! IoError | DigitError { return ReadDigit('1')!; }
        func Wider() -> int ! IoError | DigitError | char { return Either()!; }
    )"))
              .empty());
    CHECK(AnyContains(Errors(WithDeclarations("func F() { throw IoError { code: 1i32 }; }")),
                      "'throw' needs a function that can fail, but 'F' returns no value"));
}

TEST_CASE("no error leaves a deferred statement or a destructor") {
    CHECK(AnyContains(Errors(WithDeclarations("func F() { defer ReadDigit('1')!; }")),
                      "an error cannot leave a deferred statement"));
    CHECK(AnyContains(Errors(WithDeclarations(R"(
        struct File { handle: int32; }
        extend File {
            func ~(&var self) { ReadFile(self.handle)!; }
        }
    )")),
                      "an error cannot leave a destructor"));
    CHECK(Errors(WithDeclarations(R"(
        func F() {
            defer ReadDigit('1') !!;
            defer ReadDigit('2') catch { _ => {} };
        }
    )"))
              .empty());
}

TEST_CASE("an arm's call is a value of its own and the arms cover every error") {
    CHECK(AnyContains(Errors(WithDeclarations(R"(
        func Main() -> int {
            ReadDigit('1') catch { _ => { ReadDigit('2'); } };
            return 0;
        }
    )")),
                      "'ReadDigit' can fail with 'DigitError', and the result is not used"));
    CHECK(AnyContains(Errors(WithDeclarations(R"(
        func Main() -> int {
            ReadDigit('1') catch { .Blank => {} };
            return 0;
        }
    )")),
                      "missing DigitError::NotADigit"));
}

TEST_CASE("an error handled by the arms carries the state at the call into them") {
    const auto errors = Errors(WithDeclarations(R"(
        struct Resource { id: int32; }
        extend Resource { func ~(&var self) {} }
        func Take(resource: Resource, digit: int) -> int32 { return 0i32; }
        func Read(resource: &Resource) -> int32 { return resource.id; }
        func Before() -> int32 {
            var resource = Resource { id: 1i32 };
            let digit = ReadDigit('1') catch { _ => return Read(&resource) };
            return Take(resource, digit);
        }
        func After() -> int32 {
            var resource = Resource { id: 1i32 };
            Take(resource, 0);
            ReadDigit('1') catch { _ => return Read(&resource) };
            return 0i32;
        }
    )"));
    // Only the handler after the move reaches the arm without the resource.
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().contains("'resource'"));
}

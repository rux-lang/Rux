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

std::vector<std::string> Warnings(const std::string &source) {
    return Messages(source, SemanticDiagnostic::Severity::Warning);
}

bool AnyContains(const std::vector<std::string> &messages, const std::string_view text) {
    return std::ranges::any_of(messages, [&](const std::string &message) { return message.contains(text); });
}

constexpr std::string_view kDeclarations = R"(
    variant DigitError { Blank, NotADigit(char) }
    struct IoError { code: int32; }
    func ReadDigit(c: char) -> int throws DigitError {
        if c == ' ' { throw .Blank; }
        return 1;
    }
    func OldDigit(c: char) -> int ! DigitError { return 1; }
    func ReadFile(code: int32) -> int32 throws IoError { return code; }
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

TEST_CASE("an unmarked call of a throws function is an error and a marked one evaluates to its success") {
    const auto errors = Errors(WithDeclarations(R"(
        func Main() -> int throws DigitError {
            let a = ReadDigit('1');
            return a;
        }
    )"));
    REQUIRE(errors.size() == 1);
    CHECK(errors.front() == "call to 'ReadDigit' can throw 'DigitError' but is not marked with 'try'");

    const Analyzed analyzed = Analyze(WithDeclarations(R"(
        func Main() -> int throws DigitError {
            let a = try ReadDigit('1') + ReadDigit('2');
            return a;
        }
    )"));
    const auto *marked = dynamic_cast<const TryMarkExpr *>(&InitializerOf(Function(analyzed, "Main"), 0));
    REQUIRE(marked != nullptr);
    CHECK(ResolvedType(*analyzed.model, *marked) == TypeRef::MakeInt());
    const auto *sum = dynamic_cast<const BinaryExpr *>(marked->operand.get());
    REQUIRE(sum != nullptr);
    for (const Expr *call : {sum->left.get(), sum->right.get()}) {
        const ResolvedThrowSite *site = analyzed.model->TryGetThrowSite(*call);
        REQUIRE(site != nullptr);
        CHECK(site->payloadType == TypeRef::MakeInt());
        CHECK(site->errorType.ToString() == "DigitError");
        CHECK(site->LeavesFunction());
        CHECK(site->markExpression == marked);
        CHECK(site->markBlock == nullptr);
        CHECK(ResolvedType(*analyzed.model, *call) == TypeRef::MakeInt());
    }
}

TEST_CASE("a try block marks its calls and sends their errors to its arms") {
    const Analyzed analyzed = Analyze(WithDeclarations(R"(
        func Main() -> int {
            try {
                let digit = ReadDigit('1');
                let code = ReadFile(2i32);
            } catch {
                _: DigitError => {}
                e: IoError => {}
            }
            return 0;
        }
    )"));
    const FuncDecl &main = Function(analyzed, "Main");
    const auto *handler = dynamic_cast<const TryStmt *>(main.body->stmts.front().get());
    REQUIRE(handler != nullptr);
    const ResolvedThrowHandler *resolved = analyzed.model->TryGetThrowHandler(*handler);
    REQUIRE(resolved != nullptr);
    // The handler takes the sum of what its calls throw.
    CHECK(resolved->errorType == TypeRef::MakeSum({TypeRef::MakeNamed("DigitError"), TypeRef::MakeNamed("IoError")}));
    for (const auto &statement : handler->body->stmts) {
        const auto *let = dynamic_cast<const LetStmt *>(statement.get());
        REQUIRE(let != nullptr);
        const ResolvedThrowSite *site = analyzed.model->TryGetThrowSite(*let->init);
        REQUIRE(site != nullptr);
        CHECK(site->handlerBlock == handler);
        CHECK(site->markBlock == handler);
        CHECK_FALSE(site->LeavesFunction());
    }
}

TEST_CASE("a throw goes to the innermost handler, and an arm's error goes further out") {
    const Analyzed analyzed = Analyze(WithDeclarations(R"(
        func Main() -> int throws IoError {
            try {
                ReadDigit('1');
                throw DigitError::Blank;
            } catch {
                _ => throw IoError { code: 1i32 },
            }
            return 0;
        }
    )"));
    const FuncDecl &main = Function(analyzed, "Main");
    const auto *handler = dynamic_cast<const TryStmt *>(main.body->stmts.front().get());
    REQUIRE(handler != nullptr);
    const auto *thrown = dynamic_cast<const FailStmt *>(handler->body->stmts[1].get());
    REQUIRE(thrown != nullptr);
    const ResolvedThrowSite *inside = analyzed.model->TryGetThrowSite(*thrown);
    REQUIRE(inside != nullptr);
    CHECK(inside->handlerBlock == handler);
    CHECK(inside->markBlock == nullptr);

    const auto *rethrow = dynamic_cast<const DivergeExpr *>(handler->arms.front().body.get());
    REQUIRE(rethrow != nullptr);
    const ResolvedThrowSite *outside = analyzed.model->TryGetThrowSite(*rethrow);
    REQUIRE(outside != nullptr);
    CHECK(outside->LeavesFunction());
}

TEST_CASE("try? and try ... catch are handlers of their own operands") {
    const Analyzed analyzed = Analyze(WithDeclarations(R"(
        func Main() -> int {
            let maybe = try? ReadDigit('1');
            let value = try ReadDigit('2') catch { _ => 0 };
            return value;
        }
    )"));
    const FuncDecl &main = Function(analyzed, "Main");
    const auto *maybe = dynamic_cast<const TryMarkExpr *>(&InitializerOf(main, 0));
    REQUIRE(maybe != nullptr);
    CHECK(ResolvedType(*analyzed.model, *maybe).ToString() == "int?");
    const ResolvedThrowSite *optional = analyzed.model->TryGetThrowSite(*maybe->operand);
    REQUIRE(optional != nullptr);
    CHECK(optional->handlerExpression == maybe);

    const auto *value = dynamic_cast<const TryMarkExpr *>(&InitializerOf(main, 1));
    REQUIRE(value != nullptr);
    const ResolvedThrowHandler *handler = analyzed.model->TryGetThrowHandler(*value);
    REQUIRE(handler != nullptr);
    CHECK(handler->errorType.ToString() == "DigitError");
    CHECK(handler->valueType == TypeRef::MakeInt());
}

TEST_CASE("an error with nowhere to go, or one that does not fit, is reported") {
    CHECK(AnyContains(Errors(WithDeclarations("func Main() -> int { return try ReadDigit('1'); }")),
                      "call to 'ReadDigit' can throw 'DigitError', but 'Main' does not throw"));
    CHECK(AnyContains(Errors(WithDeclarations("func Load() -> int throws IoError { return try ReadDigit('1'); }")),
                      "call to 'ReadDigit' can throw 'DigitError', but 'Load' throws 'IoError'"));
    // Injection into a sum and widening into a larger one are accepted.
    CHECK(Errors(WithDeclarations(R"(
        func Either() -> int throws IoError | DigitError { return try ReadDigit('1'); }
        func Wider() -> int throws IoError | DigitError | char { return try Either(); }
    )"))
              .empty());
    CHECK(AnyContains(Errors(WithDeclarations("func F() { throw IoError { code: 1i32 }; }")),
                      "'throw' needs a function that throws, but 'F' returns no value"));
}

TEST_CASE("a try that marks nothing is a warning") {
    const auto warnings = Warnings(WithDeclarations(R"(
        func Main() -> int {
            let a = try 1 + 2;
            let b = try? 3;
            try {
                let c = 4;
            } catch {
                _ => {}
            }
            return a;
        }
    )"));
    CHECK(std::ranges::count(warnings, std::string("'try' has no call that can throw")) == 3);
    // A `throw` is what a `try` block catches, so a block holding only one marks something.
    CHECK(Warnings(WithDeclarations(R"(
        func Main() -> int {
            try { throw IoError { code: 1i32 }; } catch { _ => {} }
            return 0;
        }
    )"))
              .empty());
}

TEST_CASE("no error leaves a deferred statement or a destructor") {
    CHECK(AnyContains(Errors(WithDeclarations("func F() { defer try ReadDigit('1'); }")),
                      "an error cannot leave a deferred statement"));
    CHECK(AnyContains(Errors(WithDeclarations(R"(
        struct File { handle: int32; }
        extend File {
            func ~File(&var self) { try ReadFile(self.handle); }
        }
    )")),
                      "an error cannot leave a destructor"));
    CHECK(Errors(WithDeclarations(R"(
        func F() {
            defer try? ReadDigit('1');
            defer try ReadDigit('2') catch { _ => 0 };
            defer if true { try { ReadDigit('3'); } catch { _ => {} } }
        }
    )"))
              .empty());
}

TEST_CASE("an arm needs its own try and the arms cover every error") {
    CHECK(AnyContains(Errors(WithDeclarations(R"(
        func Main() -> int {
            try { ReadDigit('1'); } catch { _ => ReadDigit('2') }
            return 0;
        }
    )")),
                      "call to 'ReadDigit' can throw 'DigitError' but is not marked with 'try'"));
    CHECK(AnyContains(Errors(WithDeclarations(R"(
        func Main() -> int {
            try { ReadDigit('1'); } catch { .Blank => {} }
            return 0;
        }
    )")),
                      "missing DigitError::NotADigit"));
}

TEST_CASE("the older spelling keeps working beside throws") {
    // An unmarked call of an older function is its outcome; under `try` it is a throw site.
    const Analyzed analyzed = Analyze(WithDeclarations(R"(
        func Main() -> int throws DigitError {
            let outcome = OldDigit('1');
            let value = try OldDigit('2');
            let older = ReadDigit('3') catch { _ => 0 };
            return value + older;
        }
        func Forward() -> int ! DigitError {
            return ReadDigit('4')? + ReadDigit('5')? else (e => e);
        }
    )"));
    const FuncDecl &main = Function(analyzed, "Main");
    CHECK(ResolvedType(*analyzed.model, InitializerOf(main, 0)).ToString() == "int ! DigitError");
    CHECK(analyzed.model->TryGetThrowSite(InitializerOf(main, 0)) == nullptr);
    const auto *marked = dynamic_cast<const TryMarkExpr *>(&InitializerOf(main, 1));
    REQUIRE(marked != nullptr);
    CHECK(analyzed.model->TryGetThrowSite(*marked->operand) != nullptr);
    const auto *older = dynamic_cast<const CatchExpr *>(&InitializerOf(main, 2));
    REQUIRE(older != nullptr);
    CHECK(analyzed.model->TryGetThrowSite(*older->subject) == nullptr);
}

TEST_CASE("an error thrown to a handler carries the state at the throw into the arms") {
    const auto errors = Errors(WithDeclarations(R"(
        struct Resource { id: int32; }
        extend Resource { func ~Resource(&var self) {} }
        func Take(resource: Resource) {}
        func Read(resource: &Resource) -> int32 { return resource.id; }
        func Before() -> int32 {
            var resource = Resource { id: 1i32 };
            try {
                ReadDigit('1');
                Take(move resource);
            } catch {
                _ => return Read(resource),
            }
            return 0i32;
        }
        func After() -> int32 {
            var resource = Resource { id: 1i32 };
            try {
                Take(move resource);
                ReadDigit('1');
            } catch {
                _ => return Read(resource),
            }
            return 0i32;
        }
    )"));
    // Only the throw after the move reaches the arm without the resource.
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().contains("'resource'"));
}

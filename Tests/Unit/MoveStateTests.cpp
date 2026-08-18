#include "Lexer/Lexer.h"
#include "Semantic/Detail/MoveStateTracker.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <utility>

using namespace Rux;
using namespace Rux::SemanticDetail;

TEST_CASE("straight-line move state records locals temporaries assignments and snapshots") {
    MoveStateTracker tracker;
    int local = 0;
    int temporary = 0;
    int nested = 0;
    const auto localIdentity = MoveStateTracker::Local(&local);
    const auto temporaryIdentity = MoveStateTracker::Temporary(&temporary);
    const auto nestedIdentity = MoveStateTracker::Local(&nested);

    tracker.Reset();
    tracker.Declare(localIdentity, MoveStateTracker::State::Uninitialized, {2, 5, 10});
    const auto uninitialized = tracker.Read(localIdentity);
    REQUIRE(uninitialized.has_value());
    CHECK(uninitialized->kind == MoveStateTracker::IssueKind::Uninitialized);
    CHECK_EQ(uninitialized->previousTransition.line, 2);
    CHECK_EQ(uninitialized->previousTransition.column, 5);

    tracker.Assign(localIdentity, {3, 5, 20});
    CHECK_FALSE(tracker.Read(localIdentity).has_value());
    CHECK_FALSE(tracker.Move(localIdentity, {4, 9, 30}).has_value());
    const auto moved = tracker.Read(localIdentity);
    REQUIRE(moved.has_value());
    CHECK(moved->kind == MoveStateTracker::IssueKind::Moved);
    CHECK_EQ(moved->previousTransition.line, 4);
    CHECK_EQ(moved->previousTransition.column, 9);

    tracker.Declare(temporaryIdentity, MoveStateTracker::State::Initialized, {5, 7, 40});
    CHECK_FALSE(tracker.Move(temporaryIdentity, {5, 12, 45}).has_value());
    REQUIRE(tracker.Move(temporaryIdentity, {5, 18, 51}).has_value());

    const MoveStateTracker::Snapshot movedSnapshot = tracker.Save();
    tracker.Assign(localIdentity, {6, 5, 60});
    CHECK_FALSE(tracker.Read(localIdentity).has_value());
    tracker.Restore(movedSnapshot);
    REQUIRE(tracker.Read(localIdentity).has_value());
    CHECK(tracker.Read(localIdentity)->kind == MoveStateTracker::IssueKind::Moved);

    tracker.BeginScope();
    tracker.Declare(nestedIdentity, MoveStateTracker::State::Initialized, {7, 9, 70});
    REQUIRE(tracker.TryGet(nestedIdentity) != nullptr);
    tracker.EndScope();
    CHECK(tracker.TryGet(nestedIdentity) == nullptr);

    tracker.Assign(localIdentity, {8, 5, 80});
    CHECK_FALSE(tracker.Read(localIdentity).has_value());
    tracker.Reset();
    CHECK(tracker.TryGet(localIdentity) == nullptr);
    CHECK(tracker.TryGet(temporaryIdentity) == nullptr);
}

TEST_CASE("semantic analysis diagnoses straight-line reads before initialization") {
    Lexer lexer(R"(
        func Main(parameter: int32) -> int32 {
            var pending: int32;
            pending;
            pending = parameter;
            var bytes: uint8[2];
            bytes[0] = 1;
            let first = bytes[0];
            var escaped: int32;
            @escaped;
            escaped;
            return pending;
        }
    )",
                "move_state.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_state.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_EQ(model.diagnostics.size(), 1);
    const SemanticDiagnostic &diagnostic = model.diagnostics.front();
    CHECK(diagnostic.IsError());
    CHECK_EQ(diagnostic.location.line, 4);
    CHECK_EQ(diagnostic.message, "variable 'pending' is used before it is initialized");
    REQUIRE_EQ(diagnostic.notes.size(), 1);
    CHECK_EQ(diagnostic.notes.front(), "'pending' was declared without an initializer at 3:13");
    REQUIRE(diagnostic.help.has_value());
    CHECK_EQ(*diagnostic.help, "assign a value to 'pending' before this use");
}

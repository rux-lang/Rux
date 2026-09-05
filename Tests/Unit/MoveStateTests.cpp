#include "Lexer/Lexer.h"
#include "Semantic/Analysis/MoveStateTracker.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <array>
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
    const MoveStateTracker::Snapshot initializedSnapshot = tracker.Save();
    tracker.Restore(movedSnapshot);
    const MoveStateTracker::Snapshot merged = MoveStateTracker::Merge(std::array{initializedSnapshot, movedSnapshot});
    tracker.Restore(merged);
    const auto possiblyMoved = tracker.Read(localIdentity);
    REQUIRE(possiblyMoved.has_value());
    CHECK(possiblyMoved->kind == MoveStateTracker::IssueKind::PossiblyMoved);

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

TEST_CASE("semantic analysis merges availability across control-flow paths") {
    Lexer lexer(R"(
        func Main(flag: bool) -> int32 {
            var complete: int32;
            if flag { complete = 1; } else { complete = 2; }

            var conditional: int32;
            if flag { conditional = 3; }
            conditional;

            var repeated: int32;
            while flag { repeated = 4; }
            repeated;
            var neverRepeated: int32;
            while false { neverRepeated = 4; }
            neverRepeated;

            var shorted: int32;
            flag && (@shorted != null);
            shorted;
            var constantShortCircuit: int32;
            true && (@constantShortCircuit != null);

            var selected: int32;
            match flag {
                true => { selected = 5; },
                false => { selected = 6; }
            }

            var reachable: int32;
            if flag { return complete + selected; } else { reachable = 7; }
            return complete + selected + reachable + constantShortCircuit;
        }
    )",
                "control_flow_state.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "control_flow_state.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_EQ(model.diagnostics.size(), 4);
    CHECK_EQ(model.diagnostics[0].message, "value 'conditional' may be uninitialized on some control-flow paths");
    CHECK_EQ(model.diagnostics[1].message, "value 'repeated' may be uninitialized on some control-flow paths");
    CHECK_EQ(model.diagnostics[2].message, "variable 'neverRepeated' is used before it is initialized");
    CHECK_EQ(model.diagnostics[3].message, "value 'shorted' may be uninitialized on some control-flow paths");
    for (const SemanticDiagnostic &diagnostic : model.diagnostics) {
        CHECK(diagnostic.IsError());
        REQUIRE_EQ(diagnostic.notes.size(), 1);
        REQUIRE(diagnostic.help.has_value());
    }
}

TEST_CASE("loop exits preserve only reachable availability states") {
    Lexer lexer(R"(
        #NoReturn()
        func Stop() { while true {} }

        func Main() -> int32 {
            var fromBreak: int32;
            loop {
                fromBreak = 1;
                break;
                fromBreak;
            }

            var fromDoWhile: int32;
            do { fromDoWhile = 2; } while false;
            var afterStop: int32;
            Stop();
            afterStop;
            return fromBreak + fromDoWhile;
        }
    )",
                "loop_exit_state.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "loop_exit_state.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    CHECK(model.diagnostics.empty());
}

#include "Semantic/Detail/MoveStateTracker.h"

#include <cstdint>
#include <functional>
#include <ranges>

namespace Rux::SemanticDetail {
MoveStateTracker::Identity MoveStateTracker::Local(const void *address) noexcept {
    return {IdentityKind::Local, address};
}

MoveStateTracker::Identity MoveStateTracker::Temporary(const void *address) noexcept {
    return {IdentityKind::Temporary, address};
}

void MoveStateTracker::Reset() {
    records.clear();
    scopes.clear();
    scopes.emplace_back();
}

void MoveStateTracker::BeginScope() {
    if (scopes.empty()) {
        Reset();
    }
    scopes.emplace_back();
}

void MoveStateTracker::EndScope() {
    if (scopes.empty()) {
        return;
    }
    for (const Identity identity : scopes.back()) {
        records.erase(identity);
    }
    scopes.pop_back();
}

void MoveStateTracker::Declare(const Identity identity, const State state, const SourceLocation location) {
    if (scopes.empty()) {
        Reset();
    }
    const auto [record, inserted] = records.insert_or_assign(identity, Record{state, location});
    static_cast<void>(record);
    if (inserted) {
        scopes.back().push_back(identity);
    }
}

std::optional<MoveStateTracker::Issue> MoveStateTracker::Read(const Identity identity) const {
    const Record *record = TryGet(identity);
    return record ? IssueFor(*record) : std::nullopt;
}

std::optional<MoveStateTracker::Issue> MoveStateTracker::Move(const Identity identity, const SourceLocation location) {
    const auto record = records.find(identity);
    if (record == records.end()) {
        return std::nullopt;
    }
    if (const std::optional<Issue> issue = IssueFor(record->second)) {
        return issue;
    }
    record->second = {State::Moved, location};
    return std::nullopt;
}

void MoveStateTracker::Assign(const Identity identity, const SourceLocation location) {
    const auto record = records.find(identity);
    if (record != records.end()) {
        record->second = {State::Initialized, location};
    }
}

const MoveStateTracker::Record *MoveStateTracker::TryGet(const Identity identity) const {
    const auto record = records.find(identity);
    return record == records.end() ? nullptr : &record->second;
}

MoveStateTracker::Snapshot MoveStateTracker::Save() const {
    Snapshot snapshot;
    snapshot.scopeLengths.reserve(scopes.size());
    snapshot.entries.reserve(records.size());
    for (const auto &scope : scopes) {
        snapshot.scopeLengths.push_back(scope.size());
        for (const Identity identity : scope) {
            if (const auto record = records.find(identity); record != records.end()) {
                snapshot.entries.push_back({identity, record->second});
            }
        }
    }
    return snapshot;
}

void MoveStateTracker::Restore(const Snapshot &snapshot) {
    records.clear();
    scopes.clear();
    scopes.reserve(snapshot.scopeLengths.size());

    std::size_t entryIndex = 0;
    for (const std::size_t scopeLength : snapshot.scopeLengths) {
        auto &scope = scopes.emplace_back();
        scope.reserve(scopeLength);
        for (std::size_t index = 0; index < scopeLength && entryIndex < snapshot.entries.size(); ++index) {
            const SnapshotEntry &entry = snapshot.entries[entryIndex++];
            scope.push_back(entry.identity);
            records.insert_or_assign(entry.identity, entry.record);
        }
    }
    if (scopes.empty()) {
        scopes.emplace_back();
    }
}

MoveStateTracker::Snapshot MoveStateTracker::Merge(const std::span<const Snapshot> snapshots) {
    if (snapshots.empty()) {
        return {};
    }

    Snapshot result = snapshots.front();
    for (SnapshotEntry &entry : result.entries) {
        for (const Snapshot &snapshot : snapshots.subspan(1)) {
            const auto other = std::ranges::find(snapshot.entries, entry.identity, &SnapshotEntry::identity);
            if (other == snapshot.entries.end()) {
                continue;
            }
            const State merged = MergeStates(entry.record.state, other->record.state);
            if (entry.record.state == State::Initialized && other->record.state != State::Initialized) {
                entry.record.previousTransition = other->record.previousTransition;
            }
            entry.record.state = merged;
        }
    }
    return result;
}

MoveStateTracker::Snapshot MoveStateTracker::Project(const Snapshot &source, const Snapshot &shape) {
    Snapshot result = shape;
    for (SnapshotEntry &entry : result.entries) {
        const auto record = std::ranges::find(source.entries, entry.identity, &SnapshotEntry::identity);
        if (record != source.entries.end()) {
            entry.record = record->record;
        }
    }
    return result;
}

std::size_t MoveStateTracker::IdentityHash::operator()(const Identity identity) const noexcept {
    const std::size_t address = std::hash<const void *>{}(identity.address);
    const std::size_t kind = static_cast<std::size_t>(identity.kind);
    return address ^ (kind + 0x9e3779b9U + (address << 6U) + (address >> 2U));
}

std::optional<MoveStateTracker::Issue> MoveStateTracker::IssueFor(const Record &record) {
    if (record.state == State::Uninitialized) {
        return Issue{IssueKind::Uninitialized, record.previousTransition};
    }
    if (record.state == State::Moved) {
        return Issue{IssueKind::Moved, record.previousTransition};
    }
    if (record.state == State::MaybeUninitialized) {
        return Issue{IssueKind::PossiblyUninitialized, record.previousTransition};
    }
    if (record.state == State::MaybeMoved) {
        return Issue{IssueKind::PossiblyMoved, record.previousTransition};
    }
    if (record.state == State::MaybeUnavailable) {
        return Issue{IssueKind::PossiblyUnavailable, record.previousTransition};
    }
    return std::nullopt;
}

MoveStateTracker::State MoveStateTracker::MergeStates(const State left, const State right) {
    if (left == right) {
        return left;
    }
    if (left == State::MaybeUnavailable || right == State::MaybeUnavailable) {
        return State::MaybeUnavailable;
    }

    const auto has = [left, right](const State state) { return left == state || right == state; };
    const bool mayMove = has(State::Moved) || has(State::MaybeMoved);
    const bool mayBeUninitialized = has(State::Uninitialized) || has(State::MaybeUninitialized);
    if (mayMove && mayBeUninitialized) {
        return State::MaybeUnavailable;
    }
    return mayMove ? State::MaybeMoved : State::MaybeUninitialized;
}
} // namespace Rux::SemanticDetail

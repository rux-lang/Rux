#include "Semantic/Analysis/MoveStateTracker.h"

#include <algorithm>
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

std::pair<std::size_t, std::size_t> MoveStateTracker::Mark() const noexcept {
    return {scopes.size(), scopes.empty() ? 0 : scopes.back().size()};
}

void MoveStateTracker::DiscardTemporariesSince(const std::pair<std::size_t, std::size_t> mark) {
    // A statement leaves the scopes as it found them; one that did not is left alone. Outside every function the
    // tracker may have no scope yet.
    if (scopes.size() != mark.first || scopes.empty() || scopes.back().size() <= mark.second) {
        return;
    }
    auto &scope = scopes.back();
    const auto kept = std::remove_if(scope.begin() + static_cast<std::ptrdiff_t>(mark.second), scope.end(),
                                     [&](const Identity identity) {
                                         if (identity.kind != IdentityKind::Temporary) {
                                             return false;
                                         }
                                         records.erase(identity);
                                         return true;
                                     });
    scope.erase(kept, scope.end());
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

void MoveStateTracker::AssignPart(const Identity identity, std::string path) {
    const auto record = records.find(identity);
    if (record == records.end() || record->second.state == State::Initialized ||
        PartsCover(record->second.writtenParts, path)) {
        return;
    }
    // A part the new one contains is now written as part of it.
    std::erase_if(record->second.writtenParts, [&](const std::string &written) {
        return written.starts_with(path) && written.size() > path.size() && written[path.size()] == '.';
    });
    record->second.writtenParts.push_back(std::move(path));
}

std::optional<MoveStateTracker::Issue> MoveStateTracker::ReadPart(const Identity identity,
                                                                  const std::string_view path) const {
    const Record *record = TryGet(identity);
    if (!record || PartsCover(record->writtenParts, path)) {
        return std::nullopt;
    }
    return IssueFor(*record);
}

bool MoveStateTracker::PartsCover(const std::span<const std::string> parts, const std::string_view path) {
    return std::ranges::any_of(parts, [&](const std::string &written) {
        return path == written ||
               (path.starts_with(written) && path.size() > written.size() && path[written.size()] == '.');
    });
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

    // Every expression a function checks is tracked, so a snapshot grows with the function, and a function has a merge
    // for each branch: finding entries by a scan would make checking it cubic in its length.
    Snapshot result = snapshots.front();
    for (const Snapshot &snapshot : snapshots.subspan(1)) {
        const auto index = IndexEntries(snapshot);
        for (SnapshotEntry &entry : result.entries) {
            const auto found = index.find(entry.identity);
            if (found == index.end()) {
                continue;
            }
            const SnapshotEntry *other = found->second;
            if (entry.record.state == State::Initialized && other->record.state == State::Initialized) {
                // Both paths hold the whole value, so nothing about it changes.
                continue;
            }
            const State merged = MergeStates(entry.record.state, other->record.state);
            std::vector<std::string> parts = MergeParts(entry.record, other->record);
            if (entry.record.state == State::Initialized && other->record.state != State::Initialized) {
                entry.record.previousTransition = other->record.previousTransition;
            }
            entry.record.state = merged;
            entry.record.writtenParts = merged == State::Initialized ? std::vector<std::string>{} : std::move(parts);
        }
    }
    return result;
}

MoveStateTracker::Snapshot MoveStateTracker::Project(const Snapshot &source, const Snapshot &shape) {
    Snapshot result = shape;
    const auto index = IndexEntries(source);
    for (SnapshotEntry &entry : result.entries) {
        if (const auto record = index.find(entry.identity); record != index.end()) {
            entry.record = record->second->record;
        }
    }
    return result;
}

std::unordered_map<MoveStateTracker::Identity, const MoveStateTracker::SnapshotEntry *, MoveStateTracker::IdentityHash>
MoveStateTracker::IndexEntries(const Snapshot &snapshot) {
    std::unordered_map<Identity, const SnapshotEntry *, IdentityHash> index;
    index.reserve(snapshot.entries.size());
    for (const SnapshotEntry &entry : snapshot.entries) {
        index.try_emplace(entry.identity, &entry);
    }
    return index;
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

/// The parts written on every path: a whole value holds them all, so it keeps what the other path wrote, and otherwise
/// a part survives where the other path wrote it or a part containing it.
std::vector<std::string> MoveStateTracker::MergeParts(const Record &left, const Record &right) {
    if (left.state == State::Initialized) {
        return right.writtenParts;
    }
    if (right.state == State::Initialized) {
        return left.writtenParts;
    }
    std::vector<std::string> merged;
    for (const std::string &part : left.writtenParts) {
        if (PartsCover(right.writtenParts, part)) {
            merged.push_back(part);
        }
    }
    for (const std::string &part : right.writtenParts) {
        if (PartsCover(left.writtenParts, part) && !PartsCover(merged, part)) {
            merged.push_back(part);
        }
    }
    return merged;
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

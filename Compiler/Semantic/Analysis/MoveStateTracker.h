#pragma once

#include "SourceModel/SourceLocation.h"

#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Rux::SemanticDetail {
/// Tracks initialization and consumption of storage identities while semantic analysis walks one straight-line path.
class MoveStateTracker {
public:
    enum class IdentityKind {
        Local,
        Temporary,
    };

    struct Identity {
        IdentityKind kind = IdentityKind::Local;
        const void *address = nullptr;

        bool operator==(const Identity &) const = default;
    };

    enum class State {
        Uninitialized,
        Initialized,
        Moved,
        MaybeUninitialized,
        MaybeMoved,
        MaybeUnavailable,
    };

    enum class IssueKind {
        Uninitialized,
        Moved,
        PossiblyUninitialized,
        PossiblyMoved,
        PossiblyUnavailable,
    };

    struct Issue {
        IssueKind kind;
        SourceLocation previousTransition;
    };

    struct Record {
        State state;
        SourceLocation previousTransition;
    };

    struct SnapshotEntry {
        Identity identity;
        Record record;
    };

    struct Snapshot {
        std::vector<SnapshotEntry> entries;
        std::vector<std::size_t> scopeLengths;
    };

    [[nodiscard]] static Identity Local(const void *address) noexcept;
    [[nodiscard]] static Identity Temporary(const void *address) noexcept;

    void Reset();
    void BeginScope();
    void EndScope();
    void Declare(Identity identity, State state, SourceLocation location);
    [[nodiscard]] std::optional<Issue> Read(Identity identity) const;
    [[nodiscard]] std::optional<Issue> Move(Identity identity, SourceLocation location);
    void Assign(Identity identity, SourceLocation location);
    [[nodiscard]] const Record *TryGet(Identity identity) const;
    [[nodiscard]] Snapshot Save() const;
    void Restore(const Snapshot &snapshot);
    [[nodiscard]] static Snapshot Merge(std::span<const Snapshot> snapshots);
    [[nodiscard]] static Snapshot Project(const Snapshot &source, const Snapshot &shape);

private:
    struct IdentityHash {
        [[nodiscard]] std::size_t operator()(Identity identity) const noexcept;
    };

    std::unordered_map<Identity, Record, IdentityHash> records;
    std::vector<std::vector<Identity>> scopes;

    [[nodiscard]] static std::optional<Issue> IssueFor(const Record &record);
    [[nodiscard]] static State MergeStates(State left, State right);
};
} // namespace Rux::SemanticDetail

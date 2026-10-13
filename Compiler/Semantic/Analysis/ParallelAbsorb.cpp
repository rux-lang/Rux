// Merging what several analysis contexts recorded while each checked the bodies of its own range of modules.
//
// Every context ran the same Begin phases, so each holds the same declarations, signatures and prechecked
// declarations; what differs is what its bodies recorded. Each worker first separates its own records from what it
// inherited, comparing against a context that ran only the Begin phases. The records are then merged in the order of
// the workers' module ranges, each kind the way one context checking every module in order would have left it: a node
// fact recorded by a later module replaces an earlier one, a cache keeps its first entry, and per-function lists and
// the instantiation queue keep module order.

#include "Semantic/Analysis/AnalysisContext.h"

#include <concepts>
#include <functional>
#include <iterator>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
template <typename T>
concept EqualityComparable = requires(const T &left, const T &right) {
    { left == right } -> std::convertible_to<bool>;
};

/// The entries of `source` that are its own: absent from `shared`, or, where values compare, different from it.
template <typename Map>
Map OwnEntries(Map &source, const Map &shared) {
    Map own;
    for (auto &[key, value] : source) {
        if (const auto inherited = shared.find(key); inherited != shared.end()) {
            if constexpr (EqualityComparable<typename Map::mapped_type>) {
                if (inherited->second == value)
                    continue;
            }
            else {
                continue;
            }
        }
        own.emplace(key, std::move(value));
    }
    return own;
}

template <typename Set>
Set OwnElements(const Set &source, const Set &shared) {
    Set own;
    for (const auto &element : source)
        if (!shared.contains(element))
            own.insert(element);
    return own;
}

/// What each per-function list gained beyond its length in `shared`.
template <typename Map>
Map OwnSuffixes(Map &source, const Map &shared) {
    Map own;
    for (auto &[function, entries] : source) {
        const auto inherited = shared.find(function);
        const std::size_t skip = inherited == shared.end() ? 0 : inherited->second.size();
        if (entries.size() <= skip)
            continue;
        auto &suffix = own[function];
        suffix.insert(suffix.end(), std::make_move_iterator(entries.begin() + static_cast<std::ptrdiff_t>(skip)),
                      std::make_move_iterator(entries.end()));
    }
    return own;
}

template <typename Container, typename Records, typename Member>
std::size_t TotalSize(Records &records, Member member) {
    std::size_t total = 0;
    for (auto &record : records)
        total += std::invoke(member, record).size();
    return total;
}

/// A later module's record replaces an earlier one.
template <typename Map, typename Records, typename Member>
void MergeLaterWins(Map &target, Records &records, Member member) {
    target.reserve(target.size() + TotalSize<Map>(records, member));
    for (auto &record : records)
        for (auto &[key, value] : std::invoke(member, record))
            target.insert_or_assign(key, std::move(value));
}

/// A cache keeps whatever was entered first, and earlier modules enter first.
template <typename Map, typename Records, typename Member>
void MergeFirstWins(Map &target, Records &records, Member member) {
    target.reserve(target.size() + TotalSize<Map>(records, member));
    for (auto &record : records)
        for (auto &[key, value] : std::invoke(member, record))
            target.try_emplace(key, std::move(value));
}

template <typename Set, typename Records, typename Member>
void MergeSet(Set &target, Records &records, Member member) {
    target.reserve(target.size() + TotalSize<Set>(records, member));
    for (auto &record : records) {
        const auto &elements = std::invoke(member, record);
        target.insert(elements.begin(), elements.end());
    }
}

template <typename Map, typename Records, typename Member>
void MergeAppended(Map &target, Records &records, Member member) {
    for (auto &record : records) {
        for (auto &[function, entries] : std::invoke(member, record)) {
            auto &merged = target[function];
            merged.insert(merged.end(), std::make_move_iterator(entries.begin()),
                          std::make_move_iterator(entries.end()));
        }
    }
}
} // namespace

AnalysisContext::BodyRecords AnalysisContext::ExtractBodyRecords(const AnalysisContext &shared) {
    BodyRecords records;
    SemanticFacts &own = records.facts;
    SemanticFacts &mine = outputFacts;
    const SemanticFacts &base = shared.outputFacts;
    own.associatedConstants = OwnEntries(mine.associatedConstants, base.associatedConstants);
    own.constantReferences = OwnEntries(mine.constantReferences, base.constantReferences);
    own.globalReferences = OwnEntries(mine.globalReferences, base.globalReferences);
    own.evaluatedAssociatedConstants = OwnEntries(mine.evaluatedAssociatedConstants, base.evaluatedAssociatedConstants);
    own.expressionTypes = OwnEntries(mine.expressionTypes, base.expressionTypes);
    own.borrowedScalarReads = OwnElements(mine.borrowedScalarReads, base.borrowedScalarReads);
    own.referenceWrites = OwnElements(mine.referenceWrites, base.referenceWrites);
    own.placeReplacements = OwnEntries(mine.placeReplacements, base.placeReplacements);
    own.nativeConversions = OwnEntries(mine.nativeConversions, base.nativeConversions);
    own.typeNodeTypes = OwnEntries(mine.typeNodeTypes, base.typeNodeTypes);
    own.patternTypes = OwnEntries(mine.patternTypes, base.patternTypes);
    own.casePatterns = OwnEntries(mine.casePatterns, base.casePatterns);
    own.typedPatternTypes = OwnEntries(mine.typedPatternTypes, base.typedPatternTypes);
    own.sumMemberPatterns = OwnEntries(mine.sumMemberPatterns, base.sumMemberPatterns);
    own.patternBindingModes = OwnEntries(mine.patternBindingModes, base.patternBindingModes);
    own.variantEqualities = OwnEntries(mine.variantEqualities, base.variantEqualities);
    own.variantEqualityPlans = OwnEntries(mine.variantEqualityPlans, base.variantEqualityPlans);
    own.aggregateEqualities = OwnEntries(mine.aggregateEqualities, base.aggregateEqualities);
    own.aggregateEqualityOperandTypes =
        OwnEntries(mine.aggregateEqualityOperandTypes, base.aggregateEqualityOperandTypes);
    own.aggregateEqualityPlans = OwnEntries(mine.aggregateEqualityPlans, base.aggregateEqualityPlans);
    own.valueConsumptions = OwnEntries(mine.valueConsumptions, base.valueConsumptions);
    own.valueCopies = OwnEntries(mine.valueCopies, base.valueCopies);
    own.callableBindings = OwnEntries(mine.callableBindings, base.callableBindings);
    own.functionReferenceBindings = OwnEntries(mine.functionReferenceBindings, base.functionReferenceBindings);
    own.defaultConstructors = OwnEntries(mine.defaultConstructors, base.defaultConstructors);
    own.symbolIdentities = OwnEntries(mine.symbolIdentities, base.symbolIdentities);
    own.vtableIdentities = OwnEntries(mine.vtableIdentities, base.vtableIdentities);
    own.constraintWitnesses = OwnEntries(mine.constraintWitnesses, base.constraintWitnesses);
    own.propagations = OwnEntries(mine.propagations, base.propagations);
    own.mappedPropagations = OwnEntries(mine.mappedPropagations, base.mappedPropagations);
    own.coalescings = OwnEntries(mine.coalescings, base.coalescings);
    own.indexOperators = OwnEntries(mine.indexOperators, base.indexOperators);
    own.indexAssignments = OwnEntries(mine.indexAssignments, base.indexAssignments);
    own.iterations = OwnEntries(mine.iterations, base.iterations);
    own.exhaustiveMatchExpressions = OwnElements(mine.exhaustiveMatchExpressions, base.exhaustiveMatchExpressions);
    own.exhaustiveMatchStatements = OwnElements(mine.exhaustiveMatchStatements, base.exhaustiveMatchStatements);
    own.ownedMatchSubjects = OwnElements(mine.ownedMatchSubjects, base.ownedMatchSubjects);
    own.typeLayouts = OwnEntries(mine.typeLayouts, base.typeLayouts);
    own.typeProperties = OwnEntries(mine.typeProperties, base.typeProperties);
    own.dropGluePlans = OwnEntries(mine.dropGluePlans, base.dropGluePlans);
    own.typeQueryValues = OwnEntries(mine.typeQueryValues, base.typeQueryValues);

    records.checkedConstantTypes = OwnEntries(checkedConstantTypes, shared.checkedConstantTypes);
    records.checkedGlobalTypes = OwnEntries(checkedGlobalTypes, shared.checkedGlobalTypes);
    records.localConstants = OwnElements(localConstants, shared.localConstants);
    for (const ConstDecl *constant : records.localConstants) {
        if (const auto info = declarationInfos.find(constant); info != declarationInfos.end())
            records.localConstantInfos.emplace_back(constant, info->second);
    }
    records.expectedExpressionTypes = OwnEntries(expectedExpressionTypes, shared.expectedExpressionTypes);
    records.consumedConstructorOperands = OwnElements(consumedConstructorOperands, shared.consumedConstructorOperands);
    records.nativeMatchExhaustive = OwnEntries(nativeMatchExhaustive, shared.nativeMatchExhaustive);
    records.tupleMatchMissing = OwnEntries(tupleMatchMissing, shared.tupleMatchMissing);
    records.negatedIntegerLiterals = OwnElements(negatedIntegerLiterals, shared.negatedIntegerLiterals);
    for (auto &[function, types] : genericBodyTypes) {
        const auto inherited = shared.genericBodyTypes.find(function);
        auto ownTypes =
            inherited == shared.genericBodyTypes.end() ? std::move(types) : OwnEntries(types, inherited->second);
        if (!ownTypes.empty())
            records.genericBodyTypes.emplace(function, std::move(ownTypes));
    }
    records.deferredUnaryChecks = OwnSuffixes(deferredUnaryChecks, shared.deferredUnaryChecks);
    records.deferredBinaryChecks = OwnSuffixes(deferredBinaryChecks, shared.deferredBinaryChecks);
    records.deferredCastChecks = OwnSuffixes(deferredCastChecks, shared.deferredCastChecks);
    records.deferredAtomicChecks = OwnSuffixes(deferredAtomicChecks, shared.deferredAtomicChecks);
    records.deferredScalarReads = OwnSuffixes(deferredScalarReads, shared.deferredScalarReads);
    records.deferredOutcomeChecks = OwnSuffixes(deferredOutcomeChecks, shared.deferredOutcomeChecks);
    records.deferredConsumptions = OwnSuffixes(deferredConsumptions, shared.deferredConsumptions);
    records.deferredMatchChecks = OwnSuffixes(deferredMatchChecks, shared.deferredMatchChecks);
    records.deferredNativeTypes = OwnSuffixes(deferredNativeTypes, shared.deferredNativeTypes);
    records.deferredMembershipChecks = OwnSuffixes(deferredMembershipChecks, shared.deferredMembershipChecks);
    records.deferredGenericCalls = OwnSuffixes(deferredGenericCalls, shared.deferredGenericCalls);
    records.deferredEnumInstantiations = OwnSuffixes(deferredEnumInstantiations, shared.deferredEnumInstantiations);

    const auto at = [this](const std::size_t index) {
        return pendingGenericInstantiations.begin() + static_cast<std::ptrdiff_t>(index);
    };
    records.dependencyInstantiations.assign(at(instantiationsAfterDependencyBegin),
                                            at(instantiationsAfterDependencyBodies));
    records.userInstantiations.assign(at(instantiationsAfterUserBegin), pendingGenericInstantiations.end());
    return records;
}

void AnalysisContext::AbsorbBodyRecords(std::vector<BodyRecords> &records, Parallel::WorkerPool *pool) {
    using R = BodyRecords;
    SemanticFacts &facts = outputFacts;
    // Each task merges one kind of record from every worker, so the tasks share nothing.
    const std::vector<std::function<void()>> merges = {
        [&] {
            MergeLaterWins(facts.associatedConstants, records,
                           [](R &r) -> auto & { return r.facts.associatedConstants; });
        },
        [&] {
            MergeLaterWins(facts.constantReferences, records,
                           [](R &r) -> auto & { return r.facts.constantReferences; });
        },
        [&] {
            MergeLaterWins(facts.globalReferences, records, [](R &r) -> auto & { return r.facts.globalReferences; });
        },
        [&] {
            MergeFirstWins(facts.evaluatedAssociatedConstants, records,
                           [](R &r) -> auto & { return r.facts.evaluatedAssociatedConstants; });
        },
        [&] { MergeLaterWins(facts.expressionTypes, records, [](R &r) -> auto & { return r.facts.expressionTypes; }); },
        [&] {
            MergeSet(facts.borrowedScalarReads, records, [](R &r) -> auto & { return r.facts.borrowedScalarReads; });
        },
        [&] { MergeSet(facts.referenceWrites, records, [](R &r) -> auto & { return r.facts.referenceWrites; }); },
        [&] {
            MergeLaterWins(facts.placeReplacements, records, [](R &r) -> auto & { return r.facts.placeReplacements; });
        },
        [&] {
            MergeLaterWins(facts.nativeConversions, records, [](R &r) -> auto & { return r.facts.nativeConversions; });
        },
        [&] { MergeLaterWins(facts.typeNodeTypes, records, [](R &r) -> auto & { return r.facts.typeNodeTypes; }); },
        [&] { MergeLaterWins(facts.patternTypes, records, [](R &r) -> auto & { return r.facts.patternTypes; }); },
        [&] { MergeLaterWins(facts.casePatterns, records, [](R &r) -> auto & { return r.facts.casePatterns; }); },
        [&] {
            MergeLaterWins(facts.typedPatternTypes, records, [](R &r) -> auto & { return r.facts.typedPatternTypes; });
        },
        [&] {
            MergeLaterWins(facts.sumMemberPatterns, records, [](R &r) -> auto & { return r.facts.sumMemberPatterns; });
        },
        [&] {
            MergeLaterWins(facts.patternBindingModes, records,
                           [](R &r) -> auto & { return r.facts.patternBindingModes; });
        },
        [&] {
            MergeLaterWins(facts.variantEqualities, records, [](R &r) -> auto & { return r.facts.variantEqualities; });
        },
        [&] {
            MergeFirstWins(facts.variantEqualityPlans, records,
                           [](R &r) -> auto & { return r.facts.variantEqualityPlans; });
        },
        [&] {
            MergeLaterWins(facts.aggregateEqualities, records,
                           [](R &r) -> auto & { return r.facts.aggregateEqualities; });
        },
        [&] {
            MergeLaterWins(facts.aggregateEqualityOperandTypes, records,
                           [](R &r) -> auto & { return r.facts.aggregateEqualityOperandTypes; });
        },
        [&] {
            MergeLaterWins(facts.aggregateEqualityPlans, records,
                           [](R &r) -> auto & { return r.facts.aggregateEqualityPlans; });
        },
        [&] {
            MergeLaterWins(facts.valueConsumptions, records, [](R &r) -> auto & { return r.facts.valueConsumptions; });
        },
        [&] { MergeLaterWins(facts.valueCopies, records, [](R &r) -> auto & { return r.facts.valueCopies; }); },
        [&] {
            MergeLaterWins(facts.callableBindings, records, [](R &r) -> auto & { return r.facts.callableBindings; });
        },
        [&] {
            MergeLaterWins(facts.functionReferenceBindings, records,
                           [](R &r) -> auto & { return r.facts.functionReferenceBindings; });
        },
        [&] {
            MergeLaterWins(facts.defaultConstructors, records,
                           [](R &r) -> auto & { return r.facts.defaultConstructors; });
        },
        [&] {
            MergeLaterWins(facts.symbolIdentities, records, [](R &r) -> auto & { return r.facts.symbolIdentities; });
        },
        [&] {
            MergeLaterWins(facts.vtableIdentities, records, [](R &r) -> auto & { return r.facts.vtableIdentities; });
        },
        [&] {
            MergeLaterWins(facts.constraintWitnesses, records,
                           [](R &r) -> auto & { return r.facts.constraintWitnesses; });
        },
        [&] { MergeLaterWins(facts.propagations, records, [](R &r) -> auto & { return r.facts.propagations; }); },
        [&] {
            MergeLaterWins(facts.mappedPropagations, records,
                           [](R &r) -> auto & { return r.facts.mappedPropagations; });
        },
        [&] { MergeLaterWins(facts.coalescings, records, [](R &r) -> auto & { return r.facts.coalescings; }); },
        [&] { MergeLaterWins(facts.indexOperators, records, [](R &r) -> auto & { return r.facts.indexOperators; }); },
        [&] {
            MergeLaterWins(facts.indexAssignments, records, [](R &r) -> auto & { return r.facts.indexAssignments; });
        },
        [&] { MergeLaterWins(facts.iterations, records, [](R &r) -> auto & { return r.facts.iterations; }); },
        [&] {
            MergeSet(facts.exhaustiveMatchExpressions, records,
                     [](R &r) -> auto & { return r.facts.exhaustiveMatchExpressions; });
        },
        [&] {
            MergeSet(facts.exhaustiveMatchStatements, records,
                     [](R &r) -> auto & { return r.facts.exhaustiveMatchStatements; });
        },
        [&] { MergeSet(facts.ownedMatchSubjects, records, [](R &r) -> auto & { return r.facts.ownedMatchSubjects; }); },
        [&] { MergeFirstWins(facts.typeLayouts, records, [](R &r) -> auto & { return r.facts.typeLayouts; }); },
        [&] { MergeFirstWins(facts.typeProperties, records, [](R &r) -> auto & { return r.facts.typeProperties; }); },
        [&] { MergeLaterWins(facts.dropGluePlans, records, [](R &r) -> auto & { return r.facts.dropGluePlans; }); },
        [&] { MergeLaterWins(facts.typeQueryValues, records, [](R &r) -> auto & { return r.facts.typeQueryValues; }); },
        [&] { MergeFirstWins(checkedConstantTypes, records, [](R &r) -> auto & { return r.checkedConstantTypes; }); },
        [&] { MergeFirstWins(checkedGlobalTypes, records, [](R &r) -> auto & { return r.checkedGlobalTypes; }); },
        [&] { MergeSet(localConstants, records, [](R &r) -> auto & { return r.localConstants; }); },
        [&] {
            for (auto &record : records)
                for (auto &[declaration, info] : record.localConstantInfos)
                    programIndex.AdoptDeclarationInfo(*declaration, std::move(info));
        },
        [&] {
            MergeLaterWins(expectedExpressionTypes, records, [](R &r) -> auto & { return r.expectedExpressionTypes; });
        },
        [&] {
            MergeSet(consumedConstructorOperands, records,
                     [](R &r) -> auto & { return r.consumedConstructorOperands; });
        },
        [&] { MergeLaterWins(nativeMatchExhaustive, records, [](R &r) -> auto & { return r.nativeMatchExhaustive; }); },
        [&] { MergeLaterWins(tupleMatchMissing, records, [](R &r) -> auto & { return r.tupleMatchMissing; }); },
        [&] { MergeSet(negatedIntegerLiterals, records, [](R &r) -> auto & { return r.negatedIntegerLiterals; }); },
        [&] {
            for (auto &record : records)
                for (auto &[function, types] : record.genericBodyTypes) {
                    auto &merged = genericBodyTypes[function];
                    for (auto &[spelling, type] : types)
                        merged.try_emplace(spelling, std::move(type));
                }
        },
        [&] { MergeAppended(deferredUnaryChecks, records, [](R &r) -> auto & { return r.deferredUnaryChecks; }); },
        [&] { MergeAppended(deferredBinaryChecks, records, [](R &r) -> auto & { return r.deferredBinaryChecks; }); },
        [&] { MergeAppended(deferredCastChecks, records, [](R &r) -> auto & { return r.deferredCastChecks; }); },
        [&] { MergeAppended(deferredAtomicChecks, records, [](R &r) -> auto & { return r.deferredAtomicChecks; }); },
        [&] { MergeAppended(deferredScalarReads, records, [](R &r) -> auto & { return r.deferredScalarReads; }); },
        [&] { MergeAppended(deferredOutcomeChecks, records, [](R &r) -> auto & { return r.deferredOutcomeChecks; }); },
        [&] { MergeAppended(deferredConsumptions, records, [](R &r) -> auto & { return r.deferredConsumptions; }); },
        [&] { MergeAppended(deferredMatchChecks, records, [](R &r) -> auto & { return r.deferredMatchChecks; }); },
        [&] { MergeAppended(deferredNativeTypes, records, [](R &r) -> auto & { return r.deferredNativeTypes; }); },
        [&] {
            MergeAppended(deferredMembershipChecks, records, [](R &r) -> auto & { return r.deferredMembershipChecks; });
        },
        [&] { MergeAppended(deferredGenericCalls, records, [](R &r) -> auto & { return r.deferredGenericCalls; }); },
        [&] {
            MergeAppended(deferredEnumInstantiations, records,
                          [](R &r) -> auto & { return r.deferredEnumInstantiations; });
        },
        [&] {
            // This context checked no body, so its queue holds only what the Begin phases queued; the workers' body
            // entries go between them as one context would have queued them.
            std::vector<PendingGenericInstantiation> merged;
            const auto dependencyEnd =
                pendingGenericInstantiations.begin() + static_cast<std::ptrdiff_t>(instantiationsAfterDependencyBegin);
            merged.insert(merged.end(), pendingGenericInstantiations.begin(), dependencyEnd);
            for (auto &record : records)
                merged.insert(merged.end(), record.dependencyInstantiations.begin(),
                              record.dependencyInstantiations.end());
            merged.insert(merged.end(), dependencyEnd, pendingGenericInstantiations.end());
            for (auto &record : records)
                merged.insert(merged.end(), record.userInstantiations.begin(), record.userInstantiations.end());
            pendingGenericInstantiations = std::move(merged);
        },
    };
    if (pool != nullptr) {
        pool->ForEach(merges.size(), [&](const std::size_t index) { merges[index](); });
    }
    else {
        for (const auto &merge : merges)
            merge();
    }
}
} // namespace Rux::SemanticDetail

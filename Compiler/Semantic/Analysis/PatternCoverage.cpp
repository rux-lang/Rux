// Native pattern semantics: what typed, presence, `none`, and native case patterns select from their subject, which
// sum member a nominal pattern addresses, whether a binding pattern introduces a free name, and whether a match over a
// native subject covers every value without an arm that can never run.
//
// Coverage is decided by the usefulness algorithm over constructors. A sum has one constructor per member, an optional
// its absence and its presence, a fallible its two channels, a tuple and the unit one each, `bool` its two values, and
// an enum or variant one per case; every other type is open, so only a pattern that matches anything completes it. An
// arm is unreachable exactly when the unguarded arms before it already match every value it could.

#include "Semantic/Analysis/AnalysisContext.h"
#include "Types/NativeConversion.h"
#include "Types/NativeSelection.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
/// One pattern as the coverage algorithm sees it: anything, one constructor of its type with a pattern per field, a
/// value of an open type that only an equal value repeats, or a choice among several of these. A choice among nothing
/// matches nothing.
struct Space {
    enum class Kind {
        Wildcard,
        Constructor,
        Opaque,
        Or,
    };

    Kind kind = Kind::Wildcard;
    std::size_t constructor = 0;
    std::vector<Space> fields;
    std::string key;
    std::vector<Space> alternatives;

    static Space Of(const std::size_t constructor, std::vector<Space> fields) {
        Space space;
        space.kind = Kind::Constructor;
        space.constructor = constructor;
        space.fields = std::move(fields);
        return space;
    }

    static Space Value(std::string key) {
        Space space;
        space.kind = Kind::Opaque;
        space.key = std::move(key);
        return space;
    }

    static Space Choice(std::vector<Space> alternatives) {
        Space space;
        space.kind = Kind::Or;
        space.alternatives = std::move(alternatives);
        return space;
    }
};

using Row = std::vector<Space>;

/// A value no arm matches, built while the algorithm searches, for the diagnostic that names it.
struct Witness {
    Space::Kind kind = Space::Kind::Wildcard;
    std::size_t constructor = 0;
    std::vector<Witness> fields;
    std::string key;
};

using WitnessRow = std::vector<Witness>;

/// The heads of `space` with every choice spread out into its alternatives.
void SpreadChoices(const Space &space, std::vector<const Space *> &heads) {
    if (space.kind == Space::Kind::Or) {
        for (const Space &alternative : space.alternatives) {
            SpreadChoices(alternative, heads);
        }
        return;
    }
    heads.push_back(&space);
}

/// Decides usefulness for one match. Constructor lists are asked for once per type and kept.
class CoverageSolver {
public:
    using ConstructorSource = std::function<std::optional<std::vector<std::vector<TypeRef>>>(const TypeRef &)>;

    explicit CoverageSolver(ConstructorSource source)
        : constructorSource(std::move(source)) {
    }

    /// The values `query` matches that no row does, at most `limit` of them; empty when `query` adds nothing.
    std::vector<WitnessRow> Useful(const std::vector<Row> &rows, const Row &query, const std::vector<TypeRef> &types,
                                   const std::size_t limit) {
        if (query.empty()) {
            return rows.empty() ? std::vector<WitnessRow>{WitnessRow{}} : std::vector<WitnessRow>{};
        }
        const Space &head = query.front();
        const Row rest(query.begin() + 1, query.end());
        const std::vector<TypeRef> restTypes(types.begin() + 1, types.end());
        std::vector<WitnessRow> witnesses;

        if (head.kind == Space::Kind::Or) {
            for (const Space &alternative : head.alternatives) {
                Row expanded{alternative};
                expanded.insert(expanded.end(), rest.begin(), rest.end());
                for (WitnessRow &witness : Useful(rows, expanded, types, limit - witnesses.size())) {
                    witnesses.push_back(std::move(witness));
                }
                if (witnesses.size() >= limit) {
                    break;
                }
            }
            return witnesses;
        }
        if (head.kind == Space::Kind::Constructor) {
            return UsefulConstructor(rows, head.constructor, head.fields, rest, types, limit);
        }
        if (head.kind == Space::Kind::Opaque) {
            std::vector<Row> specialized;
            for (const Row &row : rows) {
                std::vector<const Space *> heads;
                SpreadChoices(row.front(), heads);
                for (const Space *candidate : heads) {
                    if (candidate->kind == Space::Kind::Wildcard ||
                        (candidate->kind == Space::Kind::Opaque && candidate->key == head.key)) {
                        specialized.emplace_back(row.begin() + 1, row.end());
                    }
                }
            }
            for (WitnessRow &witness : Useful(specialized, rest, restTypes, limit)) {
                witness.insert(witness.begin(), Witness{Space::Kind::Opaque, 0, {}, head.key});
                witnesses.push_back(std::move(witness));
            }
            return witnesses;
        }

        const std::optional<std::vector<std::vector<TypeRef>>> &constructors = ConstructorsOf(types.front());
        std::set<std::size_t> used;
        for (const Row &row : rows) {
            std::vector<const Space *> heads;
            SpreadChoices(row.front(), heads);
            for (const Space *candidate : heads) {
                if (candidate->kind == Space::Kind::Constructor) {
                    used.insert(candidate->constructor);
                }
            }
        }
        if (constructors && !constructors->empty() && used.size() == constructors->size()) {
            for (std::size_t constructor = 0; constructor < constructors->size(); ++constructor) {
                const std::vector<Space> fields((*constructors)[constructor].size());
                for (WitnessRow &witness :
                     UsefulConstructor(rows, constructor, fields, rest, types, limit - witnesses.size())) {
                    witnesses.push_back(std::move(witness));
                }
                if (witnesses.size() >= limit) {
                    break;
                }
            }
            return witnesses;
        }

        std::vector<Row> defaults;
        for (const Row &row : rows) {
            std::vector<const Space *> heads;
            SpreadChoices(row.front(), heads);
            if (std::ranges::any_of(heads,
                                    [](const Space *candidate) { return candidate->kind == Space::Kind::Wildcard; })) {
                defaults.emplace_back(row.begin() + 1, row.end());
            }
        }
        const std::vector<WitnessRow> tails = Useful(defaults, rest, restTypes, limit);
        if (tails.empty()) {
            return witnesses;
        }
        std::vector<Witness> missingHeads;
        if (constructors && !constructors->empty()) {
            for (std::size_t constructor = 0; constructor < constructors->size(); ++constructor) {
                if (!used.contains(constructor)) {
                    missingHeads.push_back(Witness{Space::Kind::Constructor,
                                                   constructor,
                                                   std::vector<Witness>((*constructors)[constructor].size()),
                                                   {}});
                }
            }
        }
        else {
            missingHeads.push_back(Witness{});
        }
        for (const Witness &missing : missingHeads) {
            for (const WitnessRow &tail : tails) {
                WitnessRow witness{missing};
                witness.insert(witness.end(), tail.begin(), tail.end());
                witnesses.push_back(std::move(witness));
                if (witnesses.size() >= limit) {
                    return witnesses;
                }
            }
        }
        return witnesses;
    }

    const std::optional<std::vector<std::vector<TypeRef>>> &ConstructorsOf(const TypeRef &type) {
        const std::string key = type.ToString();
        auto found = constructorCache.find(key);
        if (found == constructorCache.end()) {
            found = constructorCache.emplace(key, constructorSource(type)).first;
        }
        return found->second;
    }

private:
    std::vector<WitnessRow> UsefulConstructor(const std::vector<Row> &rows, const std::size_t constructor,
                                              const std::vector<Space> &fields, const Row &rest,
                                              const std::vector<TypeRef> &types, const std::size_t limit) {
        const std::optional<std::vector<std::vector<TypeRef>>> &constructors = ConstructorsOf(types.front());
        std::vector<TypeRef> fieldTypes;
        if (constructors && constructor < constructors->size()) {
            fieldTypes = (*constructors)[constructor];
        }
        fieldTypes.resize(fields.size(), TypeRef::MakeUnknown());
        const std::size_t arity = fields.size();

        std::vector<Row> specialized;
        for (const Row &row : rows) {
            std::vector<const Space *> heads;
            SpreadChoices(row.front(), heads);
            for (const Space *candidate : heads) {
                Row next;
                if (candidate->kind == Space::Kind::Wildcard) {
                    next.resize(arity);
                }
                else if (candidate->kind == Space::Kind::Constructor && candidate->constructor == constructor &&
                         candidate->fields.size() == arity) {
                    next = candidate->fields;
                }
                else {
                    continue;
                }
                next.insert(next.end(), row.begin() + 1, row.end());
                specialized.push_back(std::move(next));
            }
        }
        Row query = fields;
        query.insert(query.end(), rest.begin(), rest.end());
        std::vector<TypeRef> queryTypes = fieldTypes;
        queryTypes.insert(queryTypes.end(), types.begin() + 1, types.end());

        std::vector<WitnessRow> witnesses;
        for (WitnessRow &found : Useful(specialized, query, queryTypes, limit)) {
            Witness packed{Space::Kind::Constructor, constructor, {}, {}};
            packed.fields.assign(std::make_move_iterator(found.begin()),
                                 std::make_move_iterator(found.begin() + static_cast<std::ptrdiff_t>(arity)));
            WitnessRow witness{std::move(packed)};
            witness.insert(witness.end(), std::make_move_iterator(found.begin() + static_cast<std::ptrdiff_t>(arity)),
                           std::make_move_iterator(found.end()));
            witnesses.push_back(std::move(witness));
        }
        return witnesses;
    }

    ConstructorSource constructorSource;
    std::map<std::string, std::optional<std::vector<std::vector<TypeRef>>>> constructorCache;
};

/// Whether a pattern is an `else` arm: the parser writes `else` as the only top-level wildcard a match arm can have.
bool IsElseArm(const Pattern &pattern) {
    return dynamic_cast<const WildcardPattern *>(&pattern) != nullptr;
}

const Pattern &UnguardedPattern(const Pattern &pattern) {
    const auto *guarded = dynamic_cast<const GuardedPattern *>(&pattern);
    return guarded && guarded->inner ? *guarded->inner : pattern;
}

std::string MemberList(const TypeRef &sum) {
    return std::format("'{}'", sum.ToString());
}
} // namespace

bool AnalysisContext::IsStoredAggregate(const TypeRef &type) noexcept {
    return type.kind == TypeRef::Kind::Named || type.kind == TypeRef::Kind::Array ||
           type.kind == TypeRef::Kind::Tuple || type.IsSum() || type.IsOptional() || type.IsFallible();
}

AnalysisContext::PatternBorrow AnalysisContext::MatchSubjectBorrow(const Expr &subject,
                                                                   const TypeRef &expressionType) const {
    if (expressionType.kind == TypeRef::Kind::Reference && !expressionType.inner.empty()) {
        return expressionType.inner.front().isMut ? PatternBorrow::Exclusive : PatternBorrow::Shared;
    }
    return AnalyzeMovePlace(subject).IsBorrowedStorage() ? PatternBorrow::Shared : PatternBorrow::Owned;
}

void AnalysisContext::RecordPatternBinding(const Pattern &pattern, Symbol &symbol, const bool view) {
    if (currentPatternBorrow == PatternBorrow::Owned) {
        return;
    }
    // A borrowed subject is never consumed, so its bindings refer to its storage. A payload that sits whole at its own
    // offset can be written through an exclusive borrow; a view of several members cannot, because the tag it would
    // need belongs to the subject.
    patternBindingModes.insert_or_assign(&pattern, view ? PatternBindingMode::View : PatternBindingMode::Alias);
    symbol.isSubsetView = view;
    symbol.isMut = currentPatternBorrow == PatternBorrow::Exclusive && !view;
}

bool AnalysisContext::RejectSubsetViewUse(const Expr &expression, const SourceLocation location,
                                          const std::string_view use) {
    const Expr *named = &expression;
    if (const auto *move = dynamic_cast<const MoveExpr *>(named)) {
        named = move->operand.get();
    }
    const auto *identifier = dynamic_cast<const IdentExpr *>(named);
    const Symbol *symbol = identifier ? currentScope->Lookup(identifier->name) : nullptr;
    if (!symbol || !symbol->isSubsetView) {
        return false;
    }
    EmitError(
        location, std::format("subset view '{}' cannot {}", symbol->name, use),
        {std::format("'{}' reads several members of a borrowed subject through the subject's own tag", symbol->name)},
        std::format("copy it into an owned value first, as in 'let owned: {} = {};'", symbol->type.ToString(),
                    symbol->name));
    return true;
}

void AnalysisContext::CheckGuardKeepsBindings(const GuardedPattern &pattern) {
    std::vector<std::pair<std::string, SourceLocation>> names;
    const auto collect = [&](this auto &&self, const Pattern &candidate) -> void {
        const auto visit = [&](const PatternPtr &child) {
            if (child) {
                self(*child);
            }
        };
        if (const auto *identifier = dynamic_cast<const IdentPattern *>(&candidate)) {
            names.emplace_back(identifier->name, identifier->location);
        }
        else if (const auto *typed = dynamic_cast<const TypedPattern *>(&candidate)) {
            if (!typed->name.empty()) {
                names.emplace_back(typed->name, typed->location);
            }
        }
        else if (const auto *presence = dynamic_cast<const PresencePattern *>(&candidate)) {
            visit(presence->inner);
        }
        else if (const auto *enumeration = dynamic_cast<const EnumPattern *>(&candidate)) {
            std::ranges::for_each(enumeration->args, visit);
            for (const EnumPattern::NamedArg &argument : enumeration->namedArgs) {
                visit(argument.pattern);
            }
        }
        else if (const auto *tuple = dynamic_cast<const TuplePattern *>(&candidate)) {
            std::ranges::for_each(tuple->elements, visit);
        }
        else if (const auto *structure = dynamic_cast<const StructPattern *>(&candidate)) {
            for (const StructPattern::Field &field : structure->fields) {
                visit(field.pattern);
            }
        }
    };
    if (pattern.inner) {
        collect(*pattern.inner);
    }
    for (const auto &[name, location] : names) {
        const Symbol *symbol = currentScope->LookupLocal(name);
        if (!symbol) {
            continue;
        }
        const MoveStateTracker::Record *record = moveStates.TryGet(MoveStateTracker::Local(symbol));
        if (record &&
            (record->state == MoveStateTracker::State::Moved || record->state == MoveStateTracker::State::MaybeMoved)) {
            EmitError(pattern.guard ? pattern.guard->location : location,
                      std::format("pattern guard cannot move '{}'", name),
                      {"a guard that fails falls through to later arms, which still need the payload"},
                      "move the binding in the arm body instead");
        }
    }
}

bool AnalysisContext::IsNativeMatchSubject(const TypeRef &subjectType) {
    return subjectType.IsUnit() || MentionsNativeType(subjectType);
}

bool AnalysisContext::CheckFreeBindingName(const std::string &name, const SourceLocation location,
                                           const TypeRef &subjectType) {
    if (name.empty() || name == "_") {
        return true;
    }
    // A case of the matched value's own enum or variant, or of a variant member of a matched sum, is what the author
    // meant to select; a binding would silently match everything instead.
    std::vector<TypeRef> nominalSubjects;
    if (subjectType.IsSum()) {
        nominalSubjects = subjectType.inner;
    }
    else if (subjectType.kind == TypeRef::Kind::Named) {
        nominalSubjects.push_back(subjectType);
    }
    for (const TypeRef &candidate : nominalSubjects) {
        if (candidate.kind != TypeRef::Kind::Named) {
            continue;
        }
        const std::string enumName = BaseTypeName(candidate.name);
        if (const std::optional<ResolvedCase> resolved = LookupCase(enumName, name)) {
            const std::string_view kind = resolved->form == EnumDecl::Form::Variant ? "variant" : "enum";
            EmitError(location,
                      std::format("pattern '{}' cannot bind a new variable because '{}' is a case of {} '{}'", name,
                                  name, kind, enumName),
                      {}, std::format("write '{}::{}' to select the case", enumName, name));
            return false;
        }
    }
    const Symbol *symbol = currentScope->Lookup(name);
    if (!symbol || symbol->kind == Symbol::Kind::Var) {
        return true;
    }
    std::string_view kind = SymbolKindName(symbol->kind);
    if (symbol->kind == Symbol::Kind::Type && symbol->type.kind == TypeRef::Kind::TypeParam) {
        kind = "generic parameter";
    }
    std::string help;
    if (symbol->kind == Symbol::Kind::Const) {
        // A pattern compares only with values written in it, so a named constant is neither a test nor a binding.
        help = std::format("a pattern compares with literal values; write the value of '{0}', or bind the value and "
                           "compare it with '{0}' in a guard such as 'value if value >= {0}'",
                           name);
    }
    else if (symbol->kind == Symbol::Kind::Type) {
        std::string lowered = name;
        lowered.front() = static_cast<char>(std::tolower(static_cast<unsigned char>(name.front())));
        help = std::format("write '{0}: {1}' to select the member, '{1} {{ ... }}' to destructure it, or 'Type::Case' "
                           "to select a case",
                           lowered == name ? lowered + "Value" : lowered, name);
    }
    else {
        help = std::format("choose a binding name that does not already name a {}", kind);
    }
    EmitError(location,
              std::format("pattern '{}' cannot bind a new variable because '{}' already names a {}", name, name, kind),
              {DeclarationNote(*symbol)}, std::move(help));
    return false;
}

std::optional<std::size_t> AnalysisContext::SumMemberOfPattern(const Pattern &pattern, const TypeRef &sum,
                                                               std::string *issue, std::optional<std::string> *help) {
    const auto membersNamed = [&](const std::string &written) {
        std::vector<std::size_t> found;
        const std::string nominal = NominalTypeName(written);
        for (std::size_t index = 0; index < sum.inner.size(); ++index) {
            const TypeRef &member = sum.inner[index];
            if (member.kind == TypeRef::Kind::Named &&
                (BaseTypeName(member.name) == nominal || BaseTypeName(member.name) == written)) {
                found.push_back(index);
            }
        }
        return found;
    };
    const auto single = [&](const std::vector<std::size_t> &found, const std::string &written,
                            const std::string_view what) -> std::optional<std::size_t> {
        if (found.size() == 1) {
            return found.front();
        }
        if (found.empty()) {
            *issue = std::format("{} '{}' is not a member of {}", what, written, MemberList(sum));
        }
        else {
            *issue = std::format("case pattern on '{}' is ambiguous in {}: more than one member is that {}", written,
                                 MemberList(sum), what);
            *help = std::format("select one instantiation with a typed pattern, as in 'v: {}', and match it separately",
                                sum.inner[found.front()].ToString());
        }
        return std::nullopt;
    };

    if (const auto *enumeration = dynamic_cast<const EnumPattern *>(&pattern)) {
        if (enumeration->path.size() < 2) {
            // An unqualified case on a sum names no member; offer the qualified spelling when one member has the case.
            const std::string &caseName = enumeration->path.empty() ? std::string() : enumeration->path.front();
            std::vector<std::string> owners;
            for (const TypeRef &member : sum.inner) {
                if (member.kind == TypeRef::Kind::Named && LookupCase(BaseTypeName(member.name), caseName)) {
                    owners.push_back(BaseTypeName(member.name));
                }
            }
            *issue = std::format("case pattern '.{}' cannot select from sum {}", caseName, MemberList(sum));
            *help = owners.size() == 1
                      ? std::format("write '{}::{}' to select the member and its case", owners.front(), caseName)
                      : std::string("qualify the case with its variant, as in 'Type::Case'");
            return std::nullopt;
        }
        std::string variantName;
        for (std::size_t index = 0; index + 1 < enumeration->path.size(); ++index) {
            variantName += (index == 0 ? "" : "::") + enumeration->path[index];
        }
        return single(membersNamed(enumeration->path.front()), variantName, "variant");
    }
    if (const auto *structure = dynamic_cast<const StructPattern *>(&pattern)) {
        return single(membersNamed(structure->typeName), structure->typeName, "type");
    }
    if (const auto *literal = dynamic_cast<const LiteralPattern *>(&pattern)) {
        const Token &token = literal->value;
        const bool unsuffixedInteger =
            token.kind == TokenKind::IntLiteral && NumericLiteralSuffix(std::string_view(token.text)).empty();
        const bool unsuffixedFloat =
            token.kind == TokenKind::FloatLiteral && NumericLiteralSuffix(std::string_view(token.text)).empty();
        std::vector<std::size_t> found;
        if (unsuffixedInteger || unsuffixedFloat) {
            for (std::size_t index = 0; index < sum.inner.size(); ++index) {
                if (unsuffixedFloat ? sum.inner[index].IsFloat() : sum.inner[index].IsInteger()) {
                    found.push_back(index);
                }
            }
        }
        else {
            const TypeRef literalType = LiteralType(token);
            if (const auto member = std::ranges::find(sum.inner, literalType); member != sum.inner.end()) {
                found.push_back(static_cast<std::size_t>(member - sum.inner.begin()));
            }
        }
        if (found.size() == 1) {
            return found.front();
        }
        *issue = found.empty()
                   ? std::format("literal pattern '{}' matches no member of {}", token.text, MemberList(sum))
                   : std::format("literal pattern '{}' could match more than one member of {}", token.text,
                                 MemberList(sum));
        *help = "add a literal suffix, or select the member with a typed pattern first";
        return std::nullopt;
    }
    if (const auto *tuple = dynamic_cast<const TuplePattern *>(&pattern)) {
        std::vector<std::size_t> found;
        for (std::size_t index = 0; index < sum.inner.size(); ++index) {
            const TypeRef &member = sum.inner[index];
            if (member.kind == TypeRef::Kind::Tuple && member.inner.size() == tuple->elements.size()) {
                found.push_back(index);
            }
        }
        if (found.size() == 1) {
            return found.front();
        }
        *issue = tuple->elements.empty() ? std::format("unit pattern '()' matches no member of {}", MemberList(sum))
                                         : std::format("tuple pattern with {} elements selects no single member of {}",
                                                       tuple->elements.size(), MemberList(sum));
        *help = "select the member with a typed pattern first";
        return std::nullopt;
    }
    *issue = std::format("this pattern cannot select a member of {}", MemberList(sum));
    *help = "select the member with a typed pattern, as in 'v: Member'";
    return std::nullopt;
}

void AnalysisContext::CheckTypedPattern(const TypedPattern &pattern, const TypeRef &subjectType) {
    const TypeRef annotation = pattern.type ? ResolveType(*pattern.type) : TypeRef::MakeUnknown();
    if (!annotation.IsUnknown()) {
        typedPatternTypes.insert_or_assign(&pattern, annotation);
    }
    if (!annotation.IsUnknown() && !subjectType.IsUnknown()) {
        // A selection that depends on a type parameter is decided again by each instantiation, where the subject or
        // the annotation has its final form; only a selection that no substitution can change is reported here.
        const bool dependent = MentionsTypeParameter(subjectType) || MentionsTypeParameter(annotation);
        if (!dependent) {
            if (std::optional<PatternIssue> issue = TypedSelectionIssue(pattern, subjectType, annotation)) {
                EmitError(issue->location, issue->message, {}, issue->help);
            }
        }
    }
    if (!pattern.name.empty() && CheckFreeBindingName(pattern.name, pattern.location, TypeRef::MakeUnknown())) {
        // Whatever the annotation selects, the binding has the annotation's type: the whole subject, the selected
        // member or subset, or the selected payload. Several selected members make a subset, which under a borrowed
        // subject is only a view of it.
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.name = pattern.name;
        symbol.location = pattern.location;
        symbol.type = annotation;
        symbol.isMut = false;
        const NativeSelection selection = annotation.IsUnknown() || subjectType.IsUnknown()
                                            ? NativeSelection{}
                                            : ClassifyNativeSelection(subjectType, annotation);
        RecordPatternBinding(pattern, symbol, selection.members.size() > 1);
        DefineTrackedLocal(std::move(symbol), true);
    }
}

void AnalysisContext::CheckNativeCasePattern(const EnumPattern &pattern, const TypeRef &subjectType) {
    const std::string &name = pattern.path.front();
    const bool fallible = subjectType.IsFallible();
    TypeRef payload = TypeRef::MakeUnknown();
    if (fallible && (name == "Success" || name == "Failure")) {
        payload = name == "Success" ? subjectType.FallibleSuccess() : subjectType.FallibleError();
    }
    else if (!fallible && name == "Some") {
        payload = subjectType.inner.front();
    }
    else {
        const std::string subject = subjectType.ToString();
        if (!fallible && name == "None") {
            EmitError(pattern.location,
                      std::format("'.None' matches a variant case; use 'none' for native optional '{}'", subject), {},
                      "write 'none'");
        }
        else if (name == "Success" || name == "Failure" || name == "Some") {
            EmitError(pattern.location,
                      std::format("'.{}(...)' matches a native {}, but the matched value has type '{}'", name,
                                  name == "Some" ? "optional" : "fallible", subject));
        }
        else {
            EmitError(
                pattern.location,
                std::format("native {} '{}' has no case '{}'", fallible ? "fallible" : "optional", subject, name), {},
                fallible ? "match '.Success(...)' or '.Failure(...)'" : "match '.Some(...)', 'value?', or 'none'");
        }
        DefinePatternBindings(pattern);
        return;
    }
    const std::size_t written = pattern.args.size() + pattern.namedArgs.size();
    if (!pattern.namedArgs.empty() || pattern.args.size() != 1 || !pattern.args.front()) {
        EmitError(pattern.location, std::format("pattern '.{}' expects 1 field, but found {}", name, written));
        DefinePatternBindings(pattern);
        return;
    }
    CheckPattern(*pattern.args.front(), payload);
}

void AnalysisContext::DefinePatternBindings(const Pattern &pattern) {
    const auto define = [this](const std::string &name, const SourceLocation location) {
        if (name.empty() || name == "_") {
            return;
        }
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.name = name;
        symbol.location = location;
        symbol.type = TypeRef::MakeUnknown();
        DefineTrackedLocal(std::move(symbol), true);
    };
    const auto visit = [this](const PatternPtr &child) {
        if (child) {
            DefinePatternBindings(*child);
        }
    };
    if (const auto *identifier = dynamic_cast<const IdentPattern *>(&pattern)) {
        define(identifier->name, identifier->location);
    }
    else if (const auto *typed = dynamic_cast<const TypedPattern *>(&pattern)) {
        define(typed->name, typed->location);
    }
    else if (const auto *guarded = dynamic_cast<const GuardedPattern *>(&pattern)) {
        visit(guarded->inner);
    }
    else if (const auto *presence = dynamic_cast<const PresencePattern *>(&pattern)) {
        visit(presence->inner);
    }
    else if (const auto *enumeration = dynamic_cast<const EnumPattern *>(&pattern)) {
        std::ranges::for_each(enumeration->args, visit);
        for (const EnumPattern::NamedArg &argument : enumeration->namedArgs) {
            visit(argument.pattern);
        }
    }
    else if (const auto *tuple = dynamic_cast<const TuplePattern *>(&pattern)) {
        std::ranges::for_each(tuple->elements, visit);
    }
    else if (const auto *structure = dynamic_cast<const StructPattern *>(&pattern)) {
        for (const StructPattern::Field &field : structure->fields) {
            visit(field.pattern);
        }
    }
}

std::optional<AnalysisContext::PatternIssue> AnalysisContext::TypedSelectionIssue(const TypedPattern &pattern,
                                                                                  const TypeRef &subjectType,
                                                                                  const TypeRef &annotation) const {
    if (ClassifyNativeSelection(subjectType, annotation).kind != NativeSelection::Kind::Invalid) {
        return std::nullopt;
    }
    const std::string written = annotation.ToString();
    const std::string subject = subjectType.ToString();
    if (subjectType.IsFallible()) {
        return PatternIssue{
            pattern.location, std::format("typed pattern '{}' cannot select from fallible '{}'", written, subject),
            std::format("match a channel with '.Success(v: {})' or '.Failure(e: {})'",
                        subjectType.FallibleSuccess().ToString(), subjectType.FallibleError().ToString())};
    }
    if (subjectType.IsSum()) {
        return PatternIssue{pattern.location,
                            std::format("type '{}' is not a member or subset of sum '{}'", written, subject),
                            std::nullopt};
    }
    if (subjectType.IsOptional()) {
        return PatternIssue{pattern.location,
                            std::format("typed pattern '{}' cannot select from optional '{}'", written, subject),
                            std::format("a typed pattern selects one presence level by its payload '{}', or a "
                                        "member of it; use '.Some(...)' to match deeper levels",
                                        subjectType.inner.front().ToString())};
    }
    return PatternIssue{pattern.location,
                        std::format("typed pattern '{}' cannot match a value of type '{}'", written, subject),
                        "a typed pattern on a value that is not a sum or an optional must name its exact type"};
}

void AnalysisContext::CheckMembershipTest(const IsExpr &expression, const TypeRef &operandType,
                                          const TypeRef &testedType) {
    const TypeRef subject = operandType.kind == TypeRef::Kind::Reference && !operandType.inner.empty()
                              ? operandType.inner.front()
                              : operandType;
    if (subject.IsUnknown() || testedType.IsUnknown()) {
        return;
    }
    if (MentionsTypeParameter(subject) || MentionsTypeParameter(testedType)) {
        if (currentFunctionDecl) {
            deferredMembershipChecks[currentFunctionDecl].push_back({subject, testedType, expression.location});
        }
        return;
    }
    if (std::optional<PatternIssue> issue = MembershipIssue(subject, testedType, expression.location)) {
        EmitError(issue->location, issue->message, {}, issue->help);
    }
}

std::optional<AnalysisContext::PatternIssue>
AnalysisContext::MembershipIssue(const TypeRef &subject, const TypeRef &tested, const SourceLocation location) const {
    if (subject.IsFallible()) {
        return PatternIssue{location, std::format("'is' cannot test the channel of fallible '{}'", subject.ToString()),
                            "match '.Success(...)' or '.Failure(...)' instead"};
    }
    if (ClassifyNativeSelection(subject, tested).kind != NativeSelection::Kind::Invalid) {
        return std::nullopt;
    }
    if (subject.IsSum()) {
        return PatternIssue{
            location,
            std::format("type '{}' is not a member or subset of sum '{}'", tested.ToString(), subject.ToString()),
            std::nullopt};
    }
    if (subject.IsOptional()) {
        return PatternIssue{location,
                            std::format("type '{}' is neither the payload of optional '{}' nor a member of it",
                                        tested.ToString(), subject.ToString()),
                            "'is' tests one presence level; match '.Some(...)' to inspect deeper levels"};
    }
    return PatternIssue{
        location,
        std::format("'is {}' can never be true for a value of type '{}'", tested.ToString(), subject.ToString()),
        "an 'is' test on a value that is not a sum or an optional compares its exact type"};
}

std::optional<std::vector<std::vector<TypeRef>>> AnalysisContext::CoverageConstructors(const TypeRef &type) {
    using Constructors = std::vector<std::vector<TypeRef>>;
    if (type.IsSum()) {
        Constructors constructors;
        for (const TypeRef &member : type.inner) {
            constructors.push_back({member});
        }
        return constructors;
    }
    if (type.IsOptional() && !type.inner.empty()) {
        return Constructors{{}, {type.inner.front()}};
    }
    if (type.IsFallible()) {
        return Constructors{{type.FallibleSuccess()}, {type.FallibleError()}};
    }
    if (type.kind == TypeRef::Kind::Tuple) {
        return Constructors{type.inner};
    }
    if (type.IsBool()) {
        return Constructors{{}, {}};
    }
    if (type.kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    const EnumDecl *declaration = EnumNamed(BaseTypeName(type.name));
    if (!declaration) {
        return std::nullopt;
    }
    std::unordered_map<std::string, TypeRef> substitutions;
    const std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(type.name);
    for (std::size_t index = 0; index < std::min(arguments.size(), declaration->typeParams.size()); ++index) {
        substitutions.emplace(declaration->typeParams[index].name, arguments[index]);
    }
    Constructors constructors;
    for (const auto &selectedCase : declaration->variants) {
        std::vector<TypeRef> fields;
        for (const auto &field : selectedCase.fields) {
            fields.push_back(ResolveTypeWithSubstitution(*field, substitutions));
        }
        for (const auto &field : selectedCase.namedFields) {
            fields.push_back(ResolveTypeWithSubstitution(*field.type, substitutions));
        }
        constructors.push_back(std::move(fields));
    }
    return constructors;
}

/// Builds the coverage form of each arm against the subject's type, under one generic substitution, and names the
/// values a match leaves uncovered.
class NativePatternTranslator {
public:
    NativePatternTranslator(AnalysisContext &analysis, const std::unordered_map<std::string, TypeRef> &substitutions,
                            std::vector<AnalysisContext::PatternIssue> *issues)
        : analysis(analysis)
        , substitutions(substitutions)
        , issues(issues) {
    }

    Space Translate(const Pattern &pattern, const TypeRef &type) {
        if (type.IsUnknown()) {
            return {};
        }
        if (const auto *guarded = dynamic_cast<const GuardedPattern *>(&pattern)) {
            return guarded->inner ? Translate(*guarded->inner, type) : Space{};
        }
        if (dynamic_cast<const WildcardPattern *>(&pattern) || dynamic_cast<const IdentPattern *>(&pattern)) {
            return {};
        }
        if (const auto *typed = dynamic_cast<const TypedPattern *>(&pattern)) {
            return TranslateTyped(*typed, type);
        }
        if (dynamic_cast<const NonePattern *>(&pattern)) {
            return type.IsOptional() ? Space::Of(0, {}) : Space{};
        }
        if (const auto *presence = dynamic_cast<const PresencePattern *>(&pattern)) {
            if (!type.IsOptional() || !presence->inner) {
                return {};
            }
            return Space::Of(1, {Translate(*presence->inner, type.inner.front())});
        }
        if (const auto *enumeration = dynamic_cast<const EnumPattern *>(&pattern)) {
            if (const std::optional<Space> native = TranslateNativeCase(*enumeration, type)) {
                return *native;
            }
        }
        if (type.IsSum() && !dynamic_cast<const RangePattern *>(&pattern)) {
            std::string issue;
            std::optional<std::string> help;
            const std::optional<std::size_t> member = analysis.SumMemberOfPattern(pattern, type, &issue, &help);
            if (!member) {
                sawInvalidSelection = true;
                return Space::Choice({});
            }
            return Space::Of(*member, {Translate(pattern, type.inner[*member])});
        }
        if (const auto *enumeration = dynamic_cast<const EnumPattern *>(&pattern)) {
            return TranslateNominalCase(*enumeration, type);
        }
        if (const auto *literal = dynamic_cast<const LiteralPattern *>(&pattern)) {
            if (type.IsBool() && literal->value.kind == TokenKind::BoolLiteral) {
                return Space::Of(literal->value.text == "true" ? 1 : 0, {});
            }
            return Space::Value("literal:" + literal->value.text);
        }
        if (const auto *range = dynamic_cast<const RangePattern *>(&pattern)) {
            return Space::Value(std::format("range:{}", static_cast<const void *>(range)));
        }
        if (const auto *tuple = dynamic_cast<const TuplePattern *>(&pattern)) {
            if (type.kind != TypeRef::Kind::Tuple || type.inner.size() != tuple->elements.size()) {
                return {};
            }
            std::vector<Space> fields;
            for (std::size_t index = 0; index < tuple->elements.size(); ++index) {
                fields.push_back(tuple->elements[index] ? Translate(*tuple->elements[index], type.inner[index])
                                                        : Space{});
            }
            return Space::Of(0, std::move(fields));
        }
        if (const auto *structure = dynamic_cast<const StructPattern *>(&pattern)) {
            // A struct is open to coverage: it completes its position only when every field pattern matches anything.
            const bool irrefutable = std::ranges::all_of(structure->fields, [](const StructPattern::Field &field) {
                return !field.pattern || dynamic_cast<const WildcardPattern *>(field.pattern.get()) ||
                       dynamic_cast<const IdentPattern *>(field.pattern.get());
            });
            return irrefutable ? Space{} : Space::Value(std::format("struct:{}", static_cast<const void *>(structure)));
        }
        return {};
    }

    /// A value no arm matches, spelled as the pattern that would match it.
    std::string Describe(const Witness &witness, const TypeRef &type);

    /// Whether translating met an annotation that selects nothing from its subject.
    bool sawInvalidSelection = false;

private:
    Space TranslateTyped(const TypedPattern &pattern, const TypeRef &type) {
        const TypeRef *recorded = analysis.TypedPatternAnnotation(pattern);
        if (!recorded) {
            return {};
        }
        const TypeRef annotation =
            substitutions.empty() ? *recorded : analysis.SubstituteTypeParameters(*recorded, substitutions);
        const NativeSelection selection = ClassifyNativeSelection(type, annotation);
        const auto memberChoice = [](const std::vector<std::size_t> &members) {
            std::vector<Space> alternatives;
            for (const std::size_t member : members) {
                alternatives.push_back(Space::Of(member, {Space{}}));
            }
            return Space::Choice(std::move(alternatives));
        };
        switch (selection.kind) {
        case NativeSelection::Kind::Whole:
            return {};
        case NativeSelection::Kind::Members:
            return memberChoice(selection.members);
        case NativeSelection::Kind::Presence:
            return Space::Of(1, {selection.members.empty() ? Space{} : memberChoice(selection.members)});
        case NativeSelection::Kind::Invalid:
            break;
        }
        sawInvalidSelection = true;
        // The declaration reports an annotation no substitution can change itself; only an instantiation reports one
        // its substitution made invalid.
        if (issues && !substitutions.empty()) {
            if (std::optional<AnalysisContext::PatternIssue> issue =
                    analysis.TypedSelectionIssue(pattern, type, annotation)) {
                issues->push_back(std::move(*issue));
            }
        }
        return Space::Choice({});
    }

    std::optional<Space> TranslateNativeCase(const EnumPattern &pattern, const TypeRef &type) {
        if (pattern.path.size() != 1 || (!type.IsOptional() && !type.IsFallible())) {
            return std::nullopt;
        }
        const std::string &name = pattern.path.front();
        if (pattern.args.size() != 1 || !pattern.namedArgs.empty() || !pattern.args.front()) {
            return Space{};
        }
        const Pattern &argument = *pattern.args.front();
        if (type.IsFallible() && (name == "Success" || name == "Failure")) {
            const bool failure = name == "Failure";
            return Space::Of(failure ? 1 : 0,
                             {Translate(argument, failure ? type.FallibleError() : type.FallibleSuccess())});
        }
        if (type.IsOptional() && name == "Some") {
            return Space::Of(1, {Translate(argument, type.inner.front())});
        }
        return Space{};
    }

    Space TranslateNominalCase(const EnumPattern &pattern, const TypeRef &type) {
        if (type.kind != TypeRef::Kind::Named || pattern.path.empty()) {
            return {};
        }
        const EnumDecl *declaration = analysis.EnumNamed(analysis.BaseTypeName(type.name));
        if (!declaration) {
            return {};
        }
        const std::string &caseName = pattern.path.back();
        const auto selected = std::ranges::find(declaration->variants, caseName, &EnumDecl::Variant::name);
        if (selected == declaration->variants.end()) {
            return {};
        }
        const auto index = static_cast<std::size_t>(selected - declaration->variants.begin());
        const std::optional<std::vector<std::vector<TypeRef>>> constructors = analysis.CoverageConstructors(type);
        if (!constructors || index >= constructors->size()) {
            return {};
        }
        const std::vector<TypeRef> &fieldTypes = (*constructors)[index];
        if (!pattern.braced && pattern.args.size() != fieldTypes.size()) {
            return {};
        }
        // A field a named payload pattern leaves out stays a wildcard.
        std::vector<Space> fields(fieldTypes.size());
        for (std::size_t field = 0; field < pattern.args.size(); ++field) {
            if (pattern.args[field]) {
                fields[field] = Translate(*pattern.args[field], fieldTypes[field]);
            }
        }
        for (const EnumPattern::NamedArg &argument : pattern.namedArgs) {
            const auto named =
                std::ranges::find(selected->namedFields, argument.name, &EnumDecl::Variant::NamedField::name);
            if (named == selected->namedFields.end() || !argument.pattern) {
                continue;
            }
            const std::size_t field =
                selected->fields.size() + static_cast<std::size_t>(named - selected->namedFields.begin());
            fields[field] = Translate(*argument.pattern, fieldTypes[field]);
        }
        return Space::Of(index, std::move(fields));
    }

    AnalysisContext &analysis;
    const std::unordered_map<std::string, TypeRef> &substitutions;
    std::vector<AnalysisContext::PatternIssue> *issues;
};

std::string NativePatternTranslator::Describe(const Witness &witness, const TypeRef &type) {
    if (witness.kind == Space::Kind::Wildcard) {
        return "_";
    }
    if (witness.kind == Space::Kind::Opaque) {
        return witness.key.starts_with("literal:") ? witness.key.substr(8) : "_";
    }
    const auto field = [&](const std::size_t index, const TypeRef &fieldType) {
        return index < witness.fields.size() ? Describe(witness.fields[index], fieldType) : std::string("_");
    };
    if (type.IsOptional()) {
        return witness.constructor == 0 ? std::string("none") : ".Some(" + field(0, type.inner.front()) + ")";
    }
    if (type.IsFallible()) {
        return witness.constructor == 0 ? ".Success(" + field(0, type.FallibleSuccess()) + ")"
                                        : ".Failure(" + field(0, type.FallibleError()) + ")";
    }
    if (type.IsSum()) {
        const TypeRef &member = type.inner[witness.constructor];
        const std::string inner = field(0, member);
        return inner == "_" ? "_: " + member.ToString() : inner;
    }
    if (type.kind == TypeRef::Kind::Tuple) {
        std::string text = "(";
        for (std::size_t index = 0; index < type.inner.size(); ++index) {
            text += (index == 0 ? "" : ", ") + field(index, type.inner[index]);
        }
        return text + ")";
    }
    if (type.IsBool()) {
        return witness.constructor == 1 ? "true" : "false";
    }
    if (type.kind == TypeRef::Kind::Named) {
        const std::string enumName = analysis.BaseTypeName(type.name);
        const EnumDecl *declaration = analysis.EnumNamed(enumName);
        if (declaration && witness.constructor < declaration->variants.size()) {
            const auto constructors = analysis.CoverageConstructors(type);
            std::string text = enumName + "::" + declaration->variants[witness.constructor].name;
            if (constructors && !(*constructors)[witness.constructor].empty()) {
                const std::vector<TypeRef> &fields = (*constructors)[witness.constructor];
                text += "(";
                for (std::size_t index = 0; index < fields.size(); ++index) {
                    text += (index == 0 ? "" : ", ") + field(index, fields[index]);
                }
                text += ")";
            }
            return text;
        }
    }
    return "_";
}

bool AnalysisContext::NativeMatchCoverage(const std::vector<const Pattern *> &patterns, const TypeRef &subjectType,
                                          const std::unordered_map<std::string, TypeRef> &substitutions,
                                          std::vector<PatternIssue> *issues) {
    CoverageSolver solver([this](const TypeRef &type) { return CoverageConstructors(type); });
    NativePatternTranslator translator(*this, substitutions, issues);
    std::vector<Row> rows;
    bool earlierMatchesEverything = false;
    for (const Pattern *pattern : patterns) {
        if (!pattern) {
            continue;
        }
        const bool guarded = dynamic_cast<const GuardedPattern *>(pattern) != nullptr;
        const bool invalidBefore = translator.sawInvalidSelection;
        translator.sawInvalidSelection = false;
        const Space space = translator.Translate(UnguardedPattern(*pattern), subjectType);
        const bool invalid = translator.sawInvalidSelection;
        translator.sawInvalidSelection = invalidBefore || invalid;
        // An `else` arm covers whatever remains, possibly nothing, so it is never reported, even after a generic sum
        // collapsed and an earlier arm came to match everything.
        if (issues && !IsElseArm(*pattern) && !invalid && solver.Useful(rows, Row{space}, {subjectType}, 1).empty()) {
            issues->push_back(PatternIssue{pattern->location,
                                           earlierMatchesEverything
                                               ? "match arm is unreachable because an earlier pattern matches every "
                                                 "value"
                                               : "match arm is unreachable because earlier arms already match every "
                                                 "value it matches",
                                           std::nullopt});
        }
        if (!guarded) {
            earlierMatchesEverything = earlierMatchesEverything || space.kind == Space::Kind::Wildcard;
            rows.push_back(Row{space});
        }
    }
    const std::vector<WitnessRow> missing = solver.Useful(rows, Row{Space{}}, {subjectType}, 8);
    if (!missing.empty() && issues && !translator.sawInvalidSelection) {
        std::string names;
        for (const WitnessRow &witness : missing) {
            names += (names.empty() ? "" : ", ") + translator.Describe(witness.front(), subjectType);
        }
        issues->push_back(PatternIssue{
            patterns.empty() || !patterns.back() ? SourceLocation{} : patterns.back()->location,
            std::format("match on '{}' is not exhaustive; missing {}", subjectType.ToString(), names), std::nullopt});
    }
    return missing.empty();
}

std::optional<std::string> AnalysisContext::MissingMatchValues(const std::vector<const Pattern *> &patterns,
                                                               const TypeRef &subjectType) {
    CoverageSolver solver([this](const TypeRef &type) { return CoverageConstructors(type); });
    const std::unordered_map<std::string, TypeRef> noSubstitutions;
    NativePatternTranslator translator(*this, noSubstitutions, nullptr);
    std::vector<Row> rows;
    for (const Pattern *pattern : patterns) {
        // A guarded arm may refuse any value it matches, so it covers nothing.
        if (pattern && !dynamic_cast<const GuardedPattern *>(pattern)) {
            rows.push_back(Row{translator.Translate(*pattern, subjectType)});
        }
    }
    const std::vector<WitnessRow> missing = solver.Useful(rows, Row{Space{}}, {subjectType}, 8);
    if (missing.empty()) {
        return std::nullopt;
    }
    std::string names;
    for (const WitnessRow &witness : missing) {
        names += (names.empty() ? "" : ", ") + translator.Describe(witness.front(), subjectType);
    }
    return names;
}

void AnalysisContext::ValidateNativeMatch(const std::vector<const Pattern *> &patterns, const TypeRef &subjectType) {
    const auto mentionsParameter = [&] {
        if (MentionsTypeParameter(subjectType)) {
            return true;
        }
        return std::ranges::any_of(typedPatternTypes, [&](const auto &entry) {
            return MentionsTypeParameter(entry.second) && std::ranges::any_of(patterns, [&](const Pattern *pattern) {
                       return pattern && PatternContains(*pattern, *entry.first);
                   });
        });
    };
    const bool dependent = currentFunctionDecl && mentionsParameter();
    std::vector<PatternIssue> issues;
    const bool exhaustive = NativeMatchCoverage(patterns, subjectType, {}, dependent ? nullptr : &issues);
    if (!patterns.empty() && patterns.front()) {
        nativeMatchExhaustive.insert_or_assign(patterns.front(), exhaustive);
    }
    if (dependent) {
        // Coverage of a match over type parameters is decided per instantiation: arms that are distinct members here
        // may name one type there, and an annotation that is no member here may become one.
        deferredMatchChecks[currentFunctionDecl].push_back({patterns, subjectType});
        return;
    }
    for (PatternIssue &issue : issues) {
        EmitError(issue.location, std::move(issue.message), {}, std::move(issue.help));
    }
}

bool AnalysisContext::PatternContains(const Pattern &pattern, const Pattern &target) {
    if (&pattern == &target) {
        return true;
    }
    const auto contains = [&](const PatternPtr &child) { return child && PatternContains(*child, target); };
    if (const auto *guarded = dynamic_cast<const GuardedPattern *>(&pattern)) {
        return contains(guarded->inner);
    }
    if (const auto *presence = dynamic_cast<const PresencePattern *>(&pattern)) {
        return contains(presence->inner);
    }
    if (const auto *enumeration = dynamic_cast<const EnumPattern *>(&pattern)) {
        return std::ranges::any_of(enumeration->args, contains) ||
               std::ranges::any_of(enumeration->namedArgs,
                                   [&](const EnumPattern::NamedArg &argument) { return contains(argument.pattern); });
    }
    if (const auto *tuple = dynamic_cast<const TuplePattern *>(&pattern)) {
        return std::ranges::any_of(tuple->elements, contains);
    }
    if (const auto *structure = dynamic_cast<const StructPattern *>(&pattern)) {
        return std::ranges::any_of(structure->fields,
                                   [&](const StructPattern::Field &field) { return contains(field.pattern); });
    }
    if (const auto *range = dynamic_cast<const RangePattern *>(&pattern)) {
        return contains(range->lo) || contains(range->hi);
    }
    return false;
}

void AnalysisContext::ValidateDeferredPatternChecks(const FuncDecl &declaration,
                                                    const std::unordered_map<std::string, TypeRef> &substitutions) {
    const std::string note = InstantiationNote(declaration, substitutions);
    const auto emit = [&](PatternIssue issue) {
        EmitError(issue.location, std::move(issue.message), {note}, std::move(issue.help));
    };

    if (const auto it = deferredNativeTypes.find(&declaration); it != deferredNativeTypes.end()) {
        for (const TypeRef &type : it->second) {
            TypeRef instantiated = SubstituteTypeParameters(type, substitutions);
            LayoutOfTypeRef(instantiated);
            instantiatedTypes.push_back(std::move(instantiated));
        }
    }
    if (const auto it = deferredMatchChecks.find(&declaration); it != deferredMatchChecks.end()) {
        for (const DeferredMatchCheck &check : it->second) {
            std::vector<PatternIssue> issues;
            static_cast<void>(NativeMatchCoverage(
                check.patterns, SubstituteTypeParameters(check.subject, substitutions), substitutions, &issues));
            std::ranges::for_each(issues, emit);
        }
    }
    if (const auto it = deferredMembershipChecks.find(&declaration); it != deferredMembershipChecks.end()) {
        for (const DeferredMembershipCheck &check : it->second) {
            if (std::optional<PatternIssue> issue =
                    MembershipIssue(SubstituteTypeParameters(check.subject, substitutions),
                                    SubstituteTypeParameters(check.tested, substitutions), check.location)) {
                emit(std::move(*issue));
            }
        }
    }
}

void AnalysisContext::NoteDeferredNativeType(const TypeRef &type) {
    if (!currentFunctionDecl || !MentionsNativeType(type) || !MentionsTypeParameter(type)) {
        return;
    }
    std::vector<TypeRef> &noted = deferredNativeTypes[currentFunctionDecl];
    if (std::ranges::find(noted, type) == noted.end()) {
        noted.push_back(type);
    }
}

const TypeRef *AnalysisContext::TypedPatternAnnotation(const TypedPattern &pattern) const {
    const auto found = typedPatternTypes.find(&pattern);
    return found == typedPatternTypes.end() ? nullptr : &found->second;
}
} // namespace Rux::SemanticDetail

// The first-party package documentation house style, checked over the compiler's own syntax model.
//
// Docs/PackageStyle.md is the contract; this file is its enforcement. Everything here reads lexer comment trivia,
// parsed declarations and Syntax::Documentation metadata, never a regular expression over Rux source, so a rule
// cannot drift from what the compiler actually saw.
//
// The rules deliberately do not repeat what rux fmt, rux lint and rux doc already own. They cover what those three
// leave open: complete parameter and return tag coverage, structured @see URL ownership, tag ordering, meaningful
// summaries, ordinary file headers, and the repository's prose dash convention.
//
// Sources predating the style would fail in the thousands at once, so accepted violations live in an explicit
// baseline keyed by declaration identity and a fingerprint of the exact documentation and signature. The baseline
// may only shrink: an unrecorded violation fails, an entry whose fingerprint moved on no longer excuses anything,
// and an entry matching nothing at all fails as stale.

#include "Lexer/Lexer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {

// --- Violations ---------------------------------------------------------------------------------------------------

/// One rule failure, identified well enough that a baseline entry survives unrelated edits to the same file but not
/// an edit to the declaration itself.
struct Violation {
    std::string rule;
    std::string file; // Packages-relative, forward slashes
    std::string kind;
    std::string name;
    std::string fingerprint;
    std::string detail;

    /// The baseline key. `detail` is deliberately excluded so that improving a message does not invalidate every
    /// recorded entry.
    [[nodiscard]] std::string Key() const {
        return rule + "|" + file + "|" + kind + "|" + name + "|" + fingerprint;
    }
};

/// A stable, short digest of the text a violation was recorded against. FNV-1a is enough here: this is change
/// detection for a checked-in baseline, not a security boundary.
std::string Fingerprint(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::string digits;
    for (int shift = 60; shift >= 0; shift -= 4) {
        digits.push_back("0123456789abcdef"[(hash >> shift) & 0xF]);
    }
    return digits;
}

// --- Source inspection --------------------------------------------------------------------------------------------

std::string ReadFileText(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

/// Every maintained package source, in a stable order. Only authored sources count: a `Bin` directory below a package
/// holds build output, which is untracked and must never be held to the authored style.
std::vector<std::filesystem::path> PackageSources() {
    std::vector<std::filesystem::path> sources;
    const auto packages = std::filesystem::weakly_canonical(std::filesystem::path(RUX_PACKAGES_DIR));
    for (auto entry = std::filesystem::recursive_directory_iterator(packages);
         entry != std::filesystem::recursive_directory_iterator(); ++entry) {
        if (entry->is_directory() && entry->path().filename() == "Bin") {
            entry.disable_recursion_pending();
            continue;
        }
        if (entry->is_regular_file() && entry->path().extension() == ".rux") {
            sources.push_back(entry->path());
        }
    }
    std::ranges::sort(sources);
    return sources;
}

std::string RelativeName(const std::filesystem::path &path) {
    const auto packages = std::filesystem::weakly_canonical(std::filesystem::path(RUX_PACKAGES_DIR));
    auto relative = path.lexically_relative(packages).generic_string();
    return relative;
}

/// True for a generated data file, which carries its generator's header rather than an authored one and whose tables
/// are never reflowed by hand.
bool IsGenerated(std::string_view text) {
    const auto head = text.substr(0, std::min<std::size_t>(text.size(), 600));
    return head.find("generated") != std::string_view::npos || head.find("Generated") != std::string_view::npos;
}

// --- The declarations a rule applies to -------------------------------------------------------------------------

/// One documented declaration flattened out of the AST, carrying only what the rules ask about. Collecting first
/// keeps each rule a straight loop rather than another recursive walk.
struct Subject {
    std::string kind;
    std::string name;
    const Syntax::Documentation *documentation = nullptr;
    std::vector<std::string> parameters;     // named, receiver excluded
    std::vector<std::string> typeParameters; //
    bool returnsValue = false;
    bool isDestructor = false;
    bool isNoReturn = false;
    bool isPublic = false;
    bool requiresUrl = true; // fields and cases may rely on their containing type's URL
    std::string signature;   // fingerprint input alongside the documentation
};

std::string SignatureOf(const FuncDecl &function) {
    std::string signature = function.name;
    signature.push_back('(');
    for (const auto &parameter : function.params) {
        signature += parameter.name;
        signature.push_back(',');
    }
    signature.push_back(')');
    if (function.returnType.has_value()) {
        signature += "->value";
    }
    return signature;
}

void CollectFunction(const FuncDecl &function, bool inheritedPublic, std::vector<Subject> &subjects) {
    if (!function.isPublic && !inheritedPublic) {
        return;
    }
    Subject subject;
    subject.kind = function.isAsm ? "asm" : "func";
    subject.name = function.name;
    subject.documentation = &function.documentation;
    subject.isPublic = true;
    subject.isDestructor = !function.name.empty() && function.name.front() == '~';
    subject.isNoReturn = function.isNoReturn;
    subject.returnsValue = function.returnType.has_value();
    subject.signature = SignatureOf(function);
    for (const auto &parameter : function.params) {
        if (!parameter.IsReceiver() && !parameter.isVariadic && !parameter.name.empty()) {
            subject.parameters.push_back(parameter.name);
        }
    }
    for (const auto &typeParameter : function.typeParams) {
        subject.typeParameters.push_back(typeParameter.name);
    }
    subjects.push_back(std::move(subject));
}

void CollectDecl(const Decl &declaration, bool inheritedPublic, std::vector<Subject> &subjects);

void CollectImpl(const ImplDecl &block, std::vector<Subject> &subjects) {
    for (const auto &method : block.methods) {
        CollectFunction(*method, false, subjects);
    }
    for (const auto &constant : block.constants) {
        CollectDecl(*constant, false, subjects);
    }
}

void CollectDecl(const Decl &declaration, const bool inheritedPublic, std::vector<Subject> &subjects) {
    const bool visible = declaration.isPublic || inheritedPublic;

    if (const auto *function = dynamic_cast<const FuncDecl *>(&declaration)) {
        CollectFunction(*function, inheritedPublic, subjects);
        return;
    }
    if (const auto *record = dynamic_cast<const StructDecl *>(&declaration)) {
        if (visible) {
            Subject subject;
            subject.kind = "struct";
            subject.name = record->name;
            subject.documentation = &record->documentation;
            subject.isPublic = true;
            subject.signature = record->name;
            for (const auto &typeParameter : record->typeParams) {
                subject.typeParameters.push_back(typeParameter.name);
            }
            subjects.push_back(std::move(subject));
        }
        for (const auto &field : record->fields) {
            if (!field.isPublic) {
                continue;
            }
            Subject subject;
            subject.kind = "field";
            subject.name = record->name + "." + field.name;
            subject.documentation = &field.documentation;
            subject.isPublic = true;
            subject.requiresUrl = false;
            subject.signature = subject.name;
            subjects.push_back(std::move(subject));
        }
        return;
    }
    if (const auto *enumeration = dynamic_cast<const EnumDecl *>(&declaration)) {
        if (visible) {
            Subject subject;
            subject.kind = enumeration->IsVariant() ? "variant" : "enum";
            subject.name = enumeration->name;
            subject.documentation = &enumeration->documentation;
            subject.isPublic = true;
            subject.signature = enumeration->name;
            for (const auto &typeParameter : enumeration->typeParams) {
                subject.typeParameters.push_back(typeParameter.name);
            }
            subjects.push_back(std::move(subject));
        }
        if (visible) {
            for (const auto &variant : enumeration->variants) {
                Subject subject;
                subject.kind = "case";
                subject.name = enumeration->name + "::" + variant.name;
                subject.documentation = &variant.documentation;
                subject.isPublic = true;
                subject.requiresUrl = false;
                subject.signature = subject.name;
                subjects.push_back(std::move(subject));
            }
        }
        return;
    }
    if (const auto *interface = dynamic_cast<const InterfaceDecl *>(&declaration)) {
        if (visible) {
            Subject subject;
            subject.kind = "interface";
            subject.name = interface->name;
            subject.documentation = &interface->documentation;
            subject.isPublic = true;
            subject.signature = interface->name;
            subjects.push_back(std::move(subject));
        }
        for (const auto &method : interface->methods) {
            CollectFunction(*method, visible, subjects);
        }
        return;
    }
    if (const auto *block = dynamic_cast<const ImplDecl *>(&declaration)) {
        CollectImpl(*block, subjects);
        return;
    }
    // A type alias and a constant carry the same requirements: a real summary and a canonical URL, with no
    // parameters, type parameters or return to document.
    const auto collectNamed = [&](std::string kind, const std::string &name, const Syntax::Documentation &attached) {
        Subject subject;
        subject.kind = std::move(kind);
        subject.name = name;
        subject.documentation = &attached;
        subject.isPublic = true;
        subject.signature = name;
        subjects.push_back(std::move(subject));
    };

    if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&declaration)) {
        if (visible) {
            collectNamed("type", alias->name, alias->documentation);
        }
        return;
    }
    if (const auto *constant = dynamic_cast<const ConstDecl *>(&declaration)) {
        if (visible) {
            collectNamed("const", constant->name, constant->documentation);
        }
        return;
    }
    if (const auto *external = dynamic_cast<const ExternFuncDecl *>(&declaration)) {
        if (!visible) {
            return;
        }
        Subject subject;
        subject.kind = "extern";
        subject.name = external->name;
        subject.documentation = &external->documentation;
        subject.isPublic = true;
        subject.isNoReturn = external->isNoReturn;
        subject.returnsValue = external->returnType.has_value();
        subject.signature = external->name;
        for (const auto &parameter : external->params) {
            if (!parameter.isVariadic && !parameter.name.empty()) {
                subject.parameters.push_back(parameter.name);
                subject.signature += "," + parameter.name;
            }
        }
        subjects.push_back(std::move(subject));
        return;
    }
    if (const auto *externalBlock = dynamic_cast<const ExternBlockDecl *>(&declaration)) {
        for (const auto &item : externalBlock->items) {
            CollectDecl(*item, visible, subjects);
        }
        return;
    }
    if (const auto *module = dynamic_cast<const ModuleDecl *>(&declaration)) {
        for (const auto &item : module->items) {
            CollectDecl(*item, visible, subjects);
        }
        return;
    }
    if (const auto *conditional = dynamic_cast<const WhenDecl *>(&declaration)) {
        for (const auto &branch : conditional->branches) {
            for (const auto &item : branch.items) {
                CollectDecl(*item, inheritedPublic, subjects);
            }
        }
    }
}

// --- The rules ----------------------------------------------------------------------------------------------------

bool HasTag(const Syntax::Documentation &documentation, const Syntax::DocumentationTagKind kind,
            std::string_view subject) {
    return std::ranges::any_of(documentation.tags, [&](const Syntax::DocumentationTag &tag) {
        return tag.kind == kind && tag.subject == subject;
    });
}

bool HasAnyTag(const Syntax::Documentation &documentation, const Syntax::DocumentationTagKind kind) {
    return std::ranges::any_of(documentation.tags,
                               [&](const Syntax::DocumentationTag &tag) { return tag.kind == kind; });
}

/// The required terminal ordering: @typeParam, then @param, then @returns, then @see. @deprecated may sit anywhere in
/// the block, so it is not given a rank.
int TagRank(const Syntax::DocumentationTagKind kind) {
    switch (kind) {
    case Syntax::DocumentationTagKind::TypeParameter:
        return 0;
    case Syntax::DocumentationTagKind::Parameter:
        return 1;
    case Syntax::DocumentationTagKind::Returns:
        return 2;
    case Syntax::DocumentationTagKind::See:
        return 3;
    case Syntax::DocumentationTagKind::Deprecated:
        return -1;
    }
    return -1;
}

/// A summary that only restates the declaration's own name tells a reader nothing the signature did not. Compared
/// with separators and case removed, so "Reads a byte." against ReadByte is caught.
bool SummaryRestatesName(std::string_view summary, std::string_view name) {
    const auto reduce = [](std::string_view text) {
        std::string reduced;
        for (const unsigned char character : text) {
            if (std::isalnum(character) != 0) {
                reduced.push_back(static_cast<char>(std::tolower(character)));
            }
        }
        return reduced;
    };
    const auto reducedName = reduce(name);
    return !reducedName.empty() && reduce(summary) == reducedName;
}

/// The prose dash convention. A double hyphen is punctuation only when it stands alone between spaces; `--jobs` and
/// `git checkout -- Path` are literal syntax and are left alone.
bool UsesProseDoubleHyphen(std::string_view text) {
    for (std::size_t index = text.find(" -- "); index != std::string_view::npos; index = text.find(" -- ", index + 1)) {
        const auto lineStart = text.rfind('\n', index);
        const auto line = text.substr(lineStart == std::string_view::npos ? 0 : lineStart + 1, index);
        // A command line showing an option-and-path argument is the one legitimate spaced form.
        if (line.find("git ") == std::string_view::npos && line.find("rux ") == std::string_view::npos) {
            return true;
        }
    }
    return false;
}

void CheckSubject(const Subject &subject, const std::string &file, std::vector<Violation> &violations) {
    const auto &documentation = *subject.documentation;
    const auto fingerprint = Fingerprint(documentation.markdown + "\x1f" + subject.signature);
    const auto report = [&](std::string rule, std::string detail) {
        violations.push_back(
            Violation{std::move(rule), file, subject.kind, subject.name, fingerprint, std::move(detail)});
    };

    if (!documentation.Present() && documentation.tags.empty()) {
        report("missing-documentation", "public declaration carries no documentation");
        return;
    }

    const auto summary = documentation.Summary();
    if (summary.empty()) {
        report("empty-summary", "documentation has tags but no summary");
    }
    else if (SummaryRestatesName(summary, subject.name)) {
        report("hollow-summary", "summary only restates the declaration name");
    }

    for (const auto &parameter : subject.parameters) {
        if (!HasTag(documentation, Syntax::DocumentationTagKind::Parameter, parameter)) {
            report("missing-param", "no @param for '" + parameter + "'");
        }
    }
    for (const auto &typeParameter : subject.typeParameters) {
        if (!HasTag(documentation, Syntax::DocumentationTagKind::TypeParameter, typeParameter)) {
            report("missing-typeparam", "no @typeParam for '" + typeParameter + "'");
        }
    }

    const bool returnsRequired = subject.returnsValue && !subject.isDestructor && !subject.isNoReturn;
    const bool hasReturns = HasAnyTag(documentation, Syntax::DocumentationTagKind::Returns);
    if (returnsRequired && !hasReturns) {
        report("missing-returns", "value-returning callable has no @returns");
    }
    if (subject.isDestructor && hasReturns) {
        report("destructor-returns", "a destructor must not document @returns");
    }

    if (subject.requiresUrl && !HasAnyTag(documentation, Syntax::DocumentationTagKind::See)) {
        report("missing-see", "no canonical @see URL");
    }

    int highest = -1;
    for (const auto &tag : documentation.tags) {
        const int rank = TagRank(tag.kind);
        if (rank < 0) {
            continue;
        }
        if (rank < highest) {
            report("tag-order", "tags are not in @typeParam, @param, @returns, @see order");
            break;
        }
        highest = rank;
    }

    if (UsesProseDoubleHyphen(documentation.markdown)) {
        report("prose-dash", "documentation prose uses '--' where an em dash belongs");
    }
}

/// Everything a file is checked for that is not attached to one declaration.
void CheckFileShape(const std::string &file, const std::string &text, const LexerResult &lexed,
                    std::vector<Violation> &violations) {
    const auto fingerprint = Fingerprint(text.substr(0, std::min<std::size_t>(text.size(), 400)));
    const auto report = [&](std::string rule, std::string detail) {
        violations.push_back(Violation{std::move(rule), file, "file", "", fingerprint, std::move(detail)});
    };

    const auto *header = lexed.comments.empty() ? nullptr : &lexed.comments.front();
    if (header == nullptr || header->range.start.line != 1 || header->kind != CommentKind::Line) {
        report("missing-header", "no ordinary // header on the first line");
    }

    if (text.find("\r\n") != std::string::npos) {
        report("crlf", "file uses CRLF line endings");
    }

    for (const auto &comment : lexed.comments) {
        if (comment.kind == CommentKind::Line && UsesProseDoubleHyphen(comment.raw)) {
            report("prose-dash", "a comment uses '--' where an em dash belongs");
            break;
        }
    }
}

std::vector<Violation> InspectSource(const std::string &file, const std::string &text) {
    std::vector<Violation> violations;

    Lexer lexer(text, file);
    auto lexed = lexer.Tokenize();
    Parser parser(std::move(lexed.tokens), file);
    auto parsed = parser.Parse();

    if (!IsGenerated(text)) {
        CheckFileShape(file, text, lexed, violations);
    }

    std::vector<Subject> subjects;
    for (const auto &item : parsed.module.items) {
        CollectDecl(*item, false, subjects);
    }
    for (const auto &subject : subjects) {
        CheckSubject(subject, file, violations);
    }
    return violations;
}

/// Inspect one inline fixture. Fixtures are named so a failure says which rule's fixture broke.
std::vector<std::string> RulesFiredBy(const std::string &source) {
    const auto violations = InspectSource("fixture.rux", source);
    std::vector<std::string> rules;
    rules.reserve(violations.size());
    for (const auto &violation : violations) {
        rules.push_back(violation.rule);
    }
    std::ranges::sort(rules);
    rules.erase(std::ranges::unique(rules).begin(), rules.end());
    return rules;
}

bool Fired(const std::vector<std::string> &rules, std::string_view rule) {
    return std::ranges::find(rules, rule) != rules.end();
}

// --- The legacy baseline ------------------------------------------------------------------------------------------

std::filesystem::path BaselinePath() {
    return std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "Unit" / "PackageDocumentationStyleBaseline.txt";
}

std::set<std::string> LoadBaseline() {
    std::set<std::string> entries;
    std::ifstream input(BaselinePath());
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        entries.insert(line);
    }
    return entries;
}

} // namespace

// --- Fixtures: each rule fires exactly when it should ---------------------------------------------------------------

TEST_CASE("A source meeting the house style produces no violation") {
    const std::string source = "// Reads values out of a buffer.\n"
                               "\n"
                               "import Core::Option;\n"
                               "\n"
                               "/// Decodes one value from `bytes`, starting at `offset`.\n"
                               "///\n"
                               "/// @typeParam T The decoded value type.\n"
                               "/// @param bytes The bytes to read.\n"
                               "/// @param offset Where in `bytes` to start.\n"
                               "/// @returns The decoded value, or None when the bytes run out.\n"
                               "/// @see https://rux-lang.dev/docs/api/example/decode\n"
                               "pub func Decode<T>(bytes: char8[..], offset: uint) -> Option<T>;\n";
    CHECK(RulesFiredBy(source).empty());
}

TEST_CASE("A named parameter without an at-param tag is reported") {
    const std::string source = "// Header.\n"
                               "\n"
                               "/// Decodes a value.\n"
                               "///\n"
                               "/// @param bytes The bytes to read.\n"
                               "/// @returns The value.\n"
                               "/// @see https://rux-lang.dev/docs/api/example/decode\n"
                               "pub func Decode(bytes: char8[..], offset: uint) -> uint;\n";
    const auto rules = RulesFiredBy(source);
    CHECK(Fired(rules, "missing-param"));
    CHECK_FALSE(Fired(rules, "missing-returns"));
}

TEST_CASE("A receiver needs no at-param tag") {
    const std::string source = "// Header.\n"
                               "\n"
                               "pub struct Reader { }\n"
                               "\n"
                               "extend Reader {\n"
                               "    /// Reports how many bytes remain.\n"
                               "    ///\n"
                               "    /// @returns The remaining count.\n"
                               "    /// @see https://rux-lang.dev/docs/api/example/reader/remaining\n"
                               "    pub func Remaining(self: &Reader) -> uint;\n"
                               "}\n";
    const auto rules = RulesFiredBy(source);
    CHECK_FALSE(Fired(rules, "missing-param"));
}

TEST_CASE("A type parameter without an at-typeParam tag is reported") {
    const std::string source = "// Header.\n"
                               "\n"
                               "/// Holds one value.\n"
                               "///\n"
                               "/// @see https://rux-lang.dev/docs/api/example/box\n"
                               "pub struct Box<T> { }\n";
    CHECK(Fired(RulesFiredBy(source), "missing-typeparam"));
}

TEST_CASE("A value-returning callable without at-returns is reported, and a destructor with one is too") {
    const std::string missing = "// Header.\n"
                                "\n"
                                "/// Reports the current length in bytes.\n"
                                "///\n"
                                "/// @see https://rux-lang.dev/docs/api/example/length\n"
                                "pub func Length() -> uint;\n";
    CHECK(Fired(RulesFiredBy(missing), "missing-returns"));

    const std::string voidReturning = "// Header.\n"
                                      "\n"
                                      "/// Releases the buffer back to its allocator.\n"
                                      "///\n"
                                      "/// @see https://rux-lang.dev/docs/api/example/reset\n"
                                      "pub func Reset();\n";
    CHECK_FALSE(Fired(RulesFiredBy(voidReturning), "missing-returns"));
}

TEST_CASE("A public declaration without a canonical at-see URL is reported") {
    const std::string source = "// Header.\n"
                               "\n"
                               "/// Reports the current length in bytes.\n"
                               "///\n"
                               "/// @returns The length.\n"
                               "pub func Length() -> uint;\n";
    CHECK(Fired(RulesFiredBy(source), "missing-see"));
}

TEST_CASE("A bare URL written as prose does not satisfy the at-see requirement") {
    const std::string source = "// Header.\n"
                               "\n"
                               "/// Reports the current length in bytes.\n"
                               "///\n"
                               "/// https://rux-lang.dev/docs/api/example/length\n"
                               "pub func Length() -> uint;\n";
    CHECK(Fired(RulesFiredBy(source), "missing-see"));
}

TEST_CASE("Tags out of the required order are reported") {
    const std::string source = "// Header.\n"
                               "\n"
                               "/// Decodes a value.\n"
                               "///\n"
                               "/// @returns The value.\n"
                               "/// @param bytes The bytes to read.\n"
                               "/// @see https://rux-lang.dev/docs/api/example/decode\n"
                               "pub func Decode(bytes: char8[..]) -> uint;\n";
    CHECK(Fired(RulesFiredBy(source), "tag-order"));
}

TEST_CASE("A summary that only restates the declaration name is reported") {
    const std::string source = "// Header.\n"
                               "\n"
                               "/// Read byte.\n"
                               "///\n"
                               "/// @returns The byte.\n"
                               "/// @see https://rux-lang.dev/docs/api/example/read-byte\n"
                               "pub func ReadByte() -> char8;\n";
    CHECK(Fired(RulesFiredBy(source), "hollow-summary"));
}

TEST_CASE("A missing file header is reported and a present one is not") {
    const std::string without = "import Core::Option;\n";
    CHECK(Fired(RulesFiredBy(without), "missing-header"));

    const std::string with = "// Explains what this file owns.\n"
                             "\n"
                             "import Core::Option;\n";
    CHECK_FALSE(Fired(RulesFiredBy(with), "missing-header"));
}

TEST_CASE("A documentation comment does not count as the file header") {
    const std::string source = "/// Attached to the declaration, not the file.\n"
                               "///\n"
                               "/// @see https://rux-lang.dev/docs/api/example/value\n"
                               "pub const Value: uint = 1;\n";
    CHECK(Fired(RulesFiredBy(source), "missing-header"));
}

TEST_CASE("A double hyphen is prose punctuation but an option is not") {
    const std::string prose = "// A header whose aside -- like this one -- uses the wrong dash.\n"
                              "\n"
                              "import Core::Option;\n";
    CHECK(Fired(RulesFiredBy(prose), "prose-dash"));

    const std::string option = "// Build this package with rux build -- release to see the effect.\n"
                               "\n"
                               "import Core::Option;\n";
    CHECK_FALSE(Fired(RulesFiredBy(option), "prose-dash"));
}

TEST_CASE("A private declaration is not held to the public API style") {
    const std::string source = "// Header.\n"
                               "\n"
                               "func Helper(value: uint) -> uint;\n";
    CHECK(RulesFiredBy(source).empty());
}

// --- The repository itself ------------------------------------------------------------------------------------------

TEST_CASE("Every maintained package source meets the documentation house style") {
    const auto sources = PackageSources();
    REQUIRE_MESSAGE(sources.size() > 1, "no package sources found below ", RUX_PACKAGES_DIR);

    const auto baseline = LoadBaseline();
    std::vector<Violation> unrecorded;
    std::set<std::string> matched;

    for (const auto &path : sources) {
        const auto file = RelativeName(path);
        for (const auto &violation : InspectSource(file, ReadFileText(path))) {
            const auto key = violation.Key();
            if (baseline.contains(key)) {
                matched.insert(key);
            }
            else {
                unrecorded.push_back(violation);
            }
        }
    }

    // The message names a bounded sample: a full dump of a large regression is unreadable in CI output, and the
    // count is what says how much work arrived.
    if (!unrecorded.empty()) {
        std::ostringstream message;
        message << unrecorded.size() << " unrecorded style violation(s). New or changed declarations must meet "
                << "Docs/PackageStyle.md; the baseline may only shrink. First few:\n";
        for (std::size_t index = 0; index < std::min<std::size_t>(unrecorded.size(), 20); ++index) {
            const auto &violation = unrecorded[index];
            message << "  " << violation.file << ": " << violation.kind << ' ' << violation.name << ": "
                    << violation.rule << " (" << violation.detail << ")\n";
        }
        FAIL_CHECK(message.str());
    }

    std::vector<std::string> stale;
    for (const auto &entry : baseline) {
        if (!matched.contains(entry)) {
            stale.push_back(entry);
        }
    }
    if (!stale.empty()) {
        std::ostringstream message;
        message << stale.size() << " baseline entr(ies) matched nothing. Delete an entry once its declaration is "
                << "fixed or removed. First few:\n";
        for (std::size_t index = 0; index < std::min<std::size_t>(stale.size(), 20); ++index) {
            message << "  " << stale[index] << '\n';
        }
        FAIL_CHECK(message.str());
    }
}

// Skipped by default, because a test that rewrites checked-in files must never run as part of an ordinary suite.
// Run it deliberately to rebuild the baseline after a bulk documentation migration:
//
//   ./Bin/Tests/Unit/rux-tests --test-case="Regenerate*" --no-skip
//
// Then read the diff. The entries it removes are the work that was done; entries it adds are new debt and should be
// fixed rather than recorded.
TEST_CASE("Regenerate the package documentation style baseline" * doctest::skip()) {
    std::set<std::string> keys;
    for (const auto &path : PackageSources()) {
        const auto file = RelativeName(path);
        for (const auto &violation : InspectSource(file, ReadFileText(path))) {
            keys.insert(violation.Key());
        }
    }

    std::ofstream output(BaselinePath(), std::ios::binary);
    REQUIRE(output.is_open());
    output << "# Legacy documentation-style violations accepted when Docs/PackageStyle.md was introduced.\n"
           << "# Format: rule|file|kind|name|fingerprint   See Docs/PackageStyle.md, \"The legacy baseline\".\n"
           << "# This file may only shrink. Delete an entry once its declaration meets the style or is removed.\n"
           << "# Regenerate with: rux-tests --test-case=\"Regenerate*\" --no-skip\n";
    for (const auto &key : keys) {
        output << key << '\n';
    }
    MESSAGE("wrote " << keys.size() << " baseline entries");
}

TEST_CASE("The legacy baseline is well formed and only shrinks") {
    const auto path = BaselinePath();
    REQUIRE_MESSAGE(std::filesystem::exists(path), "the baseline file must exist: ", path.string());

    const auto baseline = LoadBaseline();
    for (const auto &entry : baseline) {
        // rule|file|kind|name|fingerprint
        CHECK_MESSAGE(std::ranges::count(entry, '|') == 4, "malformed baseline entry: ", entry);
        CHECK_MESSAGE(entry.size() > 16, "truncated baseline entry: ", entry);
    }
}

// The POSIX binding contract: three platform packages, one wrapper vocabulary, three sets of numbers.
//
// Rux/Linux, Rux/macOS and Rux/FreeBSD bind the same interface to three kernels. What each system numbers is its
// own — AT_FDCWD is -2 on Darwin and -100 elsewhere, EAGAIN is 11 on Linux and 35 on both BSDs — and none of that
// may be unified. What must not differ is the vocabulary above the numbers: the same call reached through the same
// wrapper name, taking the same parameters in the same shape, so a consumer such as Rux/FileSystem or Rux/Time
// writes one call site rather than a `when #target.os` ladder around three spellings of the same idea.
//
// That property decays silently, and had. Linux exported `NewFstatAt` where the other two exported `FstatAt`, and
// Rux/FileSystem carried a target branch for the difference. macOS took its clock records by reference where the
// other two took addresses, and Rux/Time carried four target branches for that. Neither divergence broke a build;
// both simply made every consumer pay. So the check belongs here, over the parsed sources, rather than in a review
// habit.
//
// The three checks are deliberately different in kind. Wrapper names and parameter shapes must agree exactly, and
// are compared across the three packages. Reference parameters are refused outright, because the packages document
// raw pointers as their convention and a reference in a binding is how the macOS divergence started. Shared errno
// names must be declared by all three while their values stay each system's own, so the names are compared and the
// values are asserted against what that kernel publishes — a name agreeing while its number quietly does not is
// the one hazard alignment introduces that the divergence did not have.
//
// Rux/Windows is not part of any of this. It binds a different interface, and the audit in Docs/Packages.md records
// the one thing that must stay true of it: it imports nothing from Core and must not gain a manifest edge to Core
// for the sake of a tidier dependency diagram.

#include "Lexer/Lexer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <cstdlib>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {

// --- Reading a platform package -------------------------------------------------------------------------------

/// The three packages that bind a POSIX kernel. Windows is excluded on purpose: it binds Win32, which shares no
/// call, no error domain and no parameter convention with these.
const std::vector<std::string> &PosixPackages() {
    static const std::vector<std::string> packages = {"Linux", "macOS", "FreeBSD"};
    return packages;
}

std::string ReadFileText(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::filesystem::path PackageSourceDirectory(const std::string &package) {
    return std::filesystem::path(RUX_PACKAGES_DIR) / package / "Src";
}

std::vector<std::filesystem::path> PackageSources(const std::string &package) {
    std::vector<std::filesystem::path> sources;
    for (const auto &entry : std::filesystem::directory_iterator(PackageSourceDirectory(package))) {
        if (entry.is_regular_file() && entry.path().extension() == ".rux") {
            sources.push_back(entry.path());
        }
    }
    std::ranges::sort(sources);
    return sources;
}

/// How a type is spelled, reduced to what a binding contract cares about: the qualifier chain and the leaf name.
/// `*var Timespec` and `&var Timespec` differ here, which is the point; `uint16` and `uint32` behind a shared alias
/// do not, because both spell `FileMode` and the alias is where each system's width belongs.
std::string Spell(const TypeExpr *type) {
    if (type == nullptr) {
        return "?";
    }
    if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(type)) {
        return pointer->pointeeMut ? "*var " + Spell(pointer->pointee.get()) : "*" + Spell(pointer->pointee.get());
    }
    if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(type)) {
        return reference->pointeeMut ? "&var " + Spell(reference->pointee.get())
                                     : "&" + Spell(reference->pointee.get());
    }
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(type)) {
        return named->name;
    }
    if (const auto *path = dynamic_cast<const PathTypeExpr *>(type)) {
        return path->segments.empty() ? "?" : path->segments.back();
    }
    if (const auto *slice = dynamic_cast<const SliceTypeExpr *>(type)) {
        return (slice->elementMut ? "[var " : "[") + Spell(slice->element.get()) + "]";
    }
    if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(type)) {
        return Spell(array->element.get()) + "[]";
    }
    return "?";
}

/// One public function, reduced to the part a consumer writes: its parameters, named and shaped. The return type is
/// excluded because every wrapper in all three packages already returns `int64`, and including it would only make a
/// mismatch message longer.
std::string SignatureOf(const FuncDecl &function) {
    std::string signature = "(";
    for (std::size_t index = 0; index < function.params.size(); ++index) {
        if (index > 0) {
            signature += ", ";
        }
        signature += function.params[index].name + ": " + Spell(function.params[index].type.get());
    }
    signature += ")";
    return signature;
}

/// The integer a constant is initialized to, when it is a plain literal. Every errno constant is one; anything else
/// reports nullopt and the caller says so rather than guessing.
std::optional<long long> LiteralValue(const Expr *value) {
    // `AtFdCwd` is written `-100`, which parses as a negation of a literal rather than as one. Nothing else in these
    // sources needs an operator folded, so this handles the one that appears and refuses the rest.
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(value)) {
        if (unary->op != TokenKind::Minus) {
            return std::nullopt;
        }
        const auto operand = LiteralValue(unary->operand.get());
        return operand.has_value() ? std::optional<long long>(-*operand) : std::nullopt;
    }
    const auto *literal = dynamic_cast<const LiteralExpr *>(value);
    if (literal == nullptr) {
        return std::nullopt;
    }
    std::string text;
    for (const char character : literal->token.text) {
        if (character != '_') {
            text.push_back(character);
        }
    }
    if (text.empty()) {
        return std::nullopt;
    }
    // Unsigned, then reinterpreted. `InvalidHandleValue` is written `0xFFFFFFFFFFFFFFFF`, which a signed parse
    // saturates to the largest positive value rather than reading as the all-ones pattern it is. Every other
    // constant here is small enough that the two readings agree.
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 0);
    if (end == text.c_str()) {
        return std::nullopt;
    }
    return static_cast<long long>(parsed);
}

/// Everything one platform package publishes that this file has an opinion about.
struct PackageSurface {
    std::map<std::string, std::string> functions; // name -> parameter signature
    std::map<std::string, std::string> functionFiles;
    std::map<std::string, long long> constants; // name -> literal value
    std::set<std::string> types;
    std::vector<std::string> referenceParameters; // "File.rux: Read(buffer)" for anything taking &T or &var T
};

void CollectSurface(const std::string &file, const Decl &declaration, PackageSurface &surface) {
    if (const auto *function = dynamic_cast<const FuncDecl *>(&declaration)) {
        for (const auto &parameter : function->params) {
            if (dynamic_cast<const ReferenceTypeExpr *>(parameter.type.get()) != nullptr) {
                surface.referenceParameters.push_back(file + ": " + function->name + "(" + parameter.name + ")");
            }
        }
        if (function->isPublic) {
            surface.functions[function->name] = SignatureOf(*function);
            surface.functionFiles[function->name] = file;
        }
        return;
    }
    if (const auto *external = dynamic_cast<const ExternFuncDecl *>(&declaration)) {
        for (const auto &parameter : external->params) {
            if (dynamic_cast<const ReferenceTypeExpr *>(parameter.type.get()) != nullptr) {
                surface.referenceParameters.push_back(file + ": extern " + external->name + "(" + parameter.name + ")");
            }
        }
        return;
    }
    if (const auto *constant = dynamic_cast<const ConstDecl *>(&declaration)) {
        if (constant->isPublic) {
            if (const auto value = LiteralValue(constant->value.get())) {
                surface.constants[constant->name] = *value;
            }
        }
        return;
    }
    if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&declaration)) {
        if (alias->isPublic) {
            surface.types.insert(alias->name);
        }
        return;
    }
    if (const auto *record = dynamic_cast<const StructDecl *>(&declaration)) {
        if (record->isPublic) {
            surface.types.insert(record->name);
        }
        return;
    }
    if (const auto *conditional = dynamic_cast<const WhenDecl *>(&declaration)) {
        for (const auto &branch : conditional->branches) {
            for (const auto &item : branch.items) {
                CollectSurface(file, *item, surface);
            }
        }
        return;
    }
    if (const auto *block = dynamic_cast<const ExternBlockDecl *>(&declaration)) {
        for (const auto &item : block->items) {
            CollectSurface(file, *item, surface);
        }
    }
}

PackageSurface SurfaceOf(const std::string &package) {
    PackageSurface surface;
    for (const auto &path : PackageSources(package)) {
        const auto file = path.filename().string();
        const auto text = ReadFileText(path);
        Lexer lexer(text, file);
        auto lexed = lexer.Tokenize();
        Parser parser(std::move(lexed.tokens), file);
        auto parsed = parser.Parse();
        for (const auto &item : parsed.module.items) {
            CollectSurface(file, *item, surface);
        }
    }
    return surface;
}

const PackageSurface &Surface(const std::string &package) {
    static std::map<std::string, PackageSurface> cache;
    const auto found = cache.find(package);
    if (found != cache.end()) {
        return found->second;
    }
    return cache.emplace(package, SurfaceOf(package)).first->second;
}

// --- The contract ----------------------------------------------------------------------------------------------

/// The wrappers every POSIX kernel this repository binds actually has, and which every consumer therefore calls
/// unconditionally. A call one system lacks is not here: Linux has no `Dup2` and macOS has no `Pipe2` or `Brk`, and
/// those absences are real rather than an oversight to be papered over with an emulating wrapper.
const std::vector<std::string> &SharedWrappers() {
    static const std::vector<std::string> wrappers = {"Chdir",        "ClockGetResolution",
                                                      "ClockGetTime", "Close",
                                                      "Errno",        "Exit",
                                                      "FchmodAt",     "Fstat",
                                                      "FstatAt",      "Fsync",
                                                      "Ftruncate",    "Getcwd",
                                                      "GetPid",       "IsError",
                                                      "LinkAt",       "Lseek",
                                                      "MkdirAt",      "Madvise",
                                                      "Mmap",         "Mprotect",
                                                      "Munmap",       "Nanosleep",
                                                      "OpenAt",       "QueryPageSize",
                                                      "Read",         "ReadlinkAt",
                                                      "RenameAt",     "SymlinkAt",
                                                      "UnlinkAt",     "UtimensAt",
                                                      "Write"};
    return wrappers;
}

/// The errno names all three declare. Each keeps the kernel's own spelling, and each system's number is its own:
/// the low ones descend from the same BSD table and agree, everything above `EPIPE` diverges.
const std::vector<std::string> &SharedErrnoNames() {
    static const std::vector<std::string> names = {
        "EPERM",  "ENOENT", "ESRCH", "EINTR",  "EIO",          "ENXIO",   "EBADF",  "EAGAIN",    "ENOMEM",
        "EACCES", "EFAULT", "EBUSY", "EEXIST", "EXDEV",        "ENOTDIR", "EISDIR", "EINVAL",    "EMFILE",
        "ENOSPC", "ESPIPE", "EROFS", "EPIPE",  "ENAMETOOLONG", "ENOSYS",  "ELOOP",  "EOVERFLOW", "ETIMEDOUT"};
    return names;
}

/// The ABI types every POSIX binding names, so a consumer can spell a descriptor or an offset the same way whatever
/// it is compiled for. The widths behind them are each system's own and are not compared.
const std::vector<std::string> &SharedTypes() {
    static const std::vector<std::string> types = {"FileDescriptor", "FileMode", "FileOffset", "GroupId",
                                                   "ProcessId",      "Timespec", "UserId"};
    return types;
}

std::filesystem::path WindowsManifest() {
    return std::filesystem::path(RUX_PACKAGES_DIR) / "Windows" / "Rux.toml";
}

} // namespace

TEST_CASE("Every shared POSIX wrapper is declared by all three platform packages") {
    for (const auto &package : PosixPackages()) {
        const auto &surface = Surface(package);
        REQUIRE_MESSAGE(surface.functions.size() > 20, "no declarations parsed out of Packages/", package, "/Src");

        for (const auto &wrapper : SharedWrappers()) {
            CHECK_MESSAGE(surface.functions.contains(wrapper), "Packages/", package,
                          " does not declare the shared POSIX wrapper '", wrapper,
                          "'. Either bind the call this system has, or remove it from SharedWrappers() and say in "
                          "the package README that this system does not offer it.");
        }
    }
}

TEST_CASE("Equivalent POSIX wrappers agree on parameter names and shapes") {
    const auto &reference = Surface("Linux");

    for (const auto &wrapper : SharedWrappers()) {
        const auto expected = reference.functions.find(wrapper);
        REQUIRE_MESSAGE(expected != reference.functions.end(), "Packages/Linux does not declare '", wrapper, "'");

        for (const auto &package : PosixPackages()) {
            const auto &surface = Surface(package);
            const auto actual = surface.functions.find(wrapper);
            if (actual == surface.functions.end()) {
                continue; // The absence is already reported by the previous case.
            }
            CHECK_MESSAGE(actual->second == expected->second, "Packages/", package, "/Src/",
                          surface.functionFiles.at(wrapper), " declares ", wrapper, actual->second,
                          " where Packages/Linux declares ", wrapper, expected->second,
                          ". Equivalent wrappers take the same parameters, named the same way, so a consumer writes "
                          "one call site rather than a target branch.");
        }
    }
}

TEST_CASE("No POSIX binding takes a kernel record by reference") {
    for (const auto &package : PosixPackages()) {
        const auto &surface = Surface(package);
        for (const auto &site : surface.referenceParameters) {
            FAIL_CHECK("Packages/" << package << "/Src/" << site
                                   << " takes a reference. These packages pass every kernel record by address, "
                                      "which each README documents and which is what lets one call site serve all "
                                      "three systems; a reference parameter forces every caller onto a target "
                                      "branch. Use *T or *var T.");
        }
    }
}

TEST_CASE("Every shared errno name is declared by all three platform packages") {
    for (const auto &package : PosixPackages()) {
        const auto &surface = Surface(package);
        for (const auto &name : SharedErrnoNames()) {
            CHECK_MESSAGE(surface.constants.contains(name), "Packages/", package, " does not declare '", name,
                          "', which the other POSIX packages name. Add it with this kernel's own number.");
        }
    }
}

TEST_CASE("Shared errno numbers are each system's own where the systems differ") {
    const auto &linux = Surface("Linux").constants;
    const auto &darwin = Surface("macOS").constants;
    const auto &freebsd = Surface("FreeBSD").constants;

    // The low numbers descend from the same table and agree on all three. A future package that copied one of these
    // wholesale would pass the name check above and fail here only if it also got a number wrong, which is the
    // point: the names are shared, the values are checked against what each kernel publishes.
    for (const auto &[name, value] : std::map<std::string, long long>{
             {"EPERM", 1},   {"ENOENT", 2},   {"ESRCH", 3},   {"EINTR", 4},   {"EIO", 5},     {"ENXIO", 6},
             {"EBADF", 9},   {"ENOMEM", 12},  {"EACCES", 13}, {"EFAULT", 14}, {"EBUSY", 16},  {"EEXIST", 17},
             {"EXDEV", 18},  {"ENOTDIR", 20}, {"EISDIR", 21}, {"EINVAL", 22}, {"EMFILE", 24}, {"ENOSPC", 28},
             {"ESPIPE", 29}, {"EROFS", 30},   {"EPIPE", 32}}) {
        CHECK_MESSAGE(linux.at(name) == value, "Linux ", name, " is ", linux.at(name), ", expected ", value);
        CHECK_MESSAGE(darwin.at(name) == value, "macOS ", name, " is ", darwin.at(name), ", expected ", value);
        CHECK_MESSAGE(freebsd.at(name) == value, "FreeBSD ", name, " is ", freebsd.at(name), ", expected ", value);
    }

    // Above EPIPE the three part company, and a shared name carrying a copied number would be a real bug in a
    // consumer that compares an errno against a constant. Linux keeps its own numbering; the two BSDs agree.
    CHECK(linux.at("EAGAIN") == 11);
    CHECK(darwin.at("EAGAIN") == 35);
    CHECK(freebsd.at("EAGAIN") == 35);

    CHECK(linux.at("ENAMETOOLONG") == 36);
    CHECK(darwin.at("ENAMETOOLONG") == 63);
    CHECK(freebsd.at("ENAMETOOLONG") == 63);

    CHECK(linux.at("ENOSYS") == 38);
    CHECK(darwin.at("ENOSYS") == 78);
    CHECK(freebsd.at("ENOSYS") == 78);

    CHECK(linux.at("ELOOP") == 40);
    CHECK(darwin.at("ELOOP") == 62);
    CHECK(freebsd.at("ELOOP") == 62);

    CHECK(linux.at("EOVERFLOW") == 75);
    CHECK(darwin.at("EOVERFLOW") == 84);
    CHECK(freebsd.at("EOVERFLOW") == 84);

    CHECK(linux.at("ETIMEDOUT") == 110);
    CHECK(darwin.at("ETIMEDOUT") == 60);
    CHECK(freebsd.at("ETIMEDOUT") == 60);
}

TEST_CASE("All three platform packages reserve the same error-result window") {
    // `IsError` answers "is this negative number an errno rather than an address", and the window it tests is what
    // keeps a legitimate mapping address with its top bit set from reading as a failure. The three packages must
    // agree on it, because `Rux/Memory` reads all three through the same call. The window's behaviour is asserted
    // where it runs, in Tests/Packages/{Linux,macOS,FreeBSD}/Mapping; its bound is asserted here, where it can be
    // checked on any host.
    for (const auto &package : PosixPackages()) {
        const auto &constants = Surface(package).constants;
        REQUIRE_MESSAGE(constants.contains("MaximumErrno"), "Packages/", package, " does not declare MaximumErrno");
        CHECK_MESSAGE(constants.at("MaximumErrno") == 4095, "Packages/", package, " reserves errno numbers up to ",
                      constants.at("MaximumErrno"),
                      ", where the other POSIX packages reserve 4095. A narrower window reports a real error as a "
                      "success; a wider one reports a mapping address as an error.");
    }
}

TEST_CASE("Constants a consumer would carry between platform packages actually differ") {
    // Each of these is a value that compiles when passed to the wrong package and means something else there. The
    // READMEs warn about them in prose; this is the same warning where a change would trip over it.
    CHECK(Surface("Linux").constants.at("AtFdCwd") == -100);
    CHECK(Surface("macOS").constants.at("AtFdCwd") == -2);
    CHECK(Surface("FreeBSD").constants.at("AtFdCwd") == -100);

    CHECK(Surface("Linux").constants.at("ClockMonotonic") == 1);
    CHECK(Surface("macOS").constants.at("ClockMonotonic") == 6);
    CHECK(Surface("FreeBSD").constants.at("ClockMonotonic") == 4);

    CHECK(Surface("Linux").constants.at("RTLD_LOCAL") == 0);
    CHECK(Surface("macOS").constants.at("RTLD_LOCAL") == 4);
    CHECK(Surface("FreeBSD").constants.at("RTLD_LOCAL") == 0);
}

TEST_CASE("Every shared ABI type is declared by all three platform packages") {
    for (const auto &package : PosixPackages()) {
        const auto &surface = Surface(package);
        for (const auto &type : SharedTypes()) {
            CHECK_MESSAGE(surface.types.contains(type), "Packages/", package, " does not declare the shared ABI type '",
                          type, "'. Its width is this system's own; its name is not.");
        }
    }
}

TEST_CASE("Windows keeps its failure sentinels and error codes distinct") {
    // Win32 fails in three shapes and they must not collide: a handle-returning call reports an all-ones handle, a
    // memory-returning call reports null, and an attribute query reports all ones in thirty-two bits. A caller who
    // tests a handle against null, or an allocation against the handle sentinel, sees a failure as a success.
    // Tests/Packages/Windows/Failures asserts the behaviour on a Windows host; the values are asserted here, where
    // any host can check them.
    const auto &constants = Surface("Windows").constants;

    REQUIRE_MESSAGE(constants.contains("InvalidHandleValue"), "Packages/Windows does not declare InvalidHandleValue");
    CHECK_MESSAGE(constants.at("InvalidHandleValue") == -1,
                  "the invalid-handle sentinel is all ones, which is what makes it different from null");

    REQUIRE(constants.contains("INVALID_FILE_ATTRIBUTES"));
    CHECK(static_cast<std::uint32_t>(constants.at("INVALID_FILE_ATTRIBUTES")) == 0xFFFFFFFFU);

    // Both domains spell success as zero, which is why the rest of the values have to stay apart.
    CHECK(constants.at("STATUS_SUCCESS") == 0);
    CHECK(constants.at("ERROR_SUCCESS") == 0);

    // Every Win32 code this package names, at its published value. A typo in one silently merges two distinct
    // failures wherever they are translated.
    for (const auto &[name, value] : std::map<std::string, long long>{{"ERROR_FILE_NOT_FOUND", 2},
                                                                      {"ERROR_PATH_NOT_FOUND", 3},
                                                                      {"ERROR_ACCESS_DENIED", 5},
                                                                      {"ERROR_INVALID_HANDLE", 6},
                                                                      {"ERROR_NOT_ENOUGH_MEMORY", 8},
                                                                      {"ERROR_OUTOFMEMORY", 14},
                                                                      {"ERROR_NO_MORE_FILES", 18},
                                                                      {"ERROR_NOT_SUPPORTED", 50},
                                                                      {"ERROR_FILE_EXISTS", 80},
                                                                      {"ERROR_INVALID_PARAMETER", 87},
                                                                      {"ERROR_DISK_FULL", 112},
                                                                      {"ERROR_ALREADY_EXISTS", 183},
                                                                      {"ERROR_INVALID_ADDRESS", 487},
                                                                      {"ERROR_COMMITMENT_LIMIT", 1455},
                                                                      {"ERROR_PRIVILEGE_NOT_HELD", 1314}}) {
        REQUIRE_MESSAGE(constants.contains(name), "Packages/Windows does not declare ", name);
        CHECK_MESSAGE(constants.at(name) == value, "Windows ", name, " is ", constants.at(name), ", expected ", value);
    }

    // No two of them may share a number.
    std::map<long long, std::string> seen;
    for (const auto &[name, value] : constants) {
        if (!name.starts_with("ERROR_")) {
            continue;
        }
        const auto found = seen.find(value);
        CHECK_MESSAGE(found == seen.end(), "Windows ", name, " and ", found == seen.end() ? "" : found->second,
                      " share the value ", value, ", so a translation cannot tell them apart");
        seen.emplace(value, name);
    }
}

TEST_CASE("Windows binds Win32 alone and takes no dependency on Core") {
    // Docs/Packages.md records this as the one rule the platform manifest audit enforces: a manifest edge exists
    // when the sources import the package, and for no other reason. Windows imports nothing from Core, so adding
    // the edge to level its row in the dependency diagram would be an unused dependency.
    const auto manifest = ReadFileText(WindowsManifest());
    REQUIRE_MESSAGE(!manifest.empty(), "no manifest at ", WindowsManifest().string());
    CHECK_MESSAGE(manifest.find("[Dependencies]") == std::string::npos,
                  "Packages/Windows/Rux.toml declares a dependency section. Windows imports nothing from Core; see "
                  "the platform manifest audit in Docs/Packages.md.");

    for (const auto &path : PackageSources("Windows")) {
        const auto text = ReadFileText(path);
        CHECK_MESSAGE(text.find("import Core") == std::string::npos, "Packages/Windows/Src/", path.filename().string(),
                      " imports Core. Either the manifest gains the edge and the audit row changes, or the import "
                      "goes.");
    }

    // The three POSIX packages are the other half of the same audit: each imports Core and each declares it.
    for (const auto &package : PosixPackages()) {
        const auto text = ReadFileText(std::filesystem::path(RUX_PACKAGES_DIR) / package / "Rux.toml");
        CHECK_MESSAGE(text.find("Core = {") != std::string::npos, "Packages/", package,
                      "/Rux.toml does not declare Core, which its sources import.");
    }
}

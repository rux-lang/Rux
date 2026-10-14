#pragma once

// Reuse of compiled modules between builds of one package.
//
// Every module of a program compiles to an object that depends on nothing but its own source, the declarations of the
// whole program and, in a Release build, the declarations other modules reach in it (`Optimization::ModuleRoots`). A
// build therefore records, per module, the object it compiled together with what that object was compiled from, and
// the next build reuses the object of every module whose source is unchanged as long as the program's declarations are
// too. Such a module is analyzed and lowered for its declarations only, which is all the modules compiled again need of
// it.
//
// The program's declarations are summed up by its interface hash: every module's source with the bodies of its
// non-generic functions left out (`ModuleInterface`). Editing a body therefore recompiles that module alone, while
// editing a signature, a type, a constant or a generic body recompiles them all.
//
// A build keeps one pack per profile and target under `Temp/Cache`, holding the entries of all its modules, written
// whole after a successful build in a single rename.

#include "Object/Rcu/Rcu.h"
#include "Optimization/LirReachability.h"
#include "Syntax/Ast/Ast.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rux::Driver {
/// `source` with the body of every non-generic function and method `module` declares replaced by `{}`: what other
/// modules can depend on. A module whose bodies declare items into the program's indexes is its whole source.
[[nodiscard]] std::string ModuleInterface(const Module &module, std::string_view source);

/// What a build recorded about one module.
struct CachedModule {
    /// Everything the module's object was compiled from but its roots: the compilation, the program's interface and
    /// the module's own source.
    std::string key;
    /// The roots it was pruned from (`Optimization::ModuleRoots`), which its object also depends on; empty when the
    /// build prunes nothing.
    std::vector<Optimization::LirModuleRoot> roots;
    /// The module's references before optimization, which reachability needs of a module that is not lowered again.
    Optimization::LirModuleReferences references;
    /// Whether its code can panic or fail an assertion, which calls for the program's stack-trace printer.
    bool fails = false;
    /// The object, as `SerializeRcuFile` encoded it.
    std::vector<std::uint8_t> object;
    /// The size and modification time of the source the digests below were taken of, so a later build can take them
    /// over for a file it finds unchanged instead of hashing it again.
    std::uintmax_t sourceSize = 0;
    std::int64_t sourceModified = 0;
    std::string sourceDigest;
    std::string interfaceDigest;
};

/// The modules one build recorded, by module name.
class ModulePack {
public:
    /// The pack at `path`, or an empty one when there is none or it cannot be read.
    [[nodiscard]] static ModulePack Read(const std::filesystem::path &path);

    /// The entry of `module` when it was recorded under `key`.
    [[nodiscard]] const CachedModule *Find(const std::string &module, std::string_view key) const;
    /// The entry of `module` whatever it was recorded under.
    [[nodiscard]] const CachedModule *FindAny(const std::string &module) const;

    [[nodiscard]] std::size_t Size() const noexcept {
        return entries.size();
    }

    void Put(const std::string &module, CachedModule entry);
    /// Remove the entry of `module` and hand it over, so a pack written after this one keeps it without a copy.
    /// Pointers to other entries stay valid.
    [[nodiscard]] std::optional<CachedModule> Take(const std::string &module);

    /// Write the pack to `path`, replacing any earlier one in a single rename. Returns false when it cannot be written.
    bool Write(const std::filesystem::path &path) const;

private:
    std::unordered_map<std::string, CachedModule> entries;
};
} // namespace Rux::Driver

#pragma once

// Precompiled dependency packages, kept in a cache every package of the user shares.
//
// A dependency package compiles to the same objects in every program that uses it as long as it is compiled the same
// way: by the same compiler for the same target and profile, from the same sources, under the same compile-time
// context, from the same place on disk, against dependencies compiled the same way. That is the package's key. What a
// module of it compiles to can still depend on the program in two ways: the names the program gives the package's
// declarations, which only a same-named declaration elsewhere changes, and, in Release, which of its declarations the
// rest of the program reaches. A cached package is therefore kept as variants of its key, each recording the names its
// declarations had (`NamesDigest`) and every module's roots. A build reuses the newest variant of each dependency's
// key and checks the names once the program is analyzed; when they differ it takes the newest variant kept under its
// own names instead, and only when there is none compiles the package again. The roots are checked once the program is
// lowered. So a reused object is always the one compiling the package would write.
//
// The cache lives in `<user data>/Cache/Packages/<key>/<variant>.modules`, each variant a module pack written in one
// rename, so builds that write the same variant at once leave one whole copy.

#include "Driver/ModuleCache.h"
#include "Semantic/Model/SemanticModel.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace Rux::Driver {
/// Where precompiled packages are kept: `configured` when given, else `RUX_PACKAGE_CACHE` when set, else the user's
/// data directory's `Cache/Packages`.
[[nodiscard]] std::filesystem::path PackageCacheDirectory(const std::optional<std::filesystem::path> &configured);

/// The newest variant kept under `keyDirectory`, or nothing when there is none that reads back.
[[nodiscard]] std::optional<ModulePack> ReadNewestPackageVariant(const std::filesystem::path &keyDirectory);

/// The newest variant kept under `keyDirectory` that holds every one of `modules` under `names`, or nothing. Programs
/// that name the package's declarations differently each keep a variant of it; this finds the one kept for a program
/// that names them as this one does.
[[nodiscard]] std::optional<ModulePack> ReadPackageVariantNamed(const std::filesystem::path &keyDirectory,
                                                                const std::string &names,
                                                                std::span<const Module *const> modules);

/// Keep `pack` as variant `variant` under `keyDirectory`, unless that variant is already kept.
void WritePackageVariant(const std::filesystem::path &keyDirectory, const std::string &variant, const ModulePack &pack);

/// A digest of the names the program gave the declarations of `modules`: their functions, methods, types and
/// interface tables. Constants and module variables are left out, as their names depend only on their package.
[[nodiscard]] std::string NamesDigest(const SemanticModel &model, std::span<const Module *const> modules);
} // namespace Rux::Driver

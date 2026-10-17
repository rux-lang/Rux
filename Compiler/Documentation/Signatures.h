#pragma once

#include "Syntax/Ast/Ast.h"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace Rux::Documentation {
/// Spell a type the way it was written in source, since documentation should show the reader the syntax they would type
/// rather than an internal normal form. That includes its grouping, which some positions require.
[[nodiscard]] std::string TypeText(const TypeExpr *type);
/// Spell a type as written, without the parentheses the source may have grouped it in.
[[nodiscard]] std::string UngroupedTypeText(const TypeExpr *type);
/// Spell a generic parameter list with its bounds, as in `<T: Display + Debug>`, or nothing when there is none.
[[nodiscard]] std::string TypeParams(const std::vector<TypeParameter> &parameters);
/// Spell a function's declaration line without its body.
[[nodiscard]] std::string FunctionSignature(const FuncDecl &function);
/// Spell an extern function's declaration line.
[[nodiscard]] std::string ExternFunctionSignature(const ExternFuncDecl &function);
/// The name a declaration is documented under, or empty for one that is not an item, such as an import.
[[nodiscard]] std::string DeclName(const Decl &decl);
/// The kind of declaration as the generated page names it.
[[nodiscard]] std::string DeclKind(const Decl &decl);
/// A declaration's header line as source would spell it, without a body.
[[nodiscard]] std::string DeclSignature(const Decl &decl);
/// One enum member or variant case as source would spell it, with its value or payload.
[[nodiscard]] std::string CaseSignature(const EnumDecl &declaration, const EnumDecl::Variant &variant);

/// Whether a declaration belongs in the generated output, which by default is the package's public surface only.
///
/// A public item is externally visible only while every module containing it is public.
[[nodiscard]] bool Visible(const Decl &decl, bool includePrivate, bool containingModulesPublic,
                           const std::unordered_set<std::string> &publicTypes);
/// Record the names of the externally visible types `decl` declares, both bare and qualified by their modules, so
/// `Visible` can tell an extension of a public type from one of a private type.
void CollectPublicTypes(const Decl &decl, bool containingModulesPublic, std::unordered_set<std::string> &publicTypes,
                        const std::string &prefix = {});

/// The source path a page shows for a module. A name that is already relative is shown as written: measuring it against
/// the root would drag the working directory into the result. An absolute path is shown relative to the package root,
/// and one the root does not contain keeps only its file name rather than a chain of parent steps.
[[nodiscard]] std::string SourceDisplayName(const std::filesystem::path &sourcePath,
                                            const std::filesystem::path &packageRoot);
} // namespace Rux::Documentation

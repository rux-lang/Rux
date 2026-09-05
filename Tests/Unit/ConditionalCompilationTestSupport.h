#pragma once

#include "Semantic/Model/CompileTimeContext.h"
#include "Semantic/Model/SemanticModel.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <string>
#include <string_view>

namespace Rux::Testing {
[[nodiscard]] ParseResult ParseConditionalSource(const std::string &source);
[[nodiscard]] DepPackage ConditionalCoreDependency(ParseResult &storage);
[[nodiscard]] SemanticModel AnalyzeConditionalModule(Module &module, const std::string &targetSystem = "Windows");
[[nodiscard]] SemanticModel AnalyzeConditionalModule(Module &module, CompileTimeContext context);
[[nodiscard]] SemanticModel AnalyzeConditionalModuleWithoutDependencies(Module &module, CompileTimeContext context);

[[nodiscard]] const FuncDecl *FindConditionalFunc(const Module &module, std::string_view name);
[[nodiscard]] const ExternBlockDecl *FindConditionalExternBlock(const Module &module);
[[nodiscard]] std::string ConditionalReturnedLiteral(const FuncDecl &func);
} // namespace Rux::Testing

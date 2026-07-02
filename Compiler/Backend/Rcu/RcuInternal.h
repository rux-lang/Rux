#pragma once

// Internal seams between the RCU pipeline pieces: code generation
// (RcuCodeGen.cpp), binary serialization (RcuWriter.cpp), and the
// human-readable dump (RcuDump.cpp). Only Rcu.cpp should include this.

#include "Backend/Rcu/Rcu.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Rux {

// Generate one RCU object file from a LIR module (RcuCodeGen.cpp).
[[nodiscard]] RcuFile GenerateRcuModule(const LirModule &mod, const std::vector<LirStructDecl> &structDecls,
                                        const std::vector<std::string> &interfaceNames, const std::string &packageName);

// Serialize to the binary RCU format (RcuWriter.cpp).
[[nodiscard]] std::vector<std::uint8_t> SerializeRcuFile(const RcuFile &file);

// Render a human-readable dump (RcuDump.cpp).
[[nodiscard]] std::string DumpRcuFileText(const RcuFile &file);

} // namespace Rux

#pragma once

#include "Linker/RcuLinkGraph.h"
#include "Object/Rcu/Rcu.h"

#include <cstdint>
#include <vector>

namespace Rux {
/**
 * @brief Drop every function and every piece of data no root reaches.
 *
 * Every sized symbol of a code, read-only or initialized data section is an atom: the bytes it covers, merged with the
 * symbols that overlap it. The artifact's roots (the entry point, the exports, `DllMain` and the process start frame)
 * are live, and so is every atom a live atom's relocations name, or that a relocation from a zero-fill,
 * thread-local or other section names. Bytes of a data section no sized symbol covers, such as a literal reached
 * through a label of no size, are kept whenever they hold anything. A function table of the stack-trace printer names
 * every function of its module without keeping any alive; the entry of a function that is dropped holds address zero,
 * which no return address falls in.
 *
 * Each section is rebuilt from its live atoms in their original order, each at the same offset modulo its alignment
 * (and at least sixteen) as before, so alignment does not change; `padding` fills the gaps in code and zero those in
 * data. Symbols and relocations are remapped to match, and those of dropped atoms removed.
 *
 * @return whether anything was dropped, in which case the graph built over `objects` is stale
 */
bool CollectUnreachable(std::vector<RcuFile> &objects, const RcuLinkGraph &graph, std::uint8_t padding);
} // namespace Rux

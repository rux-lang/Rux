#pragma once

#include "Linker/RcuLinkGraph.h"
#include "Object/Rcu/Rcu.h"

#include <cstdint>
#include <vector>

namespace Rux {
/**
 * @brief Drop every function no live code or data reaches.
 *
 * Every function is its own atom: the bytes of its text symbol. The artifact's roots (the entry point, the exports and
 * `DllMain`) and every function data refers to are live, and so is every function a live function's relocations name.
 * Data is always kept. A function table of the stack-trace printer names every function of its module without keeping
 * any alive; the entry of a function that is dropped holds address zero, which no return address falls in.
 *
 * The text of each object is rebuilt from its live functions in their original order, each at the same offset modulo
 * sixteen as before so code alignment does not change, with `padding` filling the gaps. Symbols and relocations are
 * remapped to match, and those of dropped functions removed.
 *
 * @return whether anything was dropped, in which case the graph built over `objects` is stale
 */
bool CollectUnreachableFunctions(std::vector<RcuFile> &objects, const RcuLinkGraph &graph, std::uint8_t padding);
} // namespace Rux

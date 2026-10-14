#include "Ir/Lir/LirRegs.h"

#include <doctest.h>
#include <utility>
#include <vector>

using namespace Rux;

TEST_CASE("LIR register lists keep short lists inline and grow to the heap like a vector") {
    LirRegs regs = {1, 2, 3};
    CHECK(regs.size() == 3);
    CHECK(regs == std::vector<LirReg>{1, 2, 3});

    regs.push_back(4);
    regs.push_back(5);
    regs.insert(regs.begin(), 0);
    CHECK(regs == std::vector<LirReg>{0, 1, 2, 3, 4, 5});
    CHECK(regs.front() == 0);
    CHECK(regs.back() == 5);
    CHECK(regs.at(2) == 2);

    regs.erase(regs.begin() + 1, regs.begin() + 3);
    CHECK(regs == std::vector<LirReg>{0, 3, 4, 5});

    const LirRegs copy = regs;
    regs[0] = 9;
    CHECK(copy == std::vector<LirReg>{0, 3, 4, 5});
    CHECK(regs == std::vector<LirReg>{9, 3, 4, 5});

    LirRegs moved = std::move(regs);
    CHECK(moved == std::vector<LirReg>{9, 3, 4, 5});
    CHECK(regs.empty()); // NOLINT(bugprone-use-after-move): a moved-from list is empty and reusable
    regs = {7};
    CHECK(regs == std::vector<LirReg>{7});

    LirRegs inlineList = {1, 2};
    LirRegs heapList = {1, 2, 3, 4, 5, 6};
    std::swap(inlineList, heapList);
    CHECK(inlineList == std::vector<LirReg>{1, 2, 3, 4, 5, 6});
    CHECK(heapList == std::vector<LirReg>{1, 2});

    std::vector<LirReg> arguments = {4, 5, 6, 7, 8};
    heapList = std::move(arguments);
    CHECK(heapList == std::vector<LirReg>{4, 5, 6, 7, 8});
    CHECK(std::vector<LirReg>(heapList.begin() + 1, heapList.end()) == std::vector<LirReg>{5, 6, 7, 8});
    heapList.clear();
    CHECK(heapList.empty());
}

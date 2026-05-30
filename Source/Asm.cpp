/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

#include "Rux/Asm.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux {
    // Type utilities
    namespace {
        int SizeOf(const TypeRef& t) {
            switch (t.kind) {
            case TypeRef::Kind::Bool8: // Bool == Bool8
            case TypeRef::Kind::Char8:
            case TypeRef::Kind::Int8:
            case TypeRef::Kind::UInt8:
                return 1;
            case TypeRef::Kind::Bool16:
            case TypeRef::Kind::Char16:
            case TypeRef::Kind::Int16:
            case TypeRef::Kind::UInt16:
                return 2;
            case TypeRef::Kind::Bool32:
            case TypeRef::Kind::Char32: // Char == Char32
            case TypeRef::Kind::Int32:
            case TypeRef::Kind::UInt32:
            case TypeRef::Kind::Float32:
                return 4;
            case TypeRef::Kind::Opaque:
                return 0;
            case TypeRef::Kind::Tuple: {
                int total = 0;
                for (const auto& elem : t.inner)
                    total += SizeOf(elem);
                return total;
            }
            case TypeRef::Kind::Named:
                if (!t.inner.empty()) return SizeOf(t.inner[0]);
                return 8;
            default:
                return 8; // int, uint, int64, uint64, float64, pointer, str, named, …
            }
        }

        bool IsFloat(const TypeRef& t) {
            return t.kind == TypeRef::Kind::Float32 || t.kind == TypeRef::Kind::Float64;
        }

        int AlignUp(int v, int a) {
            return (v + a - 1) & ~(a - 1);
        }

        // ── Physical register pool ────────────────────────────────────────────────
        // The allocator uses two tiers of registers:
        //
        //   Callee-saved (rbx, r12–r15):  survive function calls; need
        //   save/restore in the prologue/epilogue.  Suitable for ALL intervals.
        //
        //   Caller-saved (rsi, rdi, r8, r9):  clobbered by calls; NO
        //   save/restore needed.  Only suitable for intervals that do NOT span
        //   any Call / CallIndirect instruction.
        //
        // Excluded from allocation:
        //   rax — accumulator (LoadA/StoreA, call return value)
        //   r10 — scratch B (LoadB, codegen scratch)
        //   r11 — scratch C (various codegen)
        //   rcx — implicit shift count (cl) for SHL/SHR/SAR
        //   rdx — implicit dividend high / remainder for DIV/IDIV
        //   rsp, rbp — frame management
        //
        // This gives 9 allocatable registers (5 callee-saved + 4 caller-saved),
        // nearly doubling the previous 5-register pool.
        enum class PhysReg : uint8_t {
            // Callee-saved (indices 0–4) — need prologue/epilogue save
            Rbx = 0,
            R12,
            R13,
            R14,
            R15,
            // Caller-saved (indices 5–8) — no save needed
            Rsi,
            Rdi,
            R8,
            R9,
            Count, // sentinel — must be last
        };
        static constexpr int kPhysRegCount    = static_cast<int>(PhysReg::Count); // 9
        static constexpr int kCalleeSavedCount = 5; // first 5 are callee-saved

        static constexpr std::string_view kPhysReg64[kPhysRegCount] = {
            "rbx", "r12", "r13", "r14", "r15", "rsi", "rdi", "r8", "r9"};
        static constexpr std::string_view kPhysReg32[kPhysRegCount] = {
            "ebx", "r12d", "r13d", "r14d", "r15d", "esi", "edi", "r8d", "r9d"};
        static constexpr std::string_view kPhysReg16[kPhysRegCount] = {
            "bx", "r12w", "r13w", "r14w", "r15w", "si", "di", "r8w", "r9w"};
        static constexpr std::string_view kPhysReg8[kPhysRegCount] = {
            "bl", "r12b", "r13b", "r14b", "r15b", "sil", "dil", "r8b", "r9b"};

        static bool IsCalleeSaved(PhysReg r) {
            return static_cast<int>(r) < kCalleeSavedCount;
        }

        static std::string_view PhysRegName(PhysReg r, int bytes) {
            const int i = static_cast<int>(r);
            switch (bytes) {
            case 1:  return kPhysReg8[i];
            case 2:  return kPhysReg16[i];
            case 4:  return kPhysReg32[i];
            default: return kPhysReg64[i];
            }
        }

        // ── Linear Scan Register Allocator ────────────────────────────────────────
        //
        // Poletto & Sarkar 1999 algorithm with backward-dataflow liveness for
        // correctness across loops and back-edges.
        //
        // Allocates 9 integer registers (5 callee-saved + 4 caller-saved) to
        // virtual registers.  Intervals that span a Call/CallIndirect instruction
        // are restricted to callee-saved registers only (because caller-saved
        // registers are clobbered by the call).  Float vregs and vregs that
        // exceed register pressure fall back to the existing stack-slot scheme.
        struct RegAssignResult {
            std::unordered_map<LirReg, PhysReg> inReg; // vreg → physical reg
            bool used[kPhysRegCount] = {};              // which regs need save/restore
        };

        class LinearScanAllocator {
        public:
            RegAssignResult Run(const LirFunc& func) {
                if (func.blocks.empty()) return {};
                const uint32_t n = static_cast<uint32_t>(func.blocks.size());

                // 1. Build successors / predecessors from block terminators
                std::vector<std::vector<uint32_t>> succs(n), preds(n);
                for (uint32_t b = 0; b < n; b++) {
                    if (!func.blocks[b].term) continue;
                    const auto& t = *func.blocks[b].term;
                    auto addEdge = [&](uint32_t to) {
                        if (to < n) {
                            succs[b].push_back(to);
                            preds[to].push_back(b);
                        }
                    };
                    switch (t.kind) {
                    case LirTermKind::Jump:   addEdge(t.trueTarget); break;
                    case LirTermKind::Branch: addEdge(t.trueTarget); addEdge(t.falseTarget); break;
                    case LirTermKind::Switch:
                        addEdge(t.defaultTarget);
                        for (const auto& c : t.cases) addEdge(c.target);
                        break;
                    case LirTermKind::Return: break;
                    }
                }

                // 2. Instruction numbering: block b occupies [blockStart[b], blockStart[b]+|instrs|+1)
                std::vector<int> blockStart(n);
                {
                    int pos = 0;
                    for (uint32_t b = 0; b < n; b++) {
                        blockStart[b] = pos;
                        pos += static_cast<int>(func.blocks[b].instrs.size()) + 1; // +1 for terminator
                    }
                }

                // 3. Per-block use / def sets for backward dataflow
                std::vector<std::unordered_set<LirReg>> use(n), def(n);
                // Phi dsts defined at block entry (mark first so intra-block uses don't bubble up)
                for (uint32_t b = 0; b < n; b++)
                    for (const auto& instr : func.blocks[b].instrs)
                        if (instr.op == LirOpcode::Phi && instr.dst != LirNoReg)
                            def[b].insert(instr.dst);
                // Regular instructions
                for (uint32_t b = 0; b < n; b++) {
                    for (const auto& instr : func.blocks[b].instrs) {
                        if (instr.op == LirOpcode::Phi) continue; // phi srcs handled below
                        for (LirReg s : instr.srcs)
                            if (s != LirNoReg && !def[b].count(s)) use[b].insert(s);
                        if (instr.dst != LirNoReg) def[b].insert(instr.dst);
                    }
                    if (func.blocks[b].term) {
                        const auto& t = *func.blocks[b].term;
                        if (t.cond != LirNoReg && !def[b].count(t.cond))
                            use[b].insert(t.cond);
                        if (t.retVal && *t.retVal != LirNoReg && !def[b].count(*t.retVal))
                            use[b].insert(*t.retVal);
                    }
                }
                // Phi src operands are used in their predecessor blocks
                for (uint32_t b = 0; b < n; b++)
                    for (const auto& instr : func.blocks[b].instrs)
                        if (instr.op == LirOpcode::Phi)
                            for (const auto& [src, pred] : instr.phiPreds)
                                if (src != LirNoReg && pred < n && !def[pred].count(src))
                                    use[pred].insert(src);

                // 4. Backward dataflow → live_in / live_out
                std::vector<std::unordered_set<LirReg>> liveIn(n), liveOut(n);
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (uint32_t b = n; b-- > 0;) {
                        std::unordered_set<LirReg> newOut;
                        for (uint32_t s : succs[b])
                            newOut.insert(liveIn[s].begin(), liveIn[s].end());
                        std::unordered_set<LirReg> newIn = use[b];
                        for (LirReg r : newOut)
                            if (!def[b].count(r)) newIn.insert(r);
                        if (newOut != liveOut[b] || newIn != liveIn[b]) {
                            liveOut[b] = std::move(newOut);
                            liveIn[b]  = std::move(newIn);
                            changed    = true;
                        }
                    }
                }

                // 5. Build live intervals [from, to) per vreg
                std::unordered_map<LirReg, IState> imap;
                imap.reserve(256);

                auto extendTo   = [&](LirReg r, int to) {
                    if (r == LirNoReg) return;
                    imap[r].to = std::max(imap[r].to, to);
                };
                auto extendFrom = [&](LirReg r, int from) {
                    if (r == LirNoReg) return;
                    auto& iv = imap[r];
                    iv.from = std::min(iv.from, from);
                    if (iv.to == 0) iv.to = from + 1;
                };
                auto setDef     = [&](LirReg r, int pos) {
                    if (r == LirNoReg) return;
                    auto& iv = imap[r];
                    iv.from = std::min(iv.from, pos);
                    iv.to   = std::max(iv.to, pos + 1);
                };

                for (uint32_t b = 0; b < n; b++) {
                    const int bStart = blockStart[b];
                    const int bTerm  = bStart + static_cast<int>(func.blocks[b].instrs.size());
                    for (LirReg r : liveOut[b]) { extendFrom(r, bStart); extendTo(r, bTerm + 1); }
                    for (LirReg r : liveIn[b])    extendFrom(r, bStart);
                    for (const auto& instr : func.blocks[b].instrs)
                        if (instr.op == LirOpcode::Phi && instr.dst != LirNoReg)
                            setDef(instr.dst, bStart);
                    for (int i = 0; i < static_cast<int>(func.blocks[b].instrs.size()); i++) {
                        const auto& instr = func.blocks[b].instrs[i];
                        if (instr.op == LirOpcode::Phi) continue;
                        const int pos = bStart + i;
                        for (LirReg s : instr.srcs) if (s != LirNoReg) extendTo(s, pos + 1);
                        if (instr.dst != LirNoReg) setDef(instr.dst, pos);
                    }
                    if (func.blocks[b].term) {
                        const auto& t = *func.blocks[b].term;
                        if (t.cond != LirNoReg) extendTo(t.cond, bTerm + 1);
                        if (t.retVal && *t.retVal != LirNoReg) extendTo(*t.retVal, bTerm + 1);
                    }
                }
                // Phi srcs live at end of their predecessor
                for (uint32_t b = 0; b < n; b++)
                    for (const auto& instr : func.blocks[b].instrs)
                        if (instr.op == LirOpcode::Phi)
                            for (const auto& [src, pred] : instr.phiPreds)
                                if (src != LirNoReg && pred < n) {
                                    int predTerm = blockStart[pred] +
                                        static_cast<int>(func.blocks[pred].instrs.size());
                                    extendTo(src, predTerm + 1);
                                }

                // 6. Collect call instruction positions (for caller-saved constraint)
                std::vector<int> callPositions;
                for (uint32_t b = 0; b < n; b++) {
                    const int bStart = blockStart[b];
                    for (int i = 0; i < static_cast<int>(func.blocks[b].instrs.size()); i++) {
                        const auto& instr = func.blocks[b].instrs[i];
                        if (instr.op == LirOpcode::Call || instr.op == LirOpcode::CallIndirect)
                            callPositions.push_back(bStart + i);
                    }
                }

                // 7. Run linear scan on the collected intervals
                return RunLinearScan(imap, callPositions);
            }

        private:
            struct LiveInterval {
                LirReg vreg    = LirNoReg;
                int from       = 0;
                int to         = 0;
                bool spansCall = false; // true → must use callee-saved register
            };
            struct IState { int from = std::numeric_limits<int>::max(); int to = 0; };

            // Returns true if interval [from, to) overlaps any call position.
            static bool SpansAnyCall(int from, int to, const std::vector<int>& calls) {
                // Binary search for first call >= from
                auto it = std::lower_bound(calls.begin(), calls.end(), from);
                return it != calls.end() && *it < to;
            }

            RegAssignResult RunLinearScan(const std::unordered_map<LirReg, IState>& imap,
                                          const std::vector<int>& callPositions) {
                // Collect integer vregs with valid intervals (skip trivially dead ones)
                std::vector<LiveInterval> ivs;
                ivs.reserve(imap.size());
                for (const auto& [r, s] : imap)
                    if (s.from < s.to) {
                        LiveInterval li{r, s.from, s.to, false};
                        li.spansCall = SpansAnyCall(s.from, s.to, callPositions);
                        ivs.push_back(li);
                    }
                // Sort by start, tie-break by end
                std::sort(ivs.begin(), ivs.end(), [](const LiveInterval& a, const LiveInterval& b) {
                    return a.from < b.from || (a.from == b.from && a.to < b.to);
                });

                // Free register pools (back = allocated first):
                //   calleeFree — rbx, r12–r15 (usable by ALL intervals)
                //   callerFree — rsi, rdi, r8, r9 (only for non-call-spanning)
                std::vector<PhysReg> calleeFree = {
                    PhysReg::R15, PhysReg::R14, PhysReg::R13, PhysReg::R12, PhysReg::Rbx};
                std::vector<PhysReg> callerFree = {
                    PhysReg::R9, PhysReg::R8, PhysReg::Rdi, PhysReg::Rsi};
                // Active intervals sorted by end point (ascending)
                std::vector<const LiveInterval*> active;

                RegAssignResult result;
                for (const LiveInterval& iv : ivs) {
                    // Expire intervals that ended before iv starts
                    {
                        std::vector<const LiveInterval*> still;
                        for (const LiveInterval* act : active) {
                            if (act->to <= iv.from) {
                                PhysReg freed = result.inReg.at(act->vreg);
                                if (IsCalleeSaved(freed))
                                    calleeFree.push_back(freed);
                                else
                                    callerFree.push_back(freed);
                            } else {
                                still.push_back(act);
                            }
                        }
                        active = std::move(still);
                    }

                    // Choose the appropriate pool based on whether this interval
                    // spans a call instruction.
                    PhysReg chosen = PhysReg::Count; // sentinel = not assigned
                    if (iv.spansCall) {
                        // Must use callee-saved
                        if (!calleeFree.empty()) {
                            chosen = calleeFree.back();
                            calleeFree.pop_back();
                        }
                    } else {
                        // Prefer caller-saved (to save callee-saved for others)
                        if (!callerFree.empty()) {
                            chosen = callerFree.back();
                            callerFree.pop_back();
                        } else if (!calleeFree.empty()) {
                            chosen = calleeFree.back();
                            calleeFree.pop_back();
                        }
                    }

                    if (chosen != PhysReg::Count) {
                        result.inReg[iv.vreg] = chosen;
                        result.used[static_cast<int>(chosen)] = true;
                        InsertActive(active, &iv);
                    } else {
                        // All appropriate regs are occupied — try to evict
                        // Find the active interval with the furthest end that
                        // is in an appropriate pool for this interval.
                        const LiveInterval* worst = nullptr;
                        decltype(active.begin()) worstIt;
                        for (auto it = active.begin(); it != active.end(); ++it) {
                            PhysReg r = result.inReg.at((*it)->vreg);
                            // A call-spanning iv can only evict from callee-saved
                            if (iv.spansCall && !IsCalleeSaved(r)) continue;
                            if ((*it)->to > iv.to) {
                                if (!worst || (*it)->to > worst->to) {
                                    worst = *it;
                                    worstIt = it;
                                }
                            }
                        }
                        if (worst) {
                            PhysReg reg = result.inReg.at(worst->vreg);
                            result.inReg.erase(worst->vreg);
                            result.inReg[iv.vreg] = reg;
                            active.erase(worstIt);
                            InsertActive(active, &iv);
                        }
                        // else: iv is spilled (not in result.inReg)
                    }
                }
                return result;
            }

            static void InsertActive(std::vector<const LiveInterval*>& active,
                                     const LiveInterval* iv) {
                auto pos = std::lower_bound(
                    active.begin(), active.end(), iv,
                    [](const LiveInterval* a, const LiveInterval* b) { return a->to < b->to; });
                active.insert(pos, iv);
            }
        };

        // x86-64 register names sized for the rax family
        std::string_view GprA(int bytes) {
            switch (bytes) {
            case 1:
                return "al";
            case 2:
                return "ax";
            case 4:
                return "eax";
            default:
                return "rax";
            }
        }

        // r10 family (caller-saved scratch — primary operand)
        [[maybe_unused]] std::string_view GprB(const int bytes) {
            switch (bytes) {
            case 1:
                return "r10b";
            case 2:
                return "r10w";
            case 4:
                return "r10d";
            default:
                return "r10";
            }
        }

        // r11 family (caller-saved scratch — secondary operand)
        [[maybe_unused]] std::string_view GprC(const int bytes) {
            switch (bytes) {
            case 1:
                return "r11b";
            case 2:
                return "r11w";
            case 4:
                return "r11d";
            default:
                return "r11";
            }
        }

        std::string_view PtrSize(const int bytes) {
            switch (bytes) {
            case 1:
                return "byte";
            case 2:
                return "word";
            case 4:
                return "dword";
            default:
                return "qword";
            }
        }

        // System V AMD64 integer argument registers (in order)
        constexpr std::string_view kIntArgRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
        // Microsoft x64 integer argument registers (in order)
        constexpr std::string_view kWin64IntArgRegs[] = {"rcx", "rdx", "r8", "r9"};
        // System V AMD64 float argument registers (in order)
        constexpr std::string_view kFltArgRegs[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};

        // Struct field layout
        struct FieldLayout {
            std::string name;
            int offset = 0;
            int size = 0;
        };

        struct StructLayout {
            std::vector<FieldLayout> fields;
            int totalSize = 0;
            int alignment = 1;
        };

        using LayoutMap = std::unordered_map<std::string, StructLayout>;

        std::string BaseTypeName(const std::string& name) {
            const std::size_t pos = name.find('<');
            return pos == std::string::npos ? name : name.substr(0, pos);
        }

        StructLayout ComputeLayout(const LirStructDecl& s, const LayoutMap& known) {
            StructLayout result;
            int offset = 0;
            int maxAlign = 1;
            for (const auto& f : s.fields) {
                int sz = SizeOf(f.type);
                int al = sz > 0 ? std::min(sz, 8) : 1;
                if (f.type.kind == TypeRef::Kind::Named) {
                    if (auto it = known.find(BaseTypeName(f.type.name)); it != known.end()) {
                        sz = it->second.totalSize;
                        al = it->second.alignment;
                    }
                }
                if (al > 1) offset = AlignUp(offset, al);
                result.fields.push_back({f.name, offset, sz});
                offset += sz;
                maxAlign = std::max(maxAlign, al);
            }
            result.totalSize = AlignUp(offset, maxAlign);
            result.alignment = maxAlign;
            return result;
        }

        // Code generator
        class AsmGen {
        public:
            explicit AsmGen(const LirPackage& pkg)
                : pkg(pkg) {
            }

            std::string Generate();

        private:
            const LirPackage& pkg;

            // Separate output streams assembled at the end
            std::ostringstream text; // .text section
            std::ostringstream data; // .data section (writable globals)
            std::ostringstream rodata; // .rodata section (string/fp constants)
            std::ostringstream externs; // extern declarations
            std::ostringstream globals; // global declarations

            // Interned read-only constants
            std::unordered_map<std::string, std::string> strLabels;
            std::unordered_map<std::string, std::string> f32Labels;
            std::unordered_map<std::string, std::string> f64Labels;
            int constIdx = 0;

            // Struct field layouts (built once)
            LayoutMap layouts;
            std::unordered_set<std::string> interfaceNames;

            // Per-function state
            struct PhiMove {
                LirReg dst;
                LirReg src;
                TypeRef type;
            };

            std::string curFunc;

            // ── Register allocation ───────────────────────────────────────────────
            std::unordered_map<LirReg, PhysReg> regAssign; // vreg → callee-saved phys reg
            bool    csaveUsed[kPhysRegCount]  = {};        // which callee-saved regs are used
            int32_t csaveSlots[kPhysRegCount] = {};        // frame slot offset for each save

            // ── Stack frame (spilled vregs + alloca data regions) ─────────────────
            std::unordered_map<LirReg, int32_t> slotMap;    // spilled vreg → rbp offset
            std::unordered_map<LirReg, int32_t> allocaData; // alloca vreg → data-region rbp offset
            std::unordered_map<LirReg, TypeRef> regTypes;   // vreg → value type
            int32_t nextOff   = 0;
            int32_t frameSize = 0;

            // phiMoves_[fromBlock][toBlock] = list of (dst, src, type)
            std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::vector<PhiMove>>> phiMoves;

            std::unordered_set<std::string> declaredExterns;

            // Set when the unit uses the integer ** operator; emits __rux_ipow.
            bool usesIpow = false;

            // Low-level emit helpers
            void T(const std::string_view s) {
                text << s << '\n';
            }

            void TI(const std::string_view s) {
                text << "    " << s << '\n';
            }

            void TL(const std::string_view label) {
                text << label << ":\n";
            }

            void TC(const std::string_view comment) {
                text << "    ; " << comment << '\n';
            }

            void TB() {
                text << '\n';
            }

            // Constant interning
            std::string InternStr(const std::string& val) {
                if (const auto it = strLabels.find(val); it != strLabels.end()) return it->second;
                std::string lbl = std::format("__str{}", constIdx++);
                strLabels[val] = lbl;
                // Emit as NUL-terminated bytes in .rodata
                rodata << lbl << ":\n    db    ";
                for (const unsigned char c : val)
                    rodata << static_cast<int>(c) << ", ";
                rodata << "0\n";
                return lbl;
            }

            std::string InternF32(const std::string& val) {
                auto it = f32Labels.find(val);
                if (it != f32Labels.end()) return it->second;
                std::string lbl = std::format("__f32_{}", constIdx++);
                f32Labels[val] = lbl;
                std::uint32_t bits;
                if (val.starts_with("0x")) {
                    bits = static_cast<std::uint32_t>(std::stoull(val, nullptr, 16));
                }
                else {
                    float fv = std::stof(val);
                    std::memcpy(&bits, &fv, sizeof(bits));
                }
                rodata << lbl << ":\n    dd    0x" << std::hex << bits << std::dec << "\n";
                return lbl;
            }

            std::string InternF64(const std::string& val) {
                auto it = f64Labels.find(val);
                if (it != f64Labels.end()) return it->second;
                std::string lbl = std::format("__f64_{}", constIdx++);
                f64Labels[val] = lbl;
                std::uint64_t bits;
                if (val.starts_with("0x")) {
                    bits = std::stoull(val, nullptr, 16);
                }
                else {
                    double dv = std::stod(val);
                    std::memcpy(&bits, &dv, sizeof(bits));
                }
                rodata << lbl << ":\n    dq    0x" << std::hex << bits << std::dec << "\n";
                return lbl;
            }

            void NeedExtern(const std::string& name) {
                if (declaredExterns.insert(name).second) externs << "extern " << name << "\n";
            }

            // Integer exponentiation helper: rax = rdi ** rsi (signed exponent).
            // Emitted once when the unit uses the integer ** operator so the
            // output is self-contained (no libm/CRT dependency). Mirrors the
            // machine-code helper synthesized by the RCU backend.
            void EmitIntPowHelper() {
                TB();
                TL("__rux_ipow");
                TI("test    rsi, rsi"); // exponent
                TI("js      .negative"); // negative exponent yields 0
                TI("mov     eax, 1"); // result = 1
                TL(".loop");
                TI("test    rsi, rsi");
                TI("jz      .done"); // exponent == 0
                TI("test    rsi, 1");
                TI("jz      .square");
                TI("imul    rax, rdi"); // result *= base
                TL(".square");
                TI("imul    rdi, rdi"); // base *= base
                TI("sar     rsi, 1"); // exponent >>= 1
                TI("jmp     .loop");
                TL(".negative");
                TI("xor     eax, eax");
                TL(".done");
                TI("ret");
            }

            static std::string BaseTypeName(const std::string& name) {
                const std::size_t pos = name.find('<');
                return pos == std::string::npos ? name : name.substr(0, pos);
            }

            [[nodiscard]] int SizeOfRuntime(const TypeRef& t) const {
                if (t.kind == TypeRef::Kind::Named) {
                    const std::string base = BaseTypeName(t.name);
                    if (interfaceNames.contains(base)) return 16; // {data: *opaque, vtable: *opaque}
                    if (base == "Slice") return 16;
                    if (const auto it = layouts.find(base); it != layouts.end()) return it->second.totalSize;
                }
                return SizeOf(t);
            }

            // ── Stack slot helpers ────────────────────────────────────────────────
            int32_t AllocSlot(LirReg reg, int bytes) {
                if (auto it = slotMap.find(reg); it != slotMap.end()) return it->second;
                int al = (bytes > 0) ? std::min(bytes, 8) : 1;
                nextOff = AlignUp(nextOff, al);
                nextOff += (bytes > 0 ? bytes : 8);
                slotMap[reg] = nextOff;
                return nextOff;
            }

            int32_t AllocRegion(int bytes) {
                int al = (bytes > 0) ? std::min(bytes, 8) : 1;
                nextOff = AlignUp(nextOff, al);
                nextOff += (bytes > 0 ? bytes : 8);
                return nextOff;
            }

            // ── Load / store helpers ──────────────────────────────────────────────
            // All four helpers check regAssign first so register-assigned vregs never
            // touch the stack. Spilled vregs and float vregs fall through to slotMap.

            // Load vreg → rax (integer) or xmm0 (float)
            void LoadA(LirReg reg, const TypeRef& t) {
                const int sz = SizeOf(t);
                const int runtimeSz = SizeOfRuntime(t);
                if (auto it = regAssign.find(reg); it != regAssign.end() && !IsFloat(t) && runtimeSz != 16) {
                    const std::string_view phys = kPhysReg64[static_cast<int>(it->second)];
                    if (sz == 8 || sz == 0)
                        TI(std::format("{:<8}rax, {}", "mov", phys));
                    else if (t.IsSigned())
                        TI(std::format("{:<8}rax, {}", sz == 4 ? "movsxd" : "movsx",
                                       PhysRegName(it->second, sz)));
                    else if (sz == 4)
                        TI(std::format("{:<8}eax, {}", "mov", PhysRegName(it->second, 4)));
                    else
                        TI(std::format("{:<8}rax, {}", "movzx", PhysRegName(it->second, sz)));
                    return;
                }
                const int off = slotMap.at(reg);
                if (runtimeSz == 16) {
                    TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", off));
                    TI(std::format("{:<8}rdx, qword [rbp - {}]", "mov", off - 8));
                } else if (IsFloat(t)) {
                    TI(std::format("{:<8}xmm0, {} [rbp - {}]",
                                   sz == 4 ? "movss" : "movsd", PtrSize(sz), off));
                } else if (sz == 8 || sz == 0) {
                    TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", off));
                } else if (t.IsSigned()) {
                    TI(std::format("{:<8}rax, {} [rbp - {}]",
                                   sz == 4 ? "movsxd" : "movsx", PtrSize(sz), off));
                } else {
                    if (sz == 4)
                        TI(std::format("{:<8}eax, dword [rbp - {}]", "mov", off));
                    else
                        TI(std::format("{:<8}rax, {} [rbp - {}]", "movzx", PtrSize(sz), off));
                }
            }

            // Load vreg → r10 (integer) or xmm1 (float)
            void LoadB(LirReg reg, const TypeRef& t) {
                const int sz = SizeOf(t);
                if (auto it = regAssign.find(reg); it != regAssign.end() && !IsFloat(t)) {
                    if (sz == 8 || sz == 0)
                        TI(std::format("{:<8}r10, {}", "mov", kPhysReg64[static_cast<int>(it->second)]));
                    else if (t.IsSigned())
                        TI(std::format("{:<8}r10, {}", sz == 4 ? "movsxd" : "movsx",
                                       PhysRegName(it->second, sz)));
                    else if (sz == 4)
                        TI(std::format("{:<8}r10d, {}", "mov", PhysRegName(it->second, 4)));
                    else
                        TI(std::format("{:<8}r10, {}", "movzx", PhysRegName(it->second, sz)));
                    return;
                }
                const int off = slotMap.at(reg);
                if (IsFloat(t)) {
                    TI(std::format("{:<8}xmm1, {} [rbp - {}]",
                                   sz == 4 ? "movss" : "movsd", PtrSize(sz), off));
                } else if (sz == 8 || sz == 0) {
                    TI(std::format("{:<8}r10, qword [rbp - {}]", "mov", off));
                } else if (t.IsSigned()) {
                    TI(std::format("{:<8}r10, {} [rbp - {}]",
                                   sz == 4 ? "movsxd" : "movsx", PtrSize(sz), off));
                } else {
                    if (sz == 4)
                        TI(std::format("{:<8}r10d, dword [rbp - {}]", "mov", off));
                    else
                        TI(std::format("{:<8}r10, {} [rbp - {}]", "movzx", PtrSize(sz), off));
                }
            }

            // Store rax (integer) or xmm0 (float) → vreg's location
            void StoreA(LirReg dst, const TypeRef& t) {
                const int sz = SizeOf(t);
                const int runtimeSz = SizeOfRuntime(t);
                if (auto it = regAssign.find(dst); it != regAssign.end() && !IsFloat(t) && runtimeSz != 16) {
                    if (sz == 8 || sz == 0)
                        TI(std::format("{:<8}{}, rax", "mov", kPhysReg64[static_cast<int>(it->second)]));
                    else if (sz == 4)
                        TI(std::format("{:<8}{}, eax", "mov", PhysRegName(it->second, 4)));
                    else
                        TI(std::format("{:<8}{}, {}", "mov", PhysRegName(it->second, sz), GprA(sz)));
                    return;
                }
                const int off = slotMap.at(dst);
                if (runtimeSz == 16) {
                    TI(std::format("{:<8}qword [rbp - {}], rax", "mov", off));
                    TI(std::format("{:<8}qword [rbp - {}], rdx", "mov", off - 8));
                } else if (IsFloat(t)) {
                    TI(std::format("{:<8}{} [rbp - {}], xmm0",
                                   sz == 4 ? "movss" : "movsd", PtrSize(sz), off));
                } else {
                    const int store_sz = (sz > 0) ? sz : 8;
                    TI(std::format("{:<8}{} [rbp - {}], {}", "mov",
                                   PtrSize(store_sz), off, GprA(store_sz)));
                }
            }

            // Load vreg's 64-bit integer value into any named destination GPR.
            void LoadGpr64(LirReg reg, std::string_view dest) {
                if (auto it = regAssign.find(reg); it != regAssign.end()) {
                    const std::string_view src = kPhysReg64[static_cast<int>(it->second)];
                    if (src != dest) TI(std::format("{:<8}{}, {}", "mov", dest, src));
                } else {
                    TI(std::format("{:<8}{}, qword [rbp - {}]", "mov", dest, slotMap.at(reg)));
                }
            }

            // Store src GPR's 64-bit value into vreg's location.
            void StoreGpr64(LirReg dst, std::string_view src) {
                if (auto it = regAssign.find(dst); it != regAssign.end()) {
                    const std::string_view d = kPhysReg64[static_cast<int>(it->second)];
                    if (d != src) TI(std::format("{:<8}{}, {}", "mov", d, src));
                } else {
                    TI(std::format("{:<8}qword [rbp - {}], {}", "mov", slotMap.at(dst), src));
                }
            }

            // Block labels
            [[nodiscard]] std::string BlockLabel(uint32_t idx, const std::string& label) const {
                if (idx == 0) return curFunc;
                return "." + curFunc + "_" + label;
            }

            // Phi move emission: src may be in register or on stack
            void EmitPhiMoves(uint32_t fromBlock, uint32_t toBlock) {
                auto it1 = phiMoves.find(fromBlock);
                if (it1 == phiMoves.end()) return;
                auto it2 = it1->second.find(toBlock);
                if (it2 == it1->second.end()) return;
                for (const auto& m : it2->second) {
                    if (!regAssign.count(m.src) && !slotMap.count(m.src)) continue;
                    if (!regAssign.count(m.dst) && !slotMap.count(m.dst)) continue;
                    // Optimized: both in registers → direct mov (skip if same reg)
                    auto srcIt = regAssign.find(m.src);
                    auto dstIt = regAssign.find(m.dst);
                    if (srcIt != regAssign.end() && dstIt != regAssign.end() && !IsFloat(m.type)) {
                        if (srcIt->second != dstIt->second) {
                            TI(std::format("{:<8}{}, {}", "mov",
                                           kPhysReg64[static_cast<int>(dstIt->second)],
                                           kPhysReg64[static_cast<int>(srcIt->second)]));
                        }
                        continue;
                    }
                    LoadA(m.src, m.type);
                    StoreA(m.dst, m.type);
                }
            }

            // Pre-pass: run linear scan allocator, then assign stack slots for spills
            void PrepassFunc(const LirFunc& func) {
                nextOff   = 0;
                frameSize = 0;
                slotMap.clear();
                allocaData.clear();
                regTypes.clear();
                phiMoves.clear();
                regAssign.clear();
                std::fill(std::begin(csaveUsed),  std::end(csaveUsed),  false);
                std::fill(std::begin(csaveSlots), std::end(csaveSlots), 0);

                // Run linear scan to get register assignments
                LinearScanAllocator lsa;
                auto alloc = lsa.Run(func);
                regAssign = std::move(alloc.inReg);
                for (int i = 0; i < kPhysRegCount; i++) csaveUsed[i] = alloc.used[i];

                // Parameters: allocate stack slots only for spilled params
                for (const auto& p : func.params) {
                    if (!regAssign.count(p.reg)) AllocSlot(p.reg, 8);
                    regTypes[p.reg] = p.type;
                }

                // Instructions
                for (uint32_t bi = 0; bi < func.blocks.size(); bi++) {
                    const auto& block = func.blocks[bi];
                    for (const auto& instr : block.instrs) {
                        if (instr.op == LirOpcode::Phi)
                            for (const auto& [src, pred] : instr.phiPreds)
                                phiMoves[pred][bi].push_back({instr.dst, src, instr.type});
                        if (instr.dst == LirNoReg) continue;

                        if (instr.op == LirOpcode::Alloca) {
                            if (!regAssign.count(instr.dst)) AllocSlot(instr.dst, 8);
                            int dataSz;
                            if (!instr.strArg.empty()) {
                                int count = std::stoi(instr.strArg);
                                const TypeRef& et =
                                    instr.type.inner.empty() ? instr.type : instr.type.inner[0];
                                int elemSz = SizeOfRuntime(et);
                                dataSz = count * (elemSz > 0 ? elemSz : 8);
                            } else {
                                dataSz = SizeOfRuntime(instr.type);
                            }
                            allocaData[instr.dst] = AllocRegion(dataSz > 0 ? dataSz : 8);
                            regTypes[instr.dst]   = TypeRef::MakePointer(instr.type);
                        } else {
                            int sz = SizeOfRuntime(instr.type);
                            if (!regAssign.count(instr.dst))
                                AllocSlot(instr.dst, sz > 0 ? sz : 8);
                            regTypes[instr.dst] = instr.type;
                        }
                    }
                }

                // Reserve frame slots for callee-saved register saves ONLY.
                // Caller-saved regs (rsi, rdi, r8, r9) do not need saving because
                // they're only assigned to intervals that don't span calls.
                for (int i = 0; i < kCalleeSavedCount; i++) {
                    if (csaveUsed[i]) {
                        nextOff = AlignUp(nextOff, 8) + 8;
                        csaveSlots[i] = nextOff;
                    }
                }

                frameSize = AlignUp(nextOff, 16);
                if (frameSize == 0) frameSize = 16;
            }

            // Module / function generation
            void BuildLayouts() {
                for (const auto& mod : pkg.modules) {
                    for (const auto& name : mod.interfaceNames)
                        interfaceNames.insert(name);
                    for (const auto& s : mod.structs)
                        layouts[s.name] = ComputeLayout(s, layouts);
                }
            }

            void GenModule(const LirModule& mod) {
                // Extern variable declarations
                for (const auto& ev : mod.externVars) {
                    NeedExtern(ev.name);
                    // Emit a comment in .data noting the extern
                    data << "; extern var: " << ev.name << "\n";
                }

                // Module-level constants in .rodata
                for (const auto& c : mod.consts) {
                    std::string vis = c.isPublic ? "global " + c.name + "\n" : "";
                    data << vis;
                    data << c.name << ":  ; " << c.type.ToString() << " = " << c.value << "\n";
                    data << "    ; (constant — initialized at link time)\n";
                }

                // Vtables
                for (const auto& vt : mod.vtables) {
                    rodata << vt.label << ":\n";
                    for (const auto& m : vt.methods)
                        rodata << "    dq " << m << "\n";
                }

                // Functions
                for (const auto& func : mod.funcs)
                    GenFunc(func);
            }

            void GenFunc(const LirFunc& func) {
                if (func.isExtern) {
                    NeedExtern(func.name);
                    return;
                }

                curFunc = func.name;
                PrepassFunc(func);

                // Global / visibility declaration
                if (func.isPublic) globals << "global " << func.name << "\n";

                TB();
                TC(std::format("── {} ─", func.name));
                TL(func.name);

                // Prologue
                TI("push    rbp");
                TI("mov     rbp, rsp");
                if (frameSize > 0) TI(std::format("sub     rsp, {}", frameSize));

                // Save callee-saved registers that the allocator assigned to vregs.
                // Use MOV into reserved frame slots (not push/pop) so all rbp-relative
                // offsets remain stable regardless of the number of saved registers.
                // Only callee-saved regs need saving; caller-saved don't.
                for (int i = 0; i < kCalleeSavedCount; i++) {
                    if (csaveUsed[i])
                        TI(std::format("{:<8}qword [rbp - {}], {}", "mov",
                                       csaveSlots[i], kPhysReg64[i]));
                }

                // Move parameter ABI registers into their destinations.
                // If a param was allocated to a callee-saved physical reg → MOV directly.
                // Otherwise spill to its stack slot as before.
                int intArgIdx = 0, fltArgIdx = 0;
                for (const auto& p : func.params) {
                    int sz = SizeOf(p.type);
                    if (IsFloat(p.type)) {
                        if (fltArgIdx < 8) {
                            int off = slotMap.at(p.reg); // float params always spilled
                            TI(std::format("{:<8}{} [rbp - {}], {}",
                                           sz == 4 ? "movss" : "movsd",
                                           PtrSize(sz), off, kFltArgRegs[fltArgIdx]));
                            fltArgIdx++;
                        }
                    } else {
                        if (intArgIdx < 6) {
                            const std::string_view abiReg = kIntArgRegs[intArgIdx];
                            if (auto it = regAssign.find(p.reg); it != regAssign.end()) {
                                const std::string_view dest = kPhysReg64[static_cast<int>(it->second)];
                                if (dest != abiReg)
                                    TI(std::format("{:<8}{}, {}", "mov", dest, abiReg));
                            } else {
                                int off = slotMap.at(p.reg);
                                TI(std::format("{:<8}{} [rbp - {}], {}",
                                               "mov", PtrSize(std::max(sz, 1)), off, abiReg));
                            }
                            intArgIdx++;
                        }
                    }
                }

                // Basic blocks
                for (uint32_t bi = 0; bi < func.blocks.size(); bi++)
                    GenBlock(bi, func.blocks[bi], func);

                TB();
            }

            void GenBlock(uint32_t idx, const LirBlock& block, const LirFunc& func) {
                // Emit label for every block after entry
                std::string lbl = BlockLabel(idx, block.label);
                if (idx != 0) {
                    TB();
                    TL(lbl);
                }

                for (const auto& instr : block.instrs)
                    GenInstr(instr, func);

                if (block.term)
                    GenTerminator(idx, *block.term, func);
                else
                    TI("nop    ; missing terminator");
            }

            // ── Optimized register-to-register binary operation ──────────────────
            // When both sources and the destination are register-assigned (non-float),
            // emit a 2-instruction sequence (mov + op) instead of the 4-instruction
            // LoadA/LoadB/op/StoreA sequence through the rax accumulator.
            //
            // For addition, uses LEA for a single-instruction 3-operand form.
            //
            // Returns true if the optimized path was emitted.
            bool TryEmitRegRegBinOp(const LirInstr& instr, std::string_view opName) {
                if (instr.srcs.size() != 2) return false;
                auto itDst = regAssign.find(instr.dst);
                auto itA   = regAssign.find(instr.srcs[0]);
                auto itB   = regAssign.find(instr.srcs[1]);
                if (itDst == regAssign.end() || itA == regAssign.end() || itB == regAssign.end())
                    return false;

                const std::string_view dstR = kPhysReg64[static_cast<int>(itDst->second)];
                const std::string_view srcA = kPhysReg64[static_cast<int>(itA->second)];
                const std::string_view srcB = kPhysReg64[static_cast<int>(itB->second)];

                // For ADD: use lea dst, [srcA + srcB] — single instruction
                if (instr.op == LirOpcode::Add) {
                    TI(std::format("{:<8}{}, [{} + {}]", "lea", dstR, srcA, srcB));
                    return true;
                }

                // For other ops: mov dst, srcA; op dst, srcB
                // (Skip mov if dst == srcA — the value is already there)
                if (dstR != srcA)
                    TI(std::format("{:<8}{}, {}", "mov", dstR, srcA));
                TI(std::format("{:<8}{}, {}", opName, dstR, srcB));
                return true;
            }

            // Optimized register-to-register comparison.
            // Returns true if emitted.
            bool TryEmitRegRegCmp(const LirInstr& instr, std::string_view setcc) {
                if (instr.srcs.size() != 2) return false;
                auto itDst = regAssign.find(instr.dst);
                auto itA   = regAssign.find(instr.srcs[0]);
                auto itB   = regAssign.find(instr.srcs[1]);
                if (itDst == regAssign.end() || itA == regAssign.end() || itB == regAssign.end())
                    return false;

                const std::string_view dstR  = kPhysReg64[static_cast<int>(itDst->second)];
                const std::string_view dst8  = kPhysReg8[static_cast<int>(itDst->second)];
                const std::string_view srcA  = kPhysReg64[static_cast<int>(itA->second)];
                const std::string_view srcB  = kPhysReg64[static_cast<int>(itB->second)];

                TI(std::format("{:<8}{}, {}", "cmp", srcA, srcB));
                TI(std::format("{:<8}{}", setcc, dst8));
                TI(std::format("{:<8}{}, {}", "movzx", dstR, dst8));
                return true;
            }

            // Instruction generation
            void GenInstr(const LirInstr& instr, const LirFunc& /*func*/) {
                switch (instr.op) {
                case LirOpcode::Const: {
                    if (instr.dst == LirNoReg) break;
                    const TypeRef& t = instr.type;
                    int sz = SizeOf(t);
                    if (t.kind == TypeRef::Kind::Str) {
                        std::string lbl = InternStr(instr.strArg);
                        // If register-assigned, LEA directly into the physical reg
                        if (auto it = regAssign.find(instr.dst); it != regAssign.end()) {
                            TI(std::format("{:<8}{}, {}", "lea",
                                           kPhysReg64[static_cast<int>(it->second)], lbl));
                            break;
                        }
                        TI(std::format("{:<8}rax, {}", "lea", lbl));
                    }
                    else if (t.kind == TypeRef::Kind::Float32) {
                        std::string lbl = InternF32(instr.strArg);
                        TI(std::format("{:<8}xmm0, dword [rel {}]", "movss", lbl));
                        TI(std::format("{:<8}dword [rbp - {}], xmm0", "movss", slotMap.at(instr.dst)));
                        break;
                    }
                    else if (t.kind == TypeRef::Kind::Float64) {
                        std::string lbl = InternF64(instr.strArg);
                        TI(std::format("{:<8}xmm0, qword [rel {}]", "movsd", lbl));
                        TI(std::format("{:<8}qword [rbp - {}], xmm0", "movsd", slotMap.at(instr.dst)));
                        break;
                    }
                    else if (t.kind == TypeRef::Kind::Bool) {
                        std::string v = (instr.strArg == "true" || instr.strArg == "1") ? "1" : "0";
                        // If register-assigned, MOV directly
                        if (auto it = regAssign.find(instr.dst); it != regAssign.end()) {
                            TI(std::format("{:<8}{}, {}", "mov",
                                           kPhysReg64[static_cast<int>(it->second)], v));
                            break;
                        }
                        TI(std::format("{:<8}rax, {}", "mov", v));
                    }
                    else {
                        // Integer / char: the literal is the numeric value
                        const std::string& val = instr.strArg.empty() ? "0" : instr.strArg;
                        // If register-assigned, MOV directly (skip rax)
                        if (auto it = regAssign.find(instr.dst); it != regAssign.end()) {
                            TI(std::format("{:<8}{}, {}", "mov",
                                           kPhysReg64[static_cast<int>(it->second)], val));
                            break;
                        }
                        TI(std::format("{:<8}rax, {}", "mov", val));
                    }
                    StoreA(instr.dst, sz > 0 ? t : TypeRef::MakeInt64());
                    break;
                }

                case LirOpcode::Alloca: {
                    int32_t dataOff = allocaData.at(instr.dst);
                    TI(std::format("{:<8}rax, [rbp - {}]", "lea", dataOff));
                    StoreGpr64(instr.dst, "rax");
                    break;
                }

                case LirOpcode::Load: {
                    const TypeRef& t = instr.type;
                    int sz = SizeOf(t);
                    int runtimeSz = SizeOfRuntime(t);
                    if (!instr.strArg.empty()) {
                        // Named global / constant
                        TI(std::format("{:<8}rax, [rel {}]", "mov", instr.strArg));
                    }
                    else {
                        // Load through pointer in srcs[0]
                        LirReg ptr = instr.srcs[0];
                        LoadGpr64(ptr, "r10");
                        if (runtimeSz == 16) {
                            TI(std::format("{:<8}rax, qword [r10]", "mov"));
                            TI(std::format("{:<8}qword [rbp - {}], rax", "mov", slotMap.at(instr.dst)));
                            TI(std::format("{:<8}rax, qword [r10 + 8]", "mov"));
                            TI(std::format("{:<8}qword [rbp - {}], rax", "mov", slotMap.at(instr.dst) - 8));
                            break;
                        }
                        if (IsFloat(t)) {
                            TI(std::format("{:<8}xmm0, {} [r10]", sz == 4 ? "movss" : "movsd", PtrSize(sz)));
                            TI(std::format("{:<8}{} [rbp - {}], xmm0",
                                           sz == 4 ? "movss" : "movsd",
                                           PtrSize(sz),
                                           slotMap.at(instr.dst)));
                            break;
                        }
                        else if (sz == 8 || sz == 0) {
                            TI(std::format("{:<8}rax, qword [r10]", "mov"));
                        }
                        else if (t.IsSigned()) {
                            TI(std::format("{:<8}rax, {} [r10]", sz == 4 ? "movsxd" : "movsx", PtrSize(sz)));
                        }
                        else {
                            if (sz == 4)
                                TI(std::format("{:<8}eax, dword [r10]", "mov"));
                            else
                                TI(std::format("{:<8}rax, {} [r10]", "movzx", PtrSize(sz)));
                        }
                    }
                    StoreA(instr.dst, sz > 0 ? t : TypeRef::MakeInt64());
                    break;
                }

                case LirOpcode::Store: {
                    LirReg val = instr.srcs[0];
                    LirReg ptr = instr.srcs[1];
                    const TypeRef& t = instr.type;
                    int sz = SizeOf(t);
                    int runtimeSz = SizeOfRuntime(t);

                    // Load pointer
                    LoadGpr64(ptr, "r11");

                    if (runtimeSz == 16) {
                        TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", slotMap.at(val)));
                        TI(std::format("{:<8}qword [r11], rax", "mov"));
                        TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", slotMap.at(val) - 8));
                        TI(std::format("{:<8}qword [r11 + 8], rax", "mov"));
                    }
                    else if (IsFloat(t)) {
                        TI(std::format(
                            "{:<8}xmm0, {} [rbp - {}]", sz == 4 ? "movss" : "movsd", PtrSize(sz), slotMap.at(val)));
                        TI(std::format("{:<8}{} [r11], xmm0", sz == 4 ? "movss" : "movsd", PtrSize(sz)));
                    }
                    else {
                        int store_sz = (sz > 0) ? sz : 8;
                        LoadA(val, t);
                        TI(std::format("{:<8}{} [r11], {}", "mov", PtrSize(store_sz), GprA(store_sz)));
                    }
                    break;
                }

                case LirOpcode::Add:
                case LirOpcode::Sub:
                case LirOpcode::And:
                case LirOpcode::Or:
                case LirOpcode::Xor: {
                    const TypeRef& t = instr.type;
                    if (IsFloat(t)) {
                        LoadA(instr.srcs[0], t);
                        LoadB(instr.srcs[1], t);
                        std::string_view op;
                        bool f32 = (t.kind == TypeRef::Kind::Float32);
                        switch (instr.op) {
                        case LirOpcode::Add:
                            op = f32 ? "addss" : "addsd";
                            break;
                        case LirOpcode::Sub:
                            op = f32 ? "subss" : "subsd";
                            break;
                        default:
                            op = f32 ? "addss" : "addsd";
                            break;
                        }
                        TI(std::format("{:<8}xmm0, xmm1", op));
                        StoreA(instr.dst, t);
                    }
                    else {
                        // Try optimized reg-to-reg path first
                        std::string_view opN;
                        switch (instr.op) {
                        case LirOpcode::Add: opN = "add"; break;
                        case LirOpcode::Sub: opN = "sub"; break;
                        case LirOpcode::And: opN = "and"; break;
                        case LirOpcode::Or:  opN = "or";  break;
                        case LirOpcode::Xor: opN = "xor"; break;
                        default:             opN = "add"; break;
                        }
                        if (!TryEmitRegRegBinOp(instr, opN)) {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            TI(std::format("{:<8}rax, r10", opN));
                            StoreA(instr.dst, t);
                        }
                    }
                    break;
                }

                case LirOpcode::Mul: {
                    const TypeRef& t = instr.type;
                    if (IsFloat(t)) {
                        LoadA(instr.srcs[0], t);
                        LoadB(instr.srcs[1], t);
                        TI(t.kind == TypeRef::Kind::Float32 ? "mulss   xmm0, xmm1" : "mulsd   xmm0, xmm1");
                        StoreA(instr.dst, t);
                    }
                    else {
                        if (!TryEmitRegRegBinOp(instr, "imul")) {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            TI("imul    rax, r10");
                            StoreA(instr.dst, t);
                        }
                    }
                    break;
                }

                case LirOpcode::Div:
                case LirOpcode::Mod: {
                    const TypeRef& t = instr.type;
                    if (IsFloat(t)) {
                        LoadA(instr.srcs[0], t);
                        LoadB(instr.srcs[1], t);
                        TI(t.kind == TypeRef::Kind::Float32 ? "divss   xmm0, xmm1" : "divsd   xmm0, xmm1");
                        StoreA(instr.dst, t);
                    }
                    else {
                        LoadA(instr.srcs[0], t);
                        LoadB(instr.srcs[1], t);
                        // idiv uses rdx:rax; result in rax (quotient) or rdx (remainder)
                        if (t.IsSigned()) {
                            TI("cqo"); // sign-extend rax → rdx:rax
                            TI("idiv    r10");
                        }
                        else {
                            TI("xor     rdx, rdx"); // zero-extend rax
                            TI("div     r10");
                        }
                        if (instr.op == LirOpcode::Mod) TI("mov     rax, rdx"); // remainder is in rdx
                        StoreA(instr.dst, t);
                    }
                    break;
                }

                case LirOpcode::Pow: {
                    // Integer ** calls the in-unit __rux_ipow helper; float ** calls libm pow.
                    const TypeRef& t = instr.type;
                    if (IsFloat(t)) {
                        NeedExtern("pow");
                        LoadA(instr.srcs[0], t);
                        LoadB(instr.srcs[1], t);
                        TI("movsd   xmm0, xmm0");
                        TI("movsd   xmm1, xmm1");
                        TI("call    pow");
                    }
                    else {
                        usesIpow = true;
                        LoadA(instr.srcs[0], t);
                        LoadB(instr.srcs[1], t);
                        TI("mov     rdi, rax");
                        TI("mov     rsi, r10");
                        TI("call    __rux_ipow");
                    }
                    StoreA(instr.dst, t);
                    break;
                }

                case LirOpcode::Shl:
                case LirOpcode::Shr: {
                    const TypeRef& t = instr.type;
                    LoadA(instr.srcs[0], t);
                    // Shift count must be in cl
                    LoadGpr64(instr.srcs[1], "r11");
                    TI("mov     rcx, r11");
                    if (bool isShr = (instr.op == LirOpcode::Shr); isShr && t.IsSigned())
                        TI("sar     rax, cl");
                    else if (isShr)
                        TI("shr     rax, cl");
                    else
                        TI("shl     rax, cl");
                    StoreA(instr.dst, t);
                    break;
                }

                case LirOpcode::Neg: {
                    const TypeRef& t = instr.type;
                    if (IsFloat(t)) {
                        LoadA(instr.srcs[0], t);
                        // Negate by XOR with sign bit
                        if (t.kind == TypeRef::Kind::Float32) {
                            std::string lbl = InternF32("0x80000000");
                            TI(std::format("movss   xmm1, dword [rel {}]", lbl));
                            TI("xorps   xmm0, xmm1");
                        }
                        else {
                            std::string lbl = InternF64("0x8000000000000000");
                            TI(std::format("movsd   xmm1, qword [rel {}]", lbl));
                            TI("xorpd   xmm0, xmm1");
                        }
                        StoreA(instr.dst, t);
                    }
                    else {
                        LoadA(instr.srcs[0], t);
                        TI("neg     rax");
                        StoreA(instr.dst, t);
                    }
                    break;
                }

                case LirOpcode::Not: {
                    // Logical not: result = (operand == 0) ? 1 : 0
                    const TypeRef& t = instr.type;
                    LoadA(instr.srcs[0], t);
                    TI("test    rax, rax");
                    TI("setz    al");
                    TI("movzx   rax, al");
                    StoreA(instr.dst, TypeRef::MakeBool());
                    break;
                }

                case LirOpcode::BitNot: {
                    const TypeRef& t = instr.type;
                    LoadA(instr.srcs[0], t);
                    TI("not     rax");
                    StoreA(instr.dst, t);
                    break;
                }

                case LirOpcode::CmpEq:
                case LirOpcode::CmpNe:
                case LirOpcode::CmpLt:
                case LirOpcode::CmpLe:
                case LirOpcode::CmpGt:
                case LirOpcode::CmpGe: {
                    const TypeRef& lhsT = regTypes.contains(instr.srcs[0]) ? regTypes.at(instr.srcs[0]) : instr.type;
                    if (!IsFloat(lhsT)) {
                        // Determine the SETcc instruction
                        std::string_view set;
                        bool sig = lhsT.IsSigned();
                        switch (instr.op) {
                        case LirOpcode::CmpEq:  set = "sete";                   break;
                        case LirOpcode::CmpNe:  set = "setne";                  break;
                        case LirOpcode::CmpLt:  set = sig ? "setl"  : "setb";   break;
                        case LirOpcode::CmpLe:  set = sig ? "setle" : "setbe";  break;
                        case LirOpcode::CmpGt:  set = sig ? "setg"  : "seta";   break;
                        default:                set = sig ? "setge" : "setae";   break;
                        }
                        if (TryEmitRegRegCmp(instr, set)) break;
                    }
                    // Fallback: float or spilled operands
                    LoadA(instr.srcs[0], lhsT);
                    LoadB(instr.srcs[1], lhsT);
                    if (IsFloat(lhsT)) {
                        TI(lhsT.kind == TypeRef::Kind::Float32 ? "ucomiss xmm0, xmm1" : "ucomisd xmm0, xmm1");
                        std::string_view set;
                        switch (instr.op) {
                        case LirOpcode::CmpEq:  set = "sete";  break;
                        case LirOpcode::CmpNe:  set = "setne"; break;
                        case LirOpcode::CmpLt:  set = "setb";  break;
                        case LirOpcode::CmpLe:  set = "setbe"; break;
                        case LirOpcode::CmpGt:  set = "seta";  break;
                        default:                set = "setae";  break;
                        }
                        TI(std::format("{:<8}al", set));
                    }
                    else {
                        TI("cmp     rax, r10");
                        std::string_view set;
                        bool sig = lhsT.IsSigned();
                        switch (instr.op) {
                        case LirOpcode::CmpEq:  set = "sete";                   break;
                        case LirOpcode::CmpNe:  set = "setne";                  break;
                        case LirOpcode::CmpLt:  set = sig ? "setl"  : "setb";   break;
                        case LirOpcode::CmpLe:  set = sig ? "setle" : "setbe";  break;
                        case LirOpcode::CmpGt:  set = sig ? "setg"  : "seta";   break;
                        default:                set = sig ? "setge" : "setae";   break;
                        }
                        TI(std::format("{:<8}al", set));
                    }
                    TI("movzx   rax, al");
                    StoreA(instr.dst, TypeRef::MakeBool());
                    break;
                }

                case LirOpcode::Cast: {
                    const TypeRef& dst_t = instr.type;
                    TypeRef src_t;
                    // strArg holds the source type string; try to reconstruct enough info
                    // by looking up the register type
                    if (regTypes.contains(instr.srcs[0]))
                        src_t = regTypes.at(instr.srcs[0]);
                    else
                        src_t = dst_t;

                    bool srcFloat = IsFloat(src_t);
                    bool dstFloat = IsFloat(dst_t);

                    // Optimized: int→int cast where both are register-assigned
                    // is just a direct register move (or nothing if same reg).
                    if (!srcFloat && !dstFloat) {
                        auto itSrc = regAssign.find(instr.srcs[0]);
                        auto itDst = regAssign.find(instr.dst);
                        if (itSrc != regAssign.end() && itDst != regAssign.end()) {
                            if (itSrc->second != itDst->second)
                                TI(std::format("{:<8}{}, {}", "mov",
                                               kPhysReg64[static_cast<int>(itDst->second)],
                                               kPhysReg64[static_cast<int>(itSrc->second)]));
                            break;
                        }
                    }

                    LoadA(instr.srcs[0], src_t);

                    if (!srcFloat && !dstFloat) {
                        // int → int: sign/zero extend is already done by LoadA;
                        // for narrowing, the lower bits in rax are already correct.
                        // Nothing extra needed for most cases.
                    }
                    else if (srcFloat && !dstFloat) {
                        // float → int
                        bool f32src = (src_t.kind == TypeRef::Kind::Float32);
                        if (bool signed_ = dst_t.IsSigned(); signed_)
                            TI(f32src ? "cvttss2si rax, xmm0" : "cvttsd2si rax, xmm0");
                        else {
                            // Unsigned: no direct instruction; use signed then re-interpret
                            TI(f32src ? "cvttss2si rax, xmm0" : "cvttsd2si rax, xmm0");
                        }
                    }
                    else if (!srcFloat && dstFloat) {
                        // int → float
                        bool f32dst = (dst_t.kind == TypeRef::Kind::Float32);
                        TI(f32dst ? "cvtsi2ss  xmm0, rax" : "cvtsi2sd  xmm0, rax");
                    }
                    else {
                        // float → float
                        bool f32src = (src_t.kind == TypeRef::Kind::Float32);
                        bool f32dst = (dst_t.kind == TypeRef::Kind::Float32);
                        if (f32src && !f32dst)
                            TI("cvtss2sd  xmm0, xmm0");
                        else if (!f32src && f32dst)
                            TI("cvtsd2ss  xmm0, xmm0");
                    }
                    StoreA(instr.dst, dst_t);
                    break;
                }

                case LirOpcode::Call: {
                    EmitCall(instr.strArg, instr.srcs, instr.dst, instr.type, instr.callConv);
                    break;
                }

                case LirOpcode::CallIndirect: {
                    // strArg empty; srcs[0] = callee, srcs[1..] = args
                    EmitCallIndirect(instr.srcs, instr.dst, instr.type, instr.callConv);
                    break;
                }

                case LirOpcode::FieldPtr: {
                    LirReg base = instr.srcs[0];
                    // Load base pointer
                    LoadGpr64(base, "rax");

                    // Compute field offset using struct layout
                    int fieldOff = ResolveFieldOffset(base, instr.strArg);
                    if (fieldOff != 0) TI(std::format("{:<8}rax, [rax + {}]", "lea", fieldOff));
                    // else pointer is already at the field start
                    StoreGpr64(instr.dst, "rax");
                    break;
                }

                case LirOpcode::IndexPtr: {
                    LirReg base = instr.srcs[0];
                    LirReg idx = instr.srcs[1];
                    int elemSz = (instr.type.kind == TypeRef::Kind::Pointer && !instr.type.inner.empty())
                        ? SizeOfRuntime(instr.type.inner[0])
                        : 8;
                    if (elemSz < 1) elemSz = 1;

                    LoadGpr64(base, "rax");
                    LoadB(idx, regTypes.at(idx));
                    TI(std::format("{:<8}r11, r10, {}", "imul", elemSz));
                    TI("add     rax, r11");
                    StoreGpr64(instr.dst, "rax");
                    break;
                }

                case LirOpcode::Phi:
                    // Phi moves are emitted by predecessors before their terminators.
                    // The phi dst slot is already allocated; nothing to emit here.
                    break;

                case LirOpcode::GlobalAddr:
                    TI(std::format("{:<8}rax, [rel {}]", "lea", instr.strArg));
                    StoreGpr64(instr.dst, "rax");
                    break;

                default:
                    TC(std::format("TODO: opcode {}", static_cast<int>(instr.op)));
                    break;
                }
            }

            // Field offset resolution via regTypes_ + layouts_
            int ResolveFieldOffset(LirReg base, const std::string& fieldName) {
                auto typeIt = regTypes.find(base);
                if (typeIt == regTypes.end()) return 0;
                const TypeRef& ptrType = typeIt->second;
                if (ptrType.kind != TypeRef::Kind::Pointer || ptrType.inner.empty()) return 0;
                const TypeRef& inner = ptrType.inner[0];
                if (inner.kind == TypeRef::Kind::Tuple) {
                    std::size_t idx = 0;
                    try {
                        idx = std::stoul(fieldName);
                    }
                    catch (...) {
                        return 0;
                    }
                    int offset = 0;
                    for (std::size_t i = 0; i < idx && i < inner.inner.size(); ++i)
                        offset += SizeOf(inner.inner[i]);
                    return offset;
                }
                if (inner.kind != TypeRef::Kind::Named) return 0;
                const std::string baseName = BaseTypeName(inner.name);
                if (interfaceNames.count(baseName)) {
                    if (fieldName == "data") return 0;
                    if (fieldName == "vtable") return 8;
                    return 0;
                }
                if (baseName == "Slice") {
                    if (fieldName == "data") return 0;
                    if (fieldName == "length") return 8;
                    return 0;
                }
                auto layIt = layouts.find(baseName);
                if (layIt == layouts.end()) return 0;
                for (const auto& f : layIt->second.fields)
                    if (f.name == fieldName) return f.offset;
                return 0;
            }

            // Call emission
            void EmitCall(const std::string& callee,
                          const std::vector<LirReg>& args,
                          LirReg dst,
                          const TypeRef& retType,
                          CallingConvention callConv) {
                const bool win64 = callConv == CallingConvention::Win64;
                const auto* intRegs = win64 ? kWin64IntArgRegs : kIntArgRegs;
                const int maxIntRegs = win64 ? 4 : 6;
                std::vector<LirReg> stackArgs;
                // Set up arguments into ABI registers.
                // Uses LoadGpr64 which handles both register-assigned and
                // stack-spilled vregs correctly.
                int intIdx = 0, fltIdx = 0;
                for (LirReg arg : args) {
                    TypeRef at = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64();
                    if (IsFloat(at)) {
                        if (fltIdx < 8) {
                            int sz = SizeOf(at);
                            if (auto rit = regAssign.find(arg); rit != regAssign.end()) {
                                // Float in int reg (shouldn't normally happen, but be safe)
                                TI(std::format("{:<8}{}, {} [rbp - {}]",
                                               sz == 4 ? "movss" : "movsd",
                                               kFltArgRegs[fltIdx],
                                               PtrSize(sz),
                                               slotMap.at(arg)));
                            } else {
                                TI(std::format("{:<8}{}, {} [rbp - {}]",
                                               sz == 4 ? "movss" : "movsd",
                                               kFltArgRegs[fltIdx],
                                               PtrSize(sz),
                                               slotMap.at(arg)));
                            }
                            fltIdx++;
                        }
                    }
                    else {
                        if (intIdx < maxIntRegs) {
                            LoadGpr64(arg, intRegs[intIdx]);
                            intIdx++;
                        }
                        else if (win64) {
                            stackArgs.push_back(arg);
                        }
                    }
                }
                const int stackBytes = win64 ? 32 + AlignUp(static_cast<int>(stackArgs.size()) * 8, 16) : 0;
                if (win64) {
                    TI(std::format("sub     rsp, {}", stackBytes));
                    for (std::size_t i = 0; i < stackArgs.size(); ++i) {
                        LoadGpr64(stackArgs[i], "rax");
                        TI(std::format("{:<8}qword [rsp + {}], rax", "mov", 32 + i * 8));
                    }
                }
                // Stack is already 16-byte aligned: prologue sub rsp,frameSize ensures
                // rsp ≡ 8 (mod 16) which the ABI requires before a call instruction.
                TI(std::format("{:<8}{}", "call", callee));
                if (win64) TI(std::format("add     rsp, {}", stackBytes));
                if (dst != LirNoReg && !retType.IsOpaque()) StoreA(dst, retType);
            }

            void EmitCallIndirect(const std::vector<LirReg>& srcs,
                                  LirReg dst,
                                  const TypeRef& retType,
                                  CallingConvention callConv) {
                if (srcs.empty()) return;
                LirReg callee = srcs[0];
                std::vector<LirReg> args(srcs.begin() + 1, srcs.end());
                const std::vector<LirReg> stackArgs = EmitCallArgs(args, callConv);
                const int stackBytes =
                    callConv == CallingConvention::Win64 ? 32 + AlignUp(static_cast<int>(stackArgs.size()) * 8, 16) : 0;
                if (callConv == CallingConvention::Win64) {
                    TI(std::format("sub     rsp, {}", stackBytes));
                    StoreWin64StackArgs(stackArgs);
                }
                // Load the callee after preparing args because arg setup uses r10.
                LoadGpr64(callee, "r10");
                TI("call    r10");
                if (callConv == CallingConvention::Win64) TI(std::format("add     rsp, {}", stackBytes));
                if (dst != LirNoReg && !retType.IsOpaque()) StoreA(dst, retType);
            }

            void StoreWin64StackArgs(const std::vector<LirReg>& stackArgs) {
                for (std::size_t i = 0; i < stackArgs.size(); ++i) {
                    LoadGpr64(stackArgs[i], "rax");
                    TI(std::format("{:<8}qword [rsp + {}], rax", "mov", 32 + i * 8));
                }
            }

            std::vector<LirReg> EmitCallArgs(const std::vector<LirReg>& args, CallingConvention callConv) {
                const bool win64 = callConv == CallingConvention::Win64;
                const auto* intRegs = win64 ? kWin64IntArgRegs : kIntArgRegs;
                const int maxIntRegs = win64 ? 4 : 6;
                std::vector<LirReg> stackArgs;
                int intIdx = 0, fltIdx = 0;
                for (LirReg arg : args) {
                    TypeRef at = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64();
                    if (IsFloat(at)) {
                        if (fltIdx < 8) {
                            const int sz = SizeOf(at);
                            TI(std::format("{:<8}{}, {} [rbp - {}]",
                                           sz == 4 ? "movss" : "movsd",
                                           kFltArgRegs[fltIdx],
                                           PtrSize(sz),
                                           slotMap.at(arg)));
                            fltIdx++;
                        }
                    }
                    else {
                        if (intIdx < maxIntRegs) {
                            LoadGpr64(arg, intRegs[intIdx]);
                            intIdx++;
                        }
                        else if (win64) {
                            stackArgs.push_back(arg);
                        }
                    }
                }
                return stackArgs;
            }

            // Terminator generation
            void GenTerminator(uint32_t blockIdx, const LirTerminator& term, const LirFunc& func) {
                switch (term.kind) {
                case LirTermKind::Jump: {
                    EmitPhiMoves(blockIdx, term.trueTarget);
                    TI(std::format("{:<8}{}", "jmp", BlockLabel(term.trueTarget, func.blocks[term.trueTarget].label)));
                    break;
                }

                case LirTermKind::Branch: {
                    // If condition is register-assigned, TEST it directly
                    if (auto it = regAssign.find(term.cond); it != regAssign.end()) {
                        const std::string_view condR = kPhysReg64[static_cast<int>(it->second)];
                        TI(std::format("{:<8}{}, {}", "test", condR, condR));
                    } else {
                        LoadGpr64(term.cond, "rax");
                        TI("test    rax, rax");
                    }

                    std::string trueLabel = BlockLabel(term.trueTarget, func.blocks[term.trueTarget].label);
                    std::string falseLabel = BlockLabel(term.falseTarget, func.blocks[term.falseTarget].label);

                    // Check whether either branch needs phi moves
                    bool truePhi = HasPhiMoves(blockIdx, term.trueTarget);
                    bool falsePhi = HasPhiMoves(blockIdx, term.falseTarget);

                    if (!truePhi && !falsePhi) {
                        TI(std::format("{:<8}{}", "jz", falseLabel));
                        TI(std::format("{:<8}{}", "jmp", trueLabel));
                    }
                    else {
                        // Use intermediate labels so each path has room for phi moves
                        std::string trampTrue = std::format(".{}_br{}_t", curFunc, blockIdx);
                        std::string trampFalse = std::format(".{}_br{}_f", curFunc, blockIdx);
                        TI(std::format("{:<8}{}", "jz", trampFalse));
                        TL(trampTrue);
                        EmitPhiMoves(blockIdx, term.trueTarget);
                        TI(std::format("{:<8}{}", "jmp", trueLabel));
                        TL(trampFalse);
                        EmitPhiMoves(blockIdx, term.falseTarget);
                        TI(std::format("{:<8}{}", "jmp", falseLabel));
                    }
                    break;
                }

                case LirTermKind::Return: {
                    if (term.retVal && *term.retVal != LirNoReg) {
                        LoadA(*term.retVal, term.retType);
                        // Result already in rax or xmm0 — do not overwrite
                    }
                    EmitEpilogue();
                    break;
                }

                case LirTermKind::Switch: {
                    LoadGpr64(term.cond, "rax");
                    for (const auto& c : term.cases) {
                        TI(std::format("{:<8}rax, {}", "cmp", c.value));
                        std::string lbl = BlockLabel(c.target, func.blocks[c.target].label);
                        TI(std::format("{:<8}{}", "je", lbl));
                    }
                    EmitPhiMoves(blockIdx, term.defaultTarget);
                    TI(std::format(
                        "{:<8}{}", "jmp", BlockLabel(term.defaultTarget, func.blocks[term.defaultTarget].label)));
                    break;
                }
                }
            }

            [[nodiscard]] bool HasPhiMoves(uint32_t from, uint32_t to) const {
                auto it = phiMoves.find(from);
                if (it == phiMoves.end()) return false;
                return it->second.contains(to);
            }

            void EmitEpilogue() {
                // Restore callee-saved regs in reverse order before returning.
                // Only callee-saved regs have frame slots; caller-saved don't need restore.
                for (int i = kCalleeSavedCount - 1; i >= 0; i--) {
                    if (csaveUsed[i])
                        TI(std::format("{:<8}{}, qword [rbp - {}]", "mov",
                                       kPhysReg64[i], csaveSlots[i]));
                }
                TI("leave");
                TI("ret");
            }
        };

        // AsmGen::Generate
        std::string AsmGen::Generate() {
            BuildLayouts();
            for (const auto& mod : pkg.modules)
                GenModule(mod);
            if (usesIpow) EmitIntPowHelper();
            std::ostringstream out;
            out << "; Generated by Rux Compiler\n";
            out << "; Target:  x86-64  (System V AMD64 ABI, NASM syntax)\n";
            out << "; Calling: rdi/rsi/rdx/rcx/r8/r9 (int args), xmm0-7 (float args)\n";
            out << "; Scratch: r10, r11 (caller-saved)\n";
            out << "\n";
            out << "bits 64\n\n";
            if (const std::string ext = externs.str(); !ext.empty()) {
                out << ext << "\n";
            }
            if (const std::string glb = globals.str(); !glb.empty()) {
                out << glb << "\n";
            }
            if (const std::string rod = rodata.str(); !rod.empty()) {
                out << "section .rodata\n" << rod << "\n";
            }
            if (const std::string dat = data.str(); !dat.empty()) {
                out << "section .data\n" << dat << "\n";
            }
            if (const std::string cod = text.str(); !cod.empty()) {
                out << "section .text\n" << cod;
            }
            return out.str();
        }
    } // namespace

    // Public API
    Asm::Asm(LirPackage package)
        : lir(std::move(package)) {
    }

    std::string Asm::Generate() const {
        AsmGen gen(lir);
        return gen.Generate();
    }

    bool Asm::Emit(const LirPackage& package, const std::filesystem::path& path) {
        AsmGen gen(package);
        std::string text = gen.Generate();
        std::ofstream f(path, std::ios::out | std::ios::trunc);
        if (!f) return false;
        f << text;
        return f.good();
    }
} // namespace Rux

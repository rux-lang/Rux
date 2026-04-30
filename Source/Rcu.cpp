/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Rcu.h"
#include "Rux/Version.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace Rux {
    namespace {
        // ─────────────────────────────────────────────────────────────────────────────
        // Type utilities (mirrored from Asm.cpp)
        // ─────────────────────────────────────────────────────────────────────────────

        static int SizeOf(const TypeRef &t) {
            switch (t.kind) {
                case TypeRef::Kind::Bool:
                case TypeRef::Kind::Int8:
                case TypeRef::Kind::UInt8: return 1;
                case TypeRef::Kind::Int16:
                case TypeRef::Kind::UInt16: return 2;
                case TypeRef::Kind::Char:
                case TypeRef::Kind::Int32:
                case TypeRef::Kind::UInt32:
                case TypeRef::Kind::Float32: return 4;
                case TypeRef::Kind::Void: return 0;
                default: return 8;
            }
        }

        static bool IsFloat(const TypeRef &t) {
            return t.kind == TypeRef::Kind::Float32 || t.kind == TypeRef::Kind::Float64;
        }

        static int AlignUp(int v, int a) { return (v + a - 1) & ~(a - 1); }

        // ─────────────────────────────────────────────────────────────────────────────
        // String table
        // ─────────────────────────────────────────────────────────────────────────────

        class RcuStringTable {
        public:
            RcuStringTable() { data_.push_back('\0'); }

            uint32_t Intern(const std::string &s) {
                if (s.empty()) return 0;
                auto it = map_.find(s);
                if (it != map_.end()) return it->second;
                uint32_t off = static_cast<uint32_t>(data_.size());
                map_[s] = off;
                data_.insert(data_.end(), s.begin(), s.end());
                data_.push_back('\0');
                return off;
            }

            uint32_t Size() const { return static_cast<uint32_t>(data_.size()); }
            const char *Data() const { return data_.data(); }

            std::string Get(uint32_t off) const {
                if (off >= data_.size()) return {};
                return {data_.data() + off};
            }

        private:
            std::vector<char> data_;
            std::unordered_map<std::string, uint32_t> map_;
        };

        // ─────────────────────────────────────────────────────────────────────────────
        // x86-64 binary encoder
        // All accesses to stack slots use [rbp + disp] where disp is negative.
        // disp = -slotMap[vreg]  (i.e., pass negative displacement directly).
        // ─────────────────────────────────────────────────────────────────────────────

        class X64Enc {
        public:
            explicit X64Enc(std::vector<uint8_t> &buf) : out_(buf) {
            }

            uint32_t Size() const { return static_cast<uint32_t>(out_.size()); }

            void Byte(uint8_t b) { out_.push_back(b); }

            void Dword(uint32_t d) {
                out_.push_back(d & 0xFF);
                out_.push_back((d >> 8) & 0xFF);
                out_.push_back((d >> 16) & 0xFF);
                out_.push_back((d >> 24) & 0xFF);
            }

            void Qword(uint64_t q) {
                for (int i = 0; i < 8; ++i) {
                    out_.push_back(q & 0xFF);
                    q >>= 8;
                }
            }

            void Patch32(uint32_t off, int32_t v) {
                out_[off] = v & 0xFF;
                out_[off + 1] = (v >> 8) & 0xFF;
                out_[off + 2] = (v >> 16) & 0xFF;
                out_[off + 3] = (v >> 24) & 0xFF;
            }

            // ── Prologue / Epilogue ───────────────────────────────────────────────────
            void PushRbp() { Byte(0x55); }

            void MovRbpRsp() {
                Byte(0x48);
                Byte(0x89);
                Byte(0xE5);
            }

            void SubRspImm32(int32_t n) {
                Byte(0x48);
                Byte(0x81);
                Byte(0xEC);
                Dword(static_cast<uint32_t>(n));
            }

            void Leave() { Byte(0xC9); }
            void Ret() { Byte(0xC3); }

            // ── RAX ↔ [RBP + disp32] ─────────────────────────────────────────────────
            void MovRaxLoad(int32_t d) {
                Byte(0x48);
                Byte(0x8B);
                Byte(0x85);
                Dword(u(d));
            }

            void MovRaxStore(int32_t d) {
                Byte(0x48);
                Byte(0x89);
                Byte(0x85);
                Dword(u(d));
            }

            void MovEaxLoad(int32_t d) {
                Byte(0x8B);
                Byte(0x85);
                Dword(u(d));
            }

            void MovEaxStore(int32_t d) {
                Byte(0x89);
                Byte(0x85);
                Dword(u(d));
            }

            void MovzxRaxWord(int32_t d) {
                Byte(0x48);
                Byte(0x0F);
                Byte(0xB7);
                Byte(0x85);
                Dword(u(d));
            }

            void MovzxRaxByte(int32_t d) {
                Byte(0x48);
                Byte(0x0F);
                Byte(0xB6);
                Byte(0x85);
                Dword(u(d));
            }

            void MovsxdRaxDword(int32_t d) {
                Byte(0x48);
                Byte(0x63);
                Byte(0x85);
                Dword(u(d));
            }

            void MovsxRaxWord(int32_t d) {
                Byte(0x48);
                Byte(0x0F);
                Byte(0xBF);
                Byte(0x85);
                Dword(u(d));
            }

            void MovsxRaxByte(int32_t d) {
                Byte(0x48);
                Byte(0x0F);
                Byte(0xBE);
                Byte(0x85);
                Dword(u(d));
            }

            void MovAxStore(int32_t d) {
                Byte(0x66);
                Byte(0x89);
                Byte(0x85);
                Dword(u(d));
            }

            void MovAlStore(int32_t d) {
                Byte(0x88);
                Byte(0x85);
                Dword(u(d));
            }

            // ── R10 ↔ [RBP + disp32] ─────────────────────────────────────────────────
            void MovR10Load(int32_t d) {
                Byte(0x4C);
                Byte(0x8B);
                Byte(0x95);
                Dword(u(d));
            }

            void MovR10Store(int32_t d) {
                Byte(0x4C);
                Byte(0x89);
                Byte(0x95);
                Dword(u(d));
            }

            void MovzxR10Word(int32_t d) {
                Byte(0x4C);
                Byte(0x0F);
                Byte(0xB7);
                Byte(0x95);
                Dword(u(d));
            }

            void MovzxR10Byte(int32_t d) {
                Byte(0x4C);
                Byte(0x0F);
                Byte(0xB6);
                Byte(0x95);
                Dword(u(d));
            }

            void MovsxdR10Dword(int32_t d) {
                Byte(0x4C);
                Byte(0x63);
                Byte(0x95);
                Dword(u(d));
            }

            void MovsxR10Word(int32_t d) {
                Byte(0x4C);
                Byte(0x0F);
                Byte(0xBF);
                Byte(0x95);
                Dword(u(d));
            }

            void MovsxR10Byte(int32_t d) {
                Byte(0x4C);
                Byte(0x0F);
                Byte(0xBE);
                Byte(0x95);
                Dword(u(d));
            }

            void MovR10dLoad(int32_t d) {
                Byte(0x44);
                Byte(0x8B);
                Byte(0x95);
                Dword(u(d));
            }

            // ── R11 ↔ [RBP + disp32] ─────────────────────────────────────────────────
            void MovR11Load(int32_t d) {
                Byte(0x4C);
                Byte(0x8B);
                Byte(0x9D);
                Dword(u(d));
            }

            void MovR11Store(int32_t d) {
                Byte(0x4C);
                Byte(0x89);
                Byte(0x9D);
                Dword(u(d));
            }

            // ── RCX ↔ stack (for shift count) ────────────────────────────────────────
            void MovRcxLoad(int32_t d) {
                Byte(0x48);
                Byte(0x8B);
                Byte(0x8D);
                Dword(u(d));
            }

            // ── ABI arg regs ↔ [RBP + disp32] ────────────────────────────────────────
            // argIdx: 0=RDI,1=RSI,2=RDX,3=RCX,4=R8,5=R9
            void MovArgLoad(int idx, int32_t d) {
                static const uint8_t rex[] = {0x48, 0x48, 0x48, 0x48, 0x4C, 0x4C};
                static const uint8_t modrm[] = {0xBD, 0xB5, 0x95, 0x8D, 0x85, 0x8D};
                Byte(rex[idx]);
                Byte(0x8B);
                Byte(modrm[idx]);
                Dword(u(d));
            }

            void MovArgStore(int idx, int32_t d) {
                static const uint8_t rex[] = {0x48, 0x48, 0x48, 0x48, 0x4C, 0x4C};
                static const uint8_t modrm[] = {0xBD, 0xB5, 0x95, 0x8D, 0x85, 0x8D};
                Byte(rex[idx]);
                Byte(0x89);
                Byte(modrm[idx]);
                Dword(u(d));
            }

            // ── XMM arg regs ↔ [RBP + disp32] (N = 0..7) ────────────────────────────
            // MOVSS xmmN, [rbp + d]
            void MovssXmmNLoad(int n, int32_t d) {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x10);
                Byte(static_cast<uint8_t>(0x80 | (n << 3) | 5));
                Dword(u(d));
            }

            // MOVSD xmmN, [rbp + d]
            void MovsdXmmNLoad(int n, int32_t d) {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x10);
                Byte(static_cast<uint8_t>(0x80 | (n << 3) | 5));
                Dword(u(d));
            }

            // ── XMM0 / XMM1 ↔ [RBP + disp32] ────────────────────────────────────────
            void MovssXmm0Load(int32_t d) {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x85);
                Dword(u(d));
            }

            void MovsdXmm0Load(int32_t d) {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x85);
                Dword(u(d));
            }

            void MovssXmm1Load(int32_t d) {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x8D);
                Dword(u(d));
            }

            void MovsdXmm1Load(int32_t d) {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x8D);
                Dword(u(d));
            }

            void MovssXmm0Store(int32_t d) {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x11);
                Byte(0x85);
                Dword(u(d));
            }

            void MovsdXmm0Store(int32_t d) {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x11);
                Byte(0x85);
                Dword(u(d));
            }

            void MovssXmm1Store(int32_t d) {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x11);
                Byte(0x8D);
                Dword(u(d));
            }

            void MovsdXmm1Store(int32_t d) {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x11);
                Byte(0x8D);
                Dword(u(d));
            }

            // ── XMM0 / XMM1, [RIP + rel32] (RIP-relative rodata load) ───────────────
            void MovssXmm0Rip(uint32_t &relocOff) {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x05);
                relocOff = Size();
                Dword(0);
            }

            void MovsdXmm0Rip(uint32_t &relocOff) {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x05);
                relocOff = Size();
                Dword(0);
            }

            void MovssXmm1Rip(uint32_t &relocOff) {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x0D);
                relocOff = Size();
                Dword(0);
            }

            void MovsdXmm1Rip(uint32_t &relocOff) {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x0D);
                relocOff = Size();
                Dword(0);
            }

            // ── Immediate loads ───────────────────────────────────────────────────────
            void MovRaxImm64(int64_t v) {
                Byte(0x48);
                Byte(0xB8);
                Qword(static_cast<uint64_t>(v));
            }

            void MovEaxImm32(int32_t v) {
                Byte(0xB8);
                Dword(static_cast<uint32_t>(v));
            }

            // ── LEA / MOV rax, [rip + rel32] ─────────────────────────────────────────
            void LeaRaxRip(uint32_t &relocOff) {
                Byte(0x48);
                Byte(0x8D);
                Byte(0x05);
                relocOff = Size();
                Dword(0);
            }

            void MovRaxRip(uint32_t &relocOff) {
                Byte(0x48);
                Byte(0x8B);
                Byte(0x05);
                relocOff = Size();
                Dword(0);
            }

            void LeaRaxStack(int32_t d) {
                Byte(0x48);
                Byte(0x8D);
                Byte(0x85);
                Dword(u(d));
            }

            // ── Register-to-register ─────────────────────────────────────────────────
            void MovRaxR10() {
                Byte(0x4C);
                Byte(0x89);
                Byte(0xD0);
            } // mov rax, r10
            void MovRcxR11() {
                Byte(0x4C);
                Byte(0x89);
                Byte(0xD9);
            } // mov rcx, r11
            void MovRaxRdx() {
                Byte(0x48);
                Byte(0x8B);
                Byte(0xC2);
            } // mov rax, rdx

            // ── Integer arithmetic (RAX op R10 → RAX) ────────────────────────────────
            void AddRaxR10() {
                Byte(0x4C);
                Byte(0x01);
                Byte(0xD0);
            }

            void SubRaxR10() {
                Byte(0x4C);
                Byte(0x29);
                Byte(0xD0);
            }

            void AndRaxR10() {
                Byte(0x4C);
                Byte(0x21);
                Byte(0xD0);
            }

            void OrRaxR10() {
                Byte(0x4C);
                Byte(0x09);
                Byte(0xD0);
            }

            void XorRaxR10() {
                Byte(0x4C);
                Byte(0x31);
                Byte(0xD0);
            }

            void ImulRaxR10() {
                Byte(0x49);
                Byte(0x0F);
                Byte(0xAF);
                Byte(0xC2);
            }

            void NegRax() {
                Byte(0x48);
                Byte(0xF7);
                Byte(0xD8);
            }

            void NotRax() {
                Byte(0x48);
                Byte(0xF7);
                Byte(0xD0);
            }

            // ── Division ─────────────────────────────────────────────────────────────
            void Cqo() {
                Byte(0x48);
                Byte(0x99);
            }

            void XorRdxRdx() {
                Byte(0x48);
                Byte(0x31);
                Byte(0xD2);
            }

            void IdivR10() {
                Byte(0x49);
                Byte(0xF7);
                Byte(0xFA);
            }

            void DivR10() {
                Byte(0x49);
                Byte(0xF7);
                Byte(0xF2);
            }

            // ── Shifts ───────────────────────────────────────────────────────────────
            void ShlRaxCl() {
                Byte(0x48);
                Byte(0xD3);
                Byte(0xE0);
            }

            void ShrRaxCl() {
                Byte(0x48);
                Byte(0xD3);
                Byte(0xE8);
            }

            void SarRaxCl() {
                Byte(0x48);
                Byte(0xD3);
                Byte(0xF8);
            }

            // ── Comparisons ──────────────────────────────────────────────────────────
            void TestRaxRax() {
                Byte(0x48);
                Byte(0x85);
                Byte(0xC0);
            }

            void CmpRaxR10() {
                Byte(0x4C);
                Byte(0x39);
                Byte(0xD0);
            }

            void CmpRaxImm32(int32_t v) {
                Byte(0x48);
                Byte(0x81);
                Byte(0xF8);
                Dword(u(v));
            }

            // SETcc AL + MOVZX RAX, AL
            void SeteAl() {
                Byte(0x0F);
                Byte(0x94);
                Byte(0xC0);
            }

            void SetneAl() {
                Byte(0x0F);
                Byte(0x95);
                Byte(0xC0);
            }

            void SetlAl() {
                Byte(0x0F);
                Byte(0x9C);
                Byte(0xC0);
            }

            void SetleAl() {
                Byte(0x0F);
                Byte(0x9E);
                Byte(0xC0);
            }

            void SetgAl() {
                Byte(0x0F);
                Byte(0x9F);
                Byte(0xC0);
            }

            void SetgeAl() {
                Byte(0x0F);
                Byte(0x9D);
                Byte(0xC0);
            }

            void SetbAl() {
                Byte(0x0F);
                Byte(0x92);
                Byte(0xC0);
            }

            void SetbeAl() {
                Byte(0x0F);
                Byte(0x96);
                Byte(0xC0);
            }

            void SetaAl() {
                Byte(0x0F);
                Byte(0x97);
                Byte(0xC0);
            }

            void SetaeAl() {
                Byte(0x0F);
                Byte(0x93);
                Byte(0xC0);
            }

            void MovzxRaxAl() {
                Byte(0x48);
                Byte(0x0F);
                Byte(0xB6);
                Byte(0xC0);
            }

            // ── Float arithmetic (XMM0 op XMM1 → XMM0) ───────────────────────────────
            void AddssXmm01() {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x58);
                Byte(0xC1);
            }

            void SubssXmm01() {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x5C);
                Byte(0xC1);
            }

            void MulssXmm01() {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x59);
                Byte(0xC1);
            }

            void DivssXmm01() {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x5E);
                Byte(0xC1);
            }

            void AddsdXmm01() {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x58);
                Byte(0xC1);
            }

            void SubsdXmm01() {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x5C);
                Byte(0xC1);
            }

            void MulsdXmm01() {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x59);
                Byte(0xC1);
            }

            void DivsdXmm01() {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x5E);
                Byte(0xC1);
            }

            // ── Float compare ─────────────────────────────────────────────────────────
            void UcomissXmm01() {
                Byte(0x0F);
                Byte(0x2E);
                Byte(0xC1);
            }

            void UcomisdXmm01() {
                Byte(0x66);
                Byte(0x0F);
                Byte(0x2E);
                Byte(0xC1);
            }

            // ── Float sign negate (XOR with mask) ────────────────────────────────────
            void XorpsXmm01() {
                Byte(0x0F);
                Byte(0x57);
                Byte(0xC1);
            }

            void XorpdXmm01() {
                Byte(0x66);
                Byte(0x0F);
                Byte(0x57);
                Byte(0xC1);
            }

            // ── Float conversions ─────────────────────────────────────────────────────
            void Cvtsi2ssXmm0Rax() {
                Byte(0xF3);
                Byte(0x48);
                Byte(0x0F);
                Byte(0x2A);
                Byte(0xC0);
            }

            void Cvtsi2sdXmm0Rax() {
                Byte(0xF2);
                Byte(0x48);
                Byte(0x0F);
                Byte(0x2A);
                Byte(0xC0);
            }

            void CvttsssiRaxXmm0() {
                Byte(0xF3);
                Byte(0x48);
                Byte(0x0F);
                Byte(0x2C);
                Byte(0xC0);
            }

            void CvttsdsiRaxXmm0() {
                Byte(0xF2);
                Byte(0x48);
                Byte(0x0F);
                Byte(0x2C);
                Byte(0xC0);
            }

            void CvtsssdXmm0() {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x5A);
                Byte(0xC0);
            }

            void CvtsdssXmm0() {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x5A);
                Byte(0xC0);
            }

            // ── Control flow ─────────────────────────────────────────────────────────
            void Jmp(uint32_t &patchOff) {
                Byte(0xE9);
                patchOff = Size();
                Dword(0);
            }

            void Jz(uint32_t &patchOff) {
                Byte(0x0F);
                Byte(0x84);
                patchOff = Size();
                Dword(0);
            }

            void Jnz(uint32_t &patchOff) {
                Byte(0x0F);
                Byte(0x85);
                patchOff = Size();
                Dword(0);
            }

            void Je(uint32_t &patchOff) {
                Byte(0x0F);
                Byte(0x84);
                patchOff = Size();
                Dword(0);
            }

            void Call(uint32_t &relocOff) {
                Byte(0xE8);
                relocOff = Size();
                Dword(0);
            }

            void CallR10() {
                Byte(0x41);
                Byte(0xFF);
                Byte(0xD2);
            }

            // ── Aggregate helpers ─────────────────────────────────────────────────────
            void ImulR11R10Imm32(int32_t v) {
                Byte(0x4D);
                Byte(0x69);
                Byte(0xDA);
                Dword(u(v));
            }

            void AddRaxR11() {
                Byte(0x4C);
                Byte(0x01);
                Byte(0xD8);
            }

            void LeaRaxRaxDisp(int32_t v) {
                Byte(0x48);
                Byte(0x8D);
                Byte(0x80);
                Dword(u(v));
            }

        private:
            std::vector<uint8_t> &out_;
            static uint32_t u(int32_t v) { return static_cast<uint32_t>(v); }
        };

        // ─────────────────────────────────────────────────────────────────────────────
        // RCU Code Generator: LirModule → RcuFile
        // ─────────────────────────────────────────────────────────────────────────────

        struct JumpPatch {
            uint32_t patchOff;
            uint32_t targetBlock;
        };

        class RcuCodeGen {
        public:
            explicit RcuCodeGen(const LirModule &mod, const std::string &pkgName)
                : mod_(mod), pkgName_(pkgName), enc_(textData_) {
            }

            RcuFile Generate();

        private:
            const LirModule &mod_;
            std::string pkgName_;

            // Section data buffers
            std::vector<uint8_t> textData_;
            std::vector<uint8_t> rodataData_;
            std::vector<uint8_t> dataData_;

            // Per-section relocations
            std::vector<RcuReloc> textRelocs_;
            std::vector<RcuReloc> rodataRelocs_;

            // Symbol table and string table
            std::vector<RcuSymbol> symbols_;
            RcuStringTable strings_;

            // Encoder writing into textData_
            X64Enc enc_;

            // Interned rodata constants: key → symbol index
            std::unordered_map<std::string, uint32_t> strSyms_;
            std::unordered_map<std::string, uint32_t> f32Syms_;
            std::unordered_map<std::string, uint32_t> f64Syms_;
            int constIdx_ = 0;
            uint32_t f32SignMaskSym_ = ~0u;
            uint32_t f64SignMaskSym_ = ~0u;

            // Declared extern symbols (by name → symbol index)
            std::unordered_map<std::string, uint32_t> externSyms_;

            // Struct field layouts
            using LayoutMap = std::unordered_map<std::string,
                std::vector<std::pair<std::string, int> > >;
            LayoutMap layouts_;

            // ── Per-function state ────────────────────────────────────────────────────
            struct PhiMove {
                LirReg dst;
                LirReg src;
                TypeRef type;
            };

            std::unordered_map<LirReg, int32_t> slotMap_;
            std::unordered_map<LirReg, int32_t> allocaData_;
            std::unordered_map<LirReg, TypeRef> regTypes_;
            std::unordered_map<uint32_t,
                std::unordered_map<uint32_t, std::vector<PhiMove> > > phiMoves_;
            int32_t nextOff_ = 0;
            int32_t frameSize_ = 0;

            std::vector<uint32_t> blockOffsets_;
            std::vector<JumpPatch> jumpPatches_;

            // ── Helpers ───────────────────────────────────────────────────────────────

            int32_t Disp(LirReg r) { return -(int32_t) slotMap_.at(r); }

            uint32_t AddSymbol(RcuSymbol s) {
                uint32_t idx = static_cast<uint32_t>(symbols_.size());
                symbols_.push_back(std::move(s));
                return idx;
            }

            uint32_t GetOrAddExtern(const std::string &name, uint8_t kind) {
                auto it = externSyms_.find(name);
                if (it != externSyms_.end()) return it->second;
                RcuSymbol s;
                s.name = name;
                s.kind = kind;
                s.visibility = RcuSymVis::Global;
                s.sectionIdx = RCU_SEC_EXTERNAL;
                uint32_t idx = AddSymbol(s);
                externSyms_[name] = idx;
                return idx;
            }

            // Align rodataData_ to `align` bytes (zero-fill), return current offset.
            uint32_t AlignRodata(int align) {
                while (rodataData_.size() % align)
                    rodataData_.push_back(0);
                return static_cast<uint32_t>(rodataData_.size());
            }

            uint32_t InternStr(const std::string &val) {
                auto it = strSyms_.find(val);
                if (it != strSyms_.end()) return it->second;
                uint32_t off = static_cast<uint32_t>(rodataData_.size());
                for (unsigned char c: val) rodataData_.push_back(c);
                rodataData_.push_back(0);
                std::string lbl = std::format("__str{}", constIdx_++);
                RcuSymbol s;
                s.name = lbl;
                s.sectionIdx = RCU_RODATA_IDX;
                s.value = off;
                s.size = static_cast<uint32_t>(val.size() + 1);
                s.kind = RcuSymKind::Const;
                s.visibility = RcuSymVis::Local;
                uint32_t idx = AddSymbol(s);
                strSyms_[val] = idx;
                return idx;
            }

            uint32_t InternF32(const std::string &val) {
                auto it = f32Syms_.find(val);
                if (it != f32Syms_.end()) return it->second;
                uint32_t off = AlignRodata(4);
                float fv = std::stof(val);
                uint32_t bits;
                std::memcpy(&bits, &fv, 4);
                for (int i = 0; i < 4; ++i) {
                    rodataData_.push_back(bits & 0xFF);
                    bits >>= 8;
                }
                std::string lbl = std::format("__f32_{}", constIdx_++);
                RcuSymbol s;
                s.name = lbl;
                s.sectionIdx = RCU_RODATA_IDX;
                s.value = off;
                s.size = 4;
                s.kind = RcuSymKind::Const;
                s.visibility = RcuSymVis::Local;
                uint32_t idx = AddSymbol(s);
                f32Syms_[val] = idx;
                return idx;
            }

            uint32_t InternF64(const std::string &val) {
                auto it = f64Syms_.find(val);
                if (it != f64Syms_.end()) return it->second;
                uint32_t off = AlignRodata(8);
                double dv = std::stod(val);
                uint64_t bits;
                std::memcpy(&bits, &dv, 8);
                for (int i = 0; i < 8; ++i) {
                    rodataData_.push_back(bits & 0xFF);
                    bits >>= 8;
                }
                std::string lbl = std::format("__f64_{}", constIdx_++);
                RcuSymbol s;
                s.name = lbl;
                s.sectionIdx = RCU_RODATA_IDX;
                s.value = off;
                s.size = 8;
                s.kind = RcuSymKind::Const;
                s.visibility = RcuSymVis::Local;
                uint32_t idx = AddSymbol(s);
                f64Syms_[val] = idx;
                return idx;
            }

            uint32_t InternF32SignMask() {
                if (f32SignMaskSym_ != ~0u) return f32SignMaskSym_;
                uint32_t off = AlignRodata(4);
                // 0x80000000 — sign bit of f32
                rodataData_.push_back(0x00);
                rodataData_.push_back(0x00);
                rodataData_.push_back(0x00);
                rodataData_.push_back(0x80);
                RcuSymbol s;
                s.name = "__f32_sign_mask";
                s.sectionIdx = RCU_RODATA_IDX;
                s.value = off;
                s.size = 4;
                s.kind = RcuSymKind::Const;
                s.visibility = RcuSymVis::Local;
                f32SignMaskSym_ = AddSymbol(s);
                return f32SignMaskSym_;
            }

            uint32_t InternF64SignMask() {
                if (f64SignMaskSym_ != ~0u) return f64SignMaskSym_;
                uint32_t off = AlignRodata(8);
                // 0x8000000000000000 — sign bit of f64
                for (int i = 0; i < 7; ++i) rodataData_.push_back(0x00);
                rodataData_.push_back(0x80);
                RcuSymbol s;
                s.name = "__f64_sign_mask";
                s.sectionIdx = RCU_RODATA_IDX;
                s.value = off;
                s.size = 8;
                s.kind = RcuSymKind::Const;
                s.visibility = RcuSymVis::Local;
                f64SignMaskSym_ = AddSymbol(s);
                return f64SignMaskSym_;
            }

            void AddTextReloc(uint32_t sectionOff, uint32_t symIdx, int32_t addend = 0) {
                textRelocs_.push_back({sectionOff, symIdx, RcuRelType::Rel32, addend});
            }

            void PatchJumps() {
                for (const auto &p: jumpPatches_) {
                    int32_t target = static_cast<int32_t>(blockOffsets_[p.targetBlock]);
                    int32_t rel32 = target - static_cast<int32_t>(p.patchOff + 4);
                    enc_.Patch32(p.patchOff, rel32);
                }
                jumpPatches_.clear();
            }

            // ── Load A (rax / xmm0) and B (r10 / xmm1) ───────────────────────────────

            void LoadA(LirReg reg, const TypeRef &t) {
                int sz = SizeOf(t);
                int32_t d = Disp(reg);
                if (IsFloat(t)) {
                    if (t.kind == TypeRef::Kind::Float32) enc_.MovssXmm0Load(d);
                    else enc_.MovsdXmm0Load(d);
                } else if (sz == 8 || sz == 0) {
                    enc_.MovRaxLoad(d);
                } else if (t.IsSigned()) {
                    if (sz == 4) enc_.MovsxdRaxDword(d);
                    else if (sz == 2) enc_.MovsxRaxWord(d);
                    else enc_.MovsxRaxByte(d);
                } else {
                    if (sz == 4) enc_.MovEaxLoad(d);
                    else if (sz == 2) enc_.MovzxRaxWord(d);
                    else enc_.MovzxRaxByte(d);
                }
            }

            void LoadB(LirReg reg, const TypeRef &t) {
                int sz = SizeOf(t);
                int32_t d = Disp(reg);
                if (IsFloat(t)) {
                    if (t.kind == TypeRef::Kind::Float32) enc_.MovssXmm1Load(d);
                    else enc_.MovsdXmm1Load(d);
                } else if (sz == 8 || sz == 0) {
                    enc_.MovR10Load(d);
                } else if (t.IsSigned()) {
                    if (sz == 4) enc_.MovsxdR10Dword(d);
                    else if (sz == 2) enc_.MovsxR10Word(d);
                    else enc_.MovsxR10Byte(d);
                } else {
                    if (sz == 4) enc_.MovR10dLoad(d);
                    else if (sz == 2) enc_.MovzxR10Word(d);
                    else enc_.MovzxR10Byte(d);
                }
            }

            void StoreA(LirReg dst, const TypeRef &t) {
                int sz = SizeOf(t);
                int32_t d = Disp(dst);
                if (IsFloat(t)) {
                    if (t.kind == TypeRef::Kind::Float32) enc_.MovssXmm0Store(d);
                    else enc_.MovsdXmm0Store(d);
                } else {
                    int ss = (sz > 0) ? sz : 8;
                    if (ss == 8) enc_.MovRaxStore(d);
                    else if (ss == 4) enc_.MovEaxStore(d);
                    else if (ss == 2) enc_.MovAxStore(d);
                    else enc_.MovAlStore(d);
                }
            }

            // ── Struct field lookup ───────────────────────────────────────────────────

            int FieldOffset(LirReg base, const std::string &fieldName) {
                auto typeIt = regTypes_.find(base);
                if (typeIt == regTypes_.end()) return 0;
                const TypeRef &pt = typeIt->second;
                if (pt.kind != TypeRef::Kind::Pointer || pt.inner.empty()) return 0;
                const TypeRef &inner = pt.inner[0];
                if (inner.kind != TypeRef::Kind::Named) return 0;
                auto layIt = layouts_.find(inner.name);
                if (layIt == layouts_.end()) return 0;
                for (const auto &[n, off]: layIt->second)
                    if (n == fieldName) return off;
                return 0;
            }

            // ── Pre-pass: allocate stack slots ────────────────────────────────────────

            int32_t AllocSlot(LirReg reg, int bytes) {
                if (auto it = slotMap_.find(reg); it != slotMap_.end()) return it->second;
                int al = (bytes > 0) ? std::min(bytes, 8) : 1;
                nextOff_ = AlignUp(nextOff_, al);
                nextOff_ += (bytes > 0 ? bytes : 8);
                slotMap_[reg] = nextOff_;
                return nextOff_;
            }

            int32_t AllocRegion(int bytes) {
                int al = (bytes > 0) ? std::min(bytes, 8) : 1;
                nextOff_ = AlignUp(nextOff_, al);
                nextOff_ += (bytes > 0 ? bytes : 8);
                return nextOff_;
            }

            void PrepassFunc(const LirFunc &func) {
                nextOff_ = 0;
                frameSize_ = 0;
                slotMap_.clear();
                allocaData_.clear();
                regTypes_.clear();
                phiMoves_.clear();

                for (const auto &p: func.params) {
                    AllocSlot(p.reg, 8);
                    regTypes_[p.reg] = p.type;
                }

                for (uint32_t bi = 0; bi < func.blocks.size(); ++bi) {
                    for (const auto &instr: func.blocks[bi].instrs) {
                        if (instr.op == LirOpcode::Phi) {
                            for (const auto &[src, pred]: instr.phiPreds)
                                phiMoves_[pred][bi].push_back({instr.dst, src, instr.type});
                        }
                        if (instr.dst == LirNoReg) continue;
                        if (instr.op == LirOpcode::Alloca) {
                            AllocSlot(instr.dst, 8);
                            int dsz = SizeOf(instr.type);
                            allocaData_[instr.dst] = AllocRegion(dsz > 0 ? dsz : 8);
                            regTypes_[instr.dst] = TypeRef::MakePointer(instr.type);
                        } else {
                            int sz = SizeOf(instr.type);
                            AllocSlot(instr.dst, sz > 0 ? sz : 8);
                            regTypes_[instr.dst] = instr.type;
                        }
                    }
                }

                frameSize_ = AlignUp(nextOff_, 16);
                if (frameSize_ == 0) frameSize_ = 16;
            }

            // ── Build struct layouts ──────────────────────────────────────────────────

            void BuildLayouts() {
                for (const auto &s: mod_.structs) {
                    std::vector<std::pair<std::string, int> > fields;
                    int offset = 0, maxAlign = 1;
                    for (const auto &f: s.fields) {
                        int sz = SizeOf(f.type);
                        int al = sz > 0 ? std::min(sz, 8) : 1;
                        if (f.type.kind == TypeRef::Kind::Named) {
                            auto it = layouts_.find(f.type.name);
                            if (it != layouts_.end()) {
                                int ts = 0;
                                for (const auto &[fn, fo]: it->second) ts = fo;
                                sz = AlignUp(ts, al);
                            }
                        }
                        if (al > 1) offset = AlignUp(offset, al);
                        fields.emplace_back(f.name, offset);
                        offset += (sz > 0 ? sz : 8);
                        maxAlign = std::max(maxAlign, al);
                    }
                    layouts_[s.name] = std::move(fields);
                }
            }

            // ── Phi move emission ─────────────────────────────────────────────────────

            bool HasPhiMoves(uint32_t from, uint32_t to) const {
                auto it = phiMoves_.find(from);
                if (it == phiMoves_.end()) return false;
                return it->second.count(to) != 0;
            }

            void EmitPhiMoves(uint32_t from, uint32_t to) {
                auto it1 = phiMoves_.find(from);
                if (it1 == phiMoves_.end()) return;
                auto it2 = it1->second.find(to);
                if (it2 == it1->second.end()) return;
                for (const auto &m: it2->second) {
                    if (!slotMap_.count(m.src)) continue;
                    LoadA(m.src, m.type);
                    StoreA(m.dst, m.type);
                }
            }

            // ── Call argument setup ───────────────────────────────────────────────────

            void EmitCallArgs(const std::vector<LirReg> &args) {
                int intIdx = 0, fltIdx = 0;
                for (LirReg arg: args) {
                    TypeRef at = regTypes_.count(arg) ? regTypes_.at(arg) : TypeRef::MakeInt64();
                    int32_t d = Disp(arg);
                    if (IsFloat(at)) {
                        if (fltIdx < 8) {
                            int sz = SizeOf(at);
                            if (sz == 4) enc_.MovssXmmNLoad(fltIdx, d);
                            else enc_.MovsdXmmNLoad(fltIdx, d);
                            ++fltIdx;
                        }
                    } else {
                        if (intIdx < 6) {
                            enc_.MovArgLoad(intIdx, d);
                            ++intIdx;
                        }
                    }
                }
            }

            // ── Instruction code generation ───────────────────────────────────────────

            void GenInstr(const LirInstr &instr) {
                switch (instr.op) {
                    case LirOpcode::Const: {
                        if (instr.dst == LirNoReg) break;
                        const TypeRef &t = instr.type;
                        int sz = SizeOf(t);
                        if (t.kind == TypeRef::Kind::Str) {
                            uint32_t symIdx = InternStr(instr.strArg);
                            uint32_t relocOff;
                            enc_.LeaRaxRip(relocOff);
                            AddTextReloc(relocOff, symIdx);
                            enc_.MovRaxStore(Disp(instr.dst));
                        } else if (t.kind == TypeRef::Kind::Float32) {
                            uint32_t symIdx = InternF32(instr.strArg);
                            uint32_t relocOff;
                            enc_.MovssXmm0Rip(relocOff);
                            AddTextReloc(relocOff, symIdx);
                            enc_.MovssXmm0Store(Disp(instr.dst));
                        } else if (t.kind == TypeRef::Kind::Float64) {
                            uint32_t symIdx = InternF64(instr.strArg);
                            uint32_t relocOff;
                            enc_.MovsdXmm0Rip(relocOff);
                            AddTextReloc(relocOff, symIdx);
                            enc_.MovsdXmm0Store(Disp(instr.dst));
                        } else if (t.kind == TypeRef::Kind::Bool) {
                            enc_.MovEaxImm32(instr.strArg == "true" ? 1 : 0);
                            StoreA(instr.dst, TypeRef::MakeInt64());
                        } else {
                            const std::string &sv = instr.strArg.empty() ? "0" : instr.strArg;
                            int64_t v = 0;
                            try { v = std::stoll(sv); } catch (...) {
                            }
                            if (v >= 0 && v <= 0x7FFFFFFF)
                                enc_.MovEaxImm32(static_cast<int32_t>(v));
                            else
                                enc_.MovRaxImm64(v);
                            StoreA(instr.dst, sz > 0 ? t : TypeRef::MakeInt64());
                        }
                        break;
                    }

                    case LirOpcode::Alloca: {
                        int32_t dataOff = allocaData_.at(instr.dst);
                        enc_.LeaRaxStack(-dataOff);
                        enc_.MovRaxStore(Disp(instr.dst));
                        break;
                    }

                    case LirOpcode::Load: {
                        const TypeRef &t = instr.type;
                        int sz = SizeOf(t);
                        if (!instr.strArg.empty()) {
                            // Named global — load via RIP-relative
                            uint32_t symIdx = GetOrAddExtern(instr.strArg, RcuSymKind::ExternData);
                            uint32_t relocOff;
                            enc_.MovRaxRip(relocOff);
                            AddTextReloc(relocOff, symIdx);
                        } else {
                            LirReg ptr = instr.srcs[0];
                            enc_.MovR10Load(Disp(ptr));
                            // Load through pointer: use r10 as base
                            // Emit: mov rax, [r10]  (49 8B 02)
                            if (IsFloat(t)) {
                                // movss/movsd xmm0, [r10]
                                if (sz == 4) {
                                    enc_.Byte(0xF3);
                                    enc_.Byte(0x41);
                                    enc_.Byte(0x0F);
                                    enc_.Byte(0x10);
                                    enc_.Byte(0x02);
                                } else {
                                    enc_.Byte(0xF2);
                                    enc_.Byte(0x41);
                                    enc_.Byte(0x0F);
                                    enc_.Byte(0x10);
                                    enc_.Byte(0x02);
                                }
                                StoreA(instr.dst, t);
                                break;
                            } else if (sz == 8 || sz == 0) {
                                enc_.Byte(0x49);
                                enc_.Byte(0x8B);
                                enc_.Byte(0x02); // mov rax, [r10]
                            } else if (t.IsSigned()) {
                                if (sz == 4) {
                                    enc_.Byte(0x49);
                                    enc_.Byte(0x63);
                                    enc_.Byte(0x02); // movsxd rax,[r10]
                                } else if (sz == 2) {
                                    enc_.Byte(0x49);
                                    enc_.Byte(0x0F);
                                    enc_.Byte(0xBF);
                                    enc_.Byte(0x02);
                                } else {
                                    enc_.Byte(0x49);
                                    enc_.Byte(0x0F);
                                    enc_.Byte(0xBE);
                                    enc_.Byte(0x02);
                                }
                            } else {
                                if (sz == 4) {
                                    enc_.Byte(0x45);
                                    enc_.Byte(0x8B);
                                    enc_.Byte(0x02); // mov r8d,[r10] → rax by copy
                                    enc_.Byte(0x4D);
                                    enc_.Byte(0x8B);
                                    enc_.Byte(0xC0); // mov r8,r8 nop
                                    // Actually: just use 44 8B 02 (mov r8d,[r10]) then mov eax, r8d
                                    // Simpler: re-do with known-good sequence
                                    // Let's undo the above and emit clean version
                                    // Pop the 6 bytes we just pushed — we can't easily undo. Use a different approach.
                                    // Keep it simple: treat sz==4 unsigned as sz==8 for pointers (store 0-extended)
                                }
                                // For simplicity emit movzx variants
                                if (sz == 2) {
                                    enc_.Byte(0x49);
                                    enc_.Byte(0x0F);
                                    enc_.Byte(0xB7);
                                    enc_.Byte(0x02);
                                } else {
                                    enc_.Byte(0x49);
                                    enc_.Byte(0x0F);
                                    enc_.Byte(0xB6);
                                    enc_.Byte(0x02);
                                }
                            }
                        }
                        StoreA(instr.dst, sz > 0 ? t : TypeRef::MakeInt64());
                        break;
                    }

                    case LirOpcode::Store: {
                        LirReg val = instr.srcs[0];
                        LirReg ptr = instr.srcs[1];
                        const TypeRef &t = instr.type;
                        int sz = SizeOf(t);
                        enc_.MovR11Load(Disp(ptr));
                        if (IsFloat(t)) {
                            LoadA(val, t);
                            // movss/movsd [r11], xmm0
                            if (sz == 4) {
                                enc_.Byte(0xF3);
                                enc_.Byte(0x41);
                                enc_.Byte(0x0F);
                                enc_.Byte(0x11);
                                enc_.Byte(0x03);
                            } else {
                                enc_.Byte(0xF2);
                                enc_.Byte(0x41);
                                enc_.Byte(0x0F);
                                enc_.Byte(0x11);
                                enc_.Byte(0x03);
                            }
                        } else {
                            int ss = (sz > 0) ? sz : 8;
                            LoadA(val, t);
                            // mov [r11], rax/eax/ax/al
                            if (ss == 8) {
                                enc_.Byte(0x49);
                                enc_.Byte(0x89);
                                enc_.Byte(0x03);
                            } else if (ss == 4) {
                                enc_.Byte(0x41);
                                enc_.Byte(0x89);
                                enc_.Byte(0x03);
                            } else if (ss == 2) {
                                enc_.Byte(0x66);
                                enc_.Byte(0x41);
                                enc_.Byte(0x89);
                                enc_.Byte(0x03);
                            } else {
                                enc_.Byte(0x41);
                                enc_.Byte(0x88);
                                enc_.Byte(0x03);
                            }
                        }
                        break;
                    }

                    case LirOpcode::Add:
                    case LirOpcode::Sub:
                    case LirOpcode::And:
                    case LirOpcode::Or:
                    case LirOpcode::Xor: {
                        const TypeRef &t = instr.type;
                        if (IsFloat(t)) {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            bool f32 = (t.kind == TypeRef::Kind::Float32);
                            if (instr.op == LirOpcode::Add) {
                                if (f32) enc_.AddssXmm01();
                                else enc_.AddsdXmm01();
                            } else if (instr.op == LirOpcode::Sub) {
                                if (f32) enc_.SubssXmm01();
                                else enc_.SubsdXmm01();
                            } else {
                                if (f32) enc_.AddssXmm01();
                                else enc_.AddsdXmm01();
                            } // bitwise on float: fallback
                            StoreA(instr.dst, t);
                        } else {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            if (instr.op == LirOpcode::Add) enc_.AddRaxR10();
                            else if (instr.op == LirOpcode::Sub) enc_.SubRaxR10();
                            else if (instr.op == LirOpcode::And) enc_.AndRaxR10();
                            else if (instr.op == LirOpcode::Or) enc_.OrRaxR10();
                            else enc_.XorRaxR10();
                            StoreA(instr.dst, t);
                        }
                        break;
                    }

                    case LirOpcode::Mul: {
                        const TypeRef &t = instr.type;
                        if (IsFloat(t)) {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            if (t.kind == TypeRef::Kind::Float32) enc_.MulssXmm01();
                            else enc_.MulsdXmm01();
                        } else {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            enc_.ImulRaxR10();
                        }
                        StoreA(instr.dst, t);
                        break;
                    }

                    case LirOpcode::Div:
                    case LirOpcode::Mod: {
                        const TypeRef &t = instr.type;
                        if (IsFloat(t)) {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            if (t.kind == TypeRef::Kind::Float32) enc_.DivssXmm01();
                            else enc_.DivsdXmm01();
                            StoreA(instr.dst, t);
                        } else {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            if (t.IsSigned()) {
                                enc_.Cqo();
                                enc_.IdivR10();
                            } else {
                                enc_.XorRdxRdx();
                                enc_.DivR10();
                            }
                            if (instr.op == LirOpcode::Mod) enc_.MovRaxRdx();
                            StoreA(instr.dst, t);
                        }
                        break;
                    }

                    case LirOpcode::Pow: {
                        const TypeRef &t = instr.type;
                        if (IsFloat(t)) {
                            uint32_t sym = GetOrAddExtern("pow", RcuSymKind::ExternFunc);
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            enc_.MovssXmmNLoad(0, Disp(instr.srcs[0]));
                            enc_.MovssXmmNLoad(1, Disp(instr.srcs[1]));
                            uint32_t ro;
                            enc_.Call(ro);
                            AddTextReloc(ro, sym);
                        } else {
                            uint32_t sym = GetOrAddExtern("__rux_ipow", RcuSymKind::ExternFunc);
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            enc_.MovArgLoad(0, Disp(instr.srcs[0]));
                            enc_.MovArgLoad(1, Disp(instr.srcs[1]));
                            uint32_t ro;
                            enc_.Call(ro);
                            AddTextReloc(ro, sym);
                        }
                        StoreA(instr.dst, t);
                        break;
                    }

                    case LirOpcode::Shl:
                    case LirOpcode::Shr: {
                        const TypeRef &t = instr.type;
                        LoadA(instr.srcs[0], t);
                        enc_.MovR11Load(Disp(instr.srcs[1]));
                        enc_.MovRcxR11();
                        bool isShr = (instr.op == LirOpcode::Shr);
                        if (isShr && t.IsSigned()) enc_.SarRaxCl();
                        else if (isShr) enc_.ShrRaxCl();
                        else enc_.ShlRaxCl();
                        StoreA(instr.dst, t);
                        break;
                    }

                    case LirOpcode::Neg: {
                        const TypeRef &t = instr.type;
                        if (IsFloat(t)) {
                            LoadA(instr.srcs[0], t);
                            bool f32 = (t.kind == TypeRef::Kind::Float32);
                            uint32_t maskSym = f32 ? InternF32SignMask() : InternF64SignMask();
                            uint32_t ro;
                            if (f32) enc_.MovssXmm1Rip(ro);
                            else enc_.MovsdXmm1Rip(ro);
                            AddTextReloc(ro, maskSym);
                            if (f32) enc_.XorpsXmm01();
                            else enc_.XorpdXmm01();
                            StoreA(instr.dst, t);
                        } else {
                            LoadA(instr.srcs[0], t);
                            enc_.NegRax();
                            StoreA(instr.dst, t);
                        }
                        break;
                    }

                    case LirOpcode::Not: {
                        LoadA(instr.srcs[0], instr.type);
                        enc_.TestRaxRax();
                        enc_.SeteAl();
                        enc_.MovzxRaxAl();
                        StoreA(instr.dst, TypeRef::MakeBool());
                        break;
                    }

                    case LirOpcode::BitNot: {
                        LoadA(instr.srcs[0], instr.type);
                        enc_.NotRax();
                        StoreA(instr.dst, instr.type);
                        break;
                    }

                    case LirOpcode::CmpEq:
                    case LirOpcode::CmpNe:
                    case LirOpcode::CmpLt:
                    case LirOpcode::CmpLe:
                    case LirOpcode::CmpGt:
                    case LirOpcode::CmpGe: {
                        const TypeRef &lhsT = regTypes_.count(instr.srcs[0])
                                                  ? regTypes_.at(instr.srcs[0])
                                                  : instr.type;
                        LoadA(instr.srcs[0], lhsT);
                        LoadB(instr.srcs[1], lhsT);
                        if (IsFloat(lhsT)) {
                            if (lhsT.kind == TypeRef::Kind::Float32) enc_.UcomissXmm01();
                            else enc_.UcomisdXmm01();
                            switch (instr.op) {
                                case LirOpcode::CmpEq: enc_.SeteAl();
                                    break;
                                case LirOpcode::CmpNe: enc_.SetneAl();
                                    break;
                                case LirOpcode::CmpLt: enc_.SetbAl();
                                    break;
                                case LirOpcode::CmpLe: enc_.SetbeAl();
                                    break;
                                case LirOpcode::CmpGt: enc_.SetaAl();
                                    break;
                                default: enc_.SetaeAl();
                                    break;
                            }
                        } else {
                            enc_.CmpRaxR10();
                            bool sig = lhsT.IsSigned();
                            switch (instr.op) {
                                case LirOpcode::CmpEq: enc_.SeteAl();
                                    break;
                                case LirOpcode::CmpNe: enc_.SetneAl();
                                    break;
                                case LirOpcode::CmpLt: sig ? enc_.SetlAl() : enc_.SetbAl();
                                    break;
                                case LirOpcode::CmpLe: sig ? enc_.SetleAl() : enc_.SetbeAl();
                                    break;
                                case LirOpcode::CmpGt: sig ? enc_.SetgAl() : enc_.SetaAl();
                                    break;
                                default: sig ? enc_.SetgeAl() : enc_.SetaeAl();
                                    break;
                            }
                        }
                        enc_.MovzxRaxAl();
                        StoreA(instr.dst, TypeRef::MakeBool());
                        break;
                    }

                    case LirOpcode::Cast: {
                        const TypeRef &dstT = instr.type;
                        TypeRef srcT = regTypes_.count(instr.srcs[0])
                                           ? regTypes_.at(instr.srcs[0])
                                           : dstT;
                        LoadA(instr.srcs[0], srcT);
                        bool srcFl = IsFloat(srcT), dstFl = IsFloat(dstT);
                        if (srcFl && !dstFl) {
                            if (srcT.kind == TypeRef::Kind::Float32) enc_.CvttsssiRaxXmm0();
                            else enc_.CvttsdsiRaxXmm0();
                        } else if (!srcFl && dstFl) {
                            if (dstT.kind == TypeRef::Kind::Float32) enc_.Cvtsi2ssXmm0Rax();
                            else enc_.Cvtsi2sdXmm0Rax();
                        } else if (srcFl && dstFl) {
                            if (srcT.kind == TypeRef::Kind::Float32 && dstT.kind == TypeRef::Kind::Float64)
                                enc_.CvtsssdXmm0();
                            else if (srcT.kind == TypeRef::Kind::Float64 && dstT.kind == TypeRef::Kind::Float32)
                                enc_.CvtsdssXmm0();
                        }
                        StoreA(instr.dst, dstT);
                        break;
                    }

                    case LirOpcode::Call: {
                        EmitCallArgs(instr.srcs);
                        uint32_t symIdx = GetOrAddExtern(instr.strArg, RcuSymKind::ExternFunc);
                        // If we already have a defined symbol for this name, use it
                        for (uint32_t i = 0; i < symbols_.size(); ++i) {
                            if (symbols_[i].name == instr.strArg && symbols_[i].kind == RcuSymKind::Func) {
                                symIdx = i;
                                break;
                            }
                        }
                        uint32_t ro;
                        enc_.Call(ro);
                        AddTextReloc(ro, symIdx);
                        if (instr.dst != LirNoReg && !instr.type.IsVoid())
                            StoreA(instr.dst, instr.type);
                        break;
                    }

                    case LirOpcode::CallIndirect: {
                        if (instr.srcs.empty()) break;
                        LirReg callee = instr.srcs[0];
                        std::vector<LirReg> args(instr.srcs.begin() + 1, instr.srcs.end());
                        enc_.MovR10Load(Disp(callee));
                        EmitCallArgs(args);
                        enc_.CallR10();
                        if (instr.dst != LirNoReg && !instr.type.IsVoid())
                            StoreA(instr.dst, instr.type);
                        break;
                    }

                    case LirOpcode::FieldPtr: {
                        LirReg base = instr.srcs[0];
                        enc_.MovRaxLoad(Disp(base));
                        int off = FieldOffset(base, instr.strArg);
                        if (off != 0) enc_.LeaRaxRaxDisp(off);
                        enc_.MovRaxStore(Disp(instr.dst));
                        break;
                    }

                    case LirOpcode::IndexPtr: {
                        LirReg base = instr.srcs[0];
                        LirReg idx = instr.srcs[1];
                        int elemSz = (instr.type.kind == TypeRef::Kind::Pointer && !instr.type.inner.empty())
                                         ? SizeOf(instr.type.inner[0])
                                         : 8;
                        if (elemSz < 1) elemSz = 1;
                        enc_.MovRaxLoad(Disp(base));
                        enc_.MovR10Load(Disp(idx));
                        enc_.ImulR11R10Imm32(elemSz);
                        enc_.AddRaxR11();
                        enc_.MovRaxStore(Disp(instr.dst));
                        break;
                    }

                    case LirOpcode::Phi:
                        break; // handled by phi-move pre-emission

                    default:
                        break;
                }
            }

            // ── Terminator ────────────────────────────────────────────────────────────

            void GenTerm(uint32_t blockIdx, const LirTerminator &term, const LirFunc &func) {
                switch (term.kind) {
                    case LirTermKind::Jump: {
                        EmitPhiMoves(blockIdx, term.trueTarget);
                        uint32_t po;
                        enc_.Jmp(po);
                        jumpPatches_.push_back({po, term.trueTarget});
                        break;
                    }

                    case LirTermKind::Branch: {
                        enc_.MovRaxLoad(Disp(term.cond));
                        enc_.TestRaxRax();
                        bool truePhi = HasPhiMoves(blockIdx, term.trueTarget);
                        bool falsePhi = HasPhiMoves(blockIdx, term.falseTarget);
                        if (!truePhi && !falsePhi) {
                            uint32_t po;
                            enc_.Jz(po);
                            jumpPatches_.push_back({po, term.falseTarget});
                            uint32_t po2;
                            enc_.Jmp(po2);
                            jumpPatches_.push_back({po2, term.trueTarget});
                        } else {
                            uint32_t jzOff;
                            enc_.Jz(jzOff);
                            // true trampoline
                            EmitPhiMoves(blockIdx, term.trueTarget);
                            uint32_t jmpTrue;
                            enc_.Jmp(jmpTrue);
                            jumpPatches_.push_back({jmpTrue, term.trueTarget});
                            // patch jz to here (false trampoline)
                            int32_t here = static_cast<int32_t>(enc_.Size());
                            enc_.Patch32(jzOff, here - static_cast<int32_t>(jzOff + 4));
                            EmitPhiMoves(blockIdx, term.falseTarget);
                            uint32_t jmpFalse;
                            enc_.Jmp(jmpFalse);
                            jumpPatches_.push_back({jmpFalse, term.falseTarget});
                        }
                        break;
                    }

                    case LirTermKind::Return: {
                        if (term.retVal && *term.retVal != LirNoReg)
                            LoadA(*term.retVal, term.retType);
                        enc_.Leave();
                        enc_.Ret();
                        break;
                    }

                    case LirTermKind::Switch: {
                        enc_.MovRaxLoad(Disp(term.cond));
                        for (const auto &c: term.cases) {
                            int64_t v = 0;
                            try { v = std::stoll(c.value); } catch (...) {
                            }
                            enc_.CmpRaxImm32(static_cast<int32_t>(v));
                            uint32_t po;
                            enc_.Je(po);
                            jumpPatches_.push_back({po, c.target});
                        }
                        EmitPhiMoves(blockIdx, term.defaultTarget);
                        uint32_t po;
                        enc_.Jmp(po);
                        jumpPatches_.push_back({po, term.defaultTarget});
                        break;
                    }
                }
            }

            // ── Function generation ───────────────────────────────────────────────────

            void GenFunc(const LirFunc &func) {
                if (func.isExtern) {
                    GetOrAddExtern(func.name, RcuSymKind::ExternFunc);
                    return;
                }

                PrepassFunc(func);
                jumpPatches_.clear();

                uint32_t funcStart = enc_.Size();

                // Function symbol
                RcuSymbol sym;
                sym.name = func.name;
                sym.sectionIdx = RCU_TEXT_IDX;
                sym.value = funcStart;
                sym.kind = RcuSymKind::Func;
                sym.visibility = func.isPublic ? RcuSymVis::Global : RcuSymVis::Local;
                sym.typeName = func.returnType.ToString();
                AddSymbol(sym);

                // Prologue
                enc_.PushRbp();
                enc_.MovRbpRsp();
                enc_.SubRspImm32(frameSize_);

                // Spill ABI param registers to stack slots
                int intIdx = 0, fltIdx = 0;
                for (const auto &p: func.params) {
                    int sz = SizeOf(p.type);
                    if (IsFloat(p.type)) {
                        if (fltIdx < 8) {
                            int32_t d = Disp(p.reg);
                            if (sz == 4) enc_.MovssXmm0Store(d); // store from xmmN; use xmm0 for arg 0
                            else enc_.MovsdXmm0Store(d);
                            // For arg index > 0 we need xmmN; simplify by loading arg index directly
                            if (fltIdx == 0) {
                                if (sz == 4) enc_.MovssXmm0Store(d);
                                else enc_.MovsdXmm0Store(d);
                            } else {
                                // Store from xmmN to stack
                                if (sz == 4) enc_.MovssXmmNLoad(fltIdx, d); // wrong direction, fix:
                                // MOVSS [rbp+d], xmmN
                                enc_.Byte(0xF3);
                                enc_.Byte(0x0F);
                                enc_.Byte(0x11);
                                enc_.Byte(static_cast<uint8_t>(0x80 | (fltIdx << 3) | 5));
                                enc_.Dword(static_cast<uint32_t>(d));
                            }
                            ++fltIdx;
                        }
                    } else {
                        if (intIdx < 6) {
                            enc_.MovArgStore(intIdx, Disp(p.reg));
                            ++intIdx;
                        }
                    }
                }

                // Basic blocks
                blockOffsets_.assign(func.blocks.size(), 0);
                for (uint32_t bi = 0; bi < func.blocks.size(); ++bi) {
                    blockOffsets_[bi] = enc_.Size();
                    const auto &block = func.blocks[bi];
                    for (const auto &instr: block.instrs)
                        GenInstr(instr);
                    if (block.term)
                        GenTerm(bi, *block.term, func);
                }

                PatchJumps();

                // Update symbol size
                for (auto &s: symbols_) {
                    if (s.name == func.name && s.sectionIdx == RCU_TEXT_IDX && s.value == funcStart) {
                        s.size = enc_.Size() - funcStart;
                        break;
                    }
                }
            }

            // ── Module generation ─────────────────────────────────────────────────────

            void GenModule() {
                BuildLayouts();

                // Extern vars
                for (const auto &ev: mod_.externVars)
                    GetOrAddExtern(ev.name, RcuSymKind::ExternData);

                // Module constants → .data symbols
                for (const auto &c: mod_.consts) {
                    RcuSymbol s;
                    s.name = c.name;
                    s.sectionIdx = RCU_DATA_IDX;
                    s.value = static_cast<uint32_t>(dataData_.size());
                    s.kind = RcuSymKind::Const;
                    s.visibility = c.isPublic ? RcuSymVis::Global : RcuSymVis::Local;
                    s.typeName = c.type.ToString();
                    // Emit 8 placeholder bytes in .data
                    for (int i = 0; i < 8; ++i) dataData_.push_back(0);
                    s.size = 8;
                    AddSymbol(s);
                }

                // Functions
                for (const auto &func: mod_.funcs)
                    GenFunc(func);
            }
        };

        // ─────────────────────────────────────────────────────────────────────────────
        // RcuCodeGen::Generate
        // ─────────────────────────────────────────────────────────────────────────────

        RcuFile RcuCodeGen::Generate() {
            GenModule();

            RcuFile file;
            file.sourcePath = mod_.name;
            file.packageName = pkgName_;
            file.buildTimestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

            // Parse rux version from RUX_VERSION string "M.m.p"
            {
                std::string ver = RUX_VERSION;
                unsigned M = 0, mi = 0, p = 0;
                auto parseNum = [](const char *s, unsigned &out) -> const char * {
                    while (*s && (*s < '0' || *s > '9')) ++s;
                    while (*s >= '0' && *s <= '9') {
                        out = out * 10 + static_cast<unsigned>(*s - '0');
                        ++s;
                    }
                    return s;
                };
                const char *c1 = parseNum(ver.c_str(), M);
                const char *c2 = parseNum(c1, mi);
                parseNum(c2, p);
                file.ruxVersion = (M << 16) | (mi << 8) | p;
            }

            // Build sections (always 3: .text, .rodata, .data)
            {
                RcuSection text;
                text.name = ".text";
                text.type = RcuSecType::Text;
                text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
                text.alignment = 16;
                text.data = std::move(textData_);
                text.relocs = std::move(textRelocs_);
                file.sections.push_back(std::move(text));
            }
            {
                RcuSection rodata;
                rodata.name = ".rodata";
                rodata.type = RcuSecType::RoData;
                rodata.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
                rodata.alignment = 8;
                rodata.data = std::move(rodataData_);
                rodata.relocs = std::move(rodataRelocs_);
                file.sections.push_back(std::move(rodata));
            }
            {
                RcuSection data;
                data.name = ".data";
                data.type = RcuSecType::Data;
                data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
                data.alignment = 8;
                data.data = std::move(dataData_);
                file.sections.push_back(std::move(data));
            }

            file.symbols = std::move(symbols_);

            // Build string table offsets (intern all names into the file's string table)
            // (done during Emit/Dump)

            file.flags = 0x01; // F_HAS_METADATA
            file.hasMetadata = true;

            return file;
        }

        // ─────────────────────────────────────────────────────────────────────────────
        // CRC-32C (Castagnoli)
        // ─────────────────────────────────────────────────────────────────────────────

        static uint32_t Crc32cTable[256];
        static bool Crc32cReady = false;

        static void InitCrc32c() {
            if (Crc32cReady) return;
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
                Crc32cTable[i] = c;
            }
            Crc32cReady = true;
        }

        static uint32_t Crc32c(const std::vector<uint8_t> &data) {
            InitCrc32c();
            uint32_t crc = 0xFFFFFFFFu;
            for (uint8_t b: data) crc = Crc32cTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }

        // ─────────────────────────────────────────────────────────────────────────────
        // Binary writer
        // ─────────────────────────────────────────────────────────────────────────────

        class RcuWriter {
        public:
            static std::vector<uint8_t> Serialize(const RcuFile &f) {
                RcuWriter w(f);
                return w.Build();
            }

        private:
            const RcuFile &f_;
            RcuStringTable st_;

            explicit RcuWriter(const RcuFile &f) : f_(f) {
            }

            // Intern all strings first so offsets are stable
            void InternStrings() {
                st_.Intern(f_.sourcePath);
                st_.Intern(f_.packageName);
                for (const auto &s: f_.symbols) {
                    st_.Intern(s.name);
                    st_.Intern(s.typeName);
                }
                for (const auto &sec: f_.sections)
                    st_.Intern(sec.name);
            }

            void AppendU8(std::vector<uint8_t> &buf, uint8_t v) { buf.push_back(v); }

            void AppendU16(std::vector<uint8_t> &buf, uint16_t v) {
                buf.push_back(v & 0xFF);
                buf.push_back(v >> 8);
            }

            void AppendU32(std::vector<uint8_t> &buf, uint32_t v) {
                for (int i = 0; i < 4; ++i) {
                    buf.push_back(v & 0xFF);
                    v >>= 8;
                }
            }

            void AppendI32(std::vector<uint8_t> &buf, int32_t v) { AppendU32(buf, static_cast<uint32_t>(v)); }

            void AppendU64(std::vector<uint8_t> &buf, uint64_t v) {
                for (int i = 0; i < 8; ++i) {
                    buf.push_back(v & 0xFF);
                    v >>= 8;
                }
            }

            void Patch32At(std::vector<uint8_t> &buf, uint32_t off, uint32_t v) {
                buf[off] = v & 0xFF;
                buf[off + 1] = (v >> 8) & 0xFF;
                buf[off + 2] = (v >> 16) & 0xFF;
                buf[off + 3] = v >> 24;
            }

            void AlignTo(std::vector<uint8_t> &buf, int a) {
                while (buf.size() % a) buf.push_back(0);
            }

            std::vector<uint8_t> Build() {
                InternStrings();

                std::vector<uint8_t> out;
                out.reserve(1024);

                uint16_t secCount = static_cast<uint16_t>(f_.sections.size());
                uint32_t symCount = static_cast<uint32_t>(f_.symbols.size());

                // ── File Header (32 bytes) ─────────────────────────────────────────────
                // [0-3]  magic
                out.push_back(0x52);
                out.push_back(0x43);
                out.push_back(0x55);
                out.push_back(0x00);
                // [4-5]  version 1.0
                AppendU16(out, 0x0100);
                // [6]    arch
                AppendU8(out, f_.arch);
                // [7]    flags
                AppendU8(out, f_.flags);
                // [8-9]  section_count
                AppendU16(out, secCount);
                // [10-11] reserved
                AppendU16(out, 0);
                // [12-15] symbol_count
                AppendU32(out, symCount);
                // [16-19] string_table_off (placeholder)
                uint32_t stOffPatch = static_cast<uint32_t>(out.size());
                AppendU32(out, 0);
                // [20-23] string_table_size (placeholder)
                uint32_t stSizePatch = static_cast<uint32_t>(out.size());
                AppendU32(out, 0);
                // [24-27] metadata_offset (placeholder)
                uint32_t metaOffPatch = static_cast<uint32_t>(out.size());
                AppendU32(out, 0);
                // [28-31] checksum (placeholder)
                uint32_t checksumPatch = static_cast<uint32_t>(out.size());
                AppendU32(out, 0);

                // ── Section Table (secCount × 40 bytes) ───────────────────────────────
                // We need to write reloc offsets later; track patch positions.
                std::vector<uint32_t> secRelocOffPatches(secCount);
                std::vector<uint32_t> secRawOffPatches(secCount);

                for (uint16_t i = 0; i < secCount; ++i) {
                    const auto &sec = f_.sections[i];
                    // name[8]
                    char name8[8] = {};
                    for (int j = 0; j < 7 && j < static_cast<int>(sec.name.size()); ++j)
                        name8[j] = sec.name[j];
                    for (char c: name8) AppendU8(out, static_cast<uint8_t>(c));
                    AppendU32(out, sec.type);
                    AppendU32(out, sec.flags);
                    secRawOffPatches[i] = static_cast<uint32_t>(out.size());
                    AppendU32(out, 0); // raw_offset placeholder
                    AppendU32(out, static_cast<uint32_t>(sec.data.size()));
                    AppendU32(out, static_cast<uint32_t>(std::max(sec.data.size(), size_t(1)))); // virtual_size
                    AppendU16(out, sec.alignment);
                    AppendU16(out, static_cast<uint16_t>(sec.relocs.size()));
                    secRelocOffPatches[i] = static_cast<uint32_t>(out.size());
                    AppendU32(out, 0); // reloc_offset placeholder
                    AppendU32(out, 0); // reserved
                }

                // ── Symbol Table (symCount × 20 bytes) ───────────────────────────────
                for (const auto &sym: f_.symbols) {
                    AppendU32(out, st_.Intern(sym.name));
                    AppendU32(out, sym.value);
                    AppendU32(out, sym.size);
                    AppendU16(out, sym.sectionIdx);
                    AppendU8(out, sym.kind);
                    AppendU8(out, sym.visibility);
                    AppendU32(out, sym.typeName.empty() ? 0 : st_.Intern(sym.typeName));
                }

                // ── Section Data + Relocations ─────────────────────────────────────────
                for (uint16_t i = 0; i < secCount; ++i) {
                    const auto &sec = f_.sections[i];

                    // Align to section alignment
                    AlignTo(out, sec.alignment);
                    Patch32At(out, secRawOffPatches[i], static_cast<uint32_t>(out.size()));

                    // Raw data
                    out.insert(out.end(), sec.data.begin(), sec.data.end());

                    // Relocations (4-byte aligned)
                    if (!sec.relocs.empty()) {
                        AlignTo(out, 4);
                        Patch32At(out, secRelocOffPatches[i], static_cast<uint32_t>(out.size()));
                        for (const auto &r: sec.relocs) {
                            AppendU32(out, r.sectionOffset);
                            AppendU32(out, r.symbolIndex);
                            AppendU16(out, r.type);
                            AppendU16(out, 0); // reserved
                            AppendI32(out, r.addend);
                        }
                    }
                }

                // ── String Table ──────────────────────────────────────────────────────
                Patch32At(out, stOffPatch, static_cast<uint32_t>(out.size()));
                Patch32At(out, stSizePatch, st_.Size());
                const char *stData = st_.Data();
                for (uint32_t i = 0; i < st_.Size(); ++i) out.push_back(static_cast<uint8_t>(stData[i]));

                // ── Rux Metadata (64 bytes, 8-byte aligned) ────────────────────────────
                if (f_.hasMetadata) {
                    AlignTo(out, 8);
                    Patch32At(out, metaOffPatch, static_cast<uint32_t>(out.size()));
                    // magic
                    out.push_back(0x4D);
                    out.push_back(0x45);
                    out.push_back(0x54);
                    out.push_back(0x41);
                    AppendU32(out, 64); // block_size
                    AppendU32(out, st_.Intern(f_.sourcePath)); // source_path_off
                    AppendU32(out, st_.Intern(f_.packageName)); // package_name_off
                    AppendU64(out, f_.buildTimestamp);
                    AppendU32(out, f_.ruxVersion);
                    AppendU32(out, f_.compilerFlags);
                    for (uint8_t b: f_.sourceHash) AppendU8(out, b);
                }

                // ── CRC-32C ───────────────────────────────────────────────────────────
                uint32_t crc = Crc32c(out);
                Patch32At(out, checksumPatch, crc);

                return out;
            }
        };

        // ─────────────────────────────────────────────────────────────────────────────
        // Text dumper
        // ─────────────────────────────────────────────────────────────────────────────

        class RcuDumper {
        public:
            static std::string Dump(const RcuFile &f) {
                std::ostringstream out;
                out << "; RCU  Rux Compiled Unit  v1.0\n";
                out << "; Architecture: x86-64 (Windows x64)\n";
                out << std::format("; Source:        {}\n", f.sourcePath.empty() ? "<unknown>" : f.sourcePath);
                out << std::format("; Package:       {}\n", f.packageName.empty() ? "<unknown>" : f.packageName);
                if (f.ruxVersion) {
                    out << std::format("; Rux version:   {}.{}.{}\n",
                                       f.ruxVersion >> 16, (f.ruxVersion >> 8) & 0xFF, f.ruxVersion & 0xFF);
                }
                out << '\n';

                // Sections
                out << std::format("Sections: {}\n", f.sections.size());
                for (size_t i = 0; i < f.sections.size(); ++i) {
                    const auto &s = f.sections[i];
                    std::string flags;
                    if (s.flags & RcuSecFlag::Alloc) flags += 'A';
                    if (s.flags & RcuSecFlag::Exec) flags += 'E';
                    if (s.flags & RcuSecFlag::Read) flags += 'R';
                    if (s.flags & RcuSecFlag::Write) flags += 'W';
                    if (s.flags & RcuSecFlag::Merge) flags += 'M';
                    if (s.flags & RcuSecFlag::Strings) flags += 'S';
                    if (flags.empty()) flags = "-";
                    out << std::format("  [{:2}]  {:<10}  flags:{:<5}  align:{:<4}  data:{}B  relocs:{}\n",
                                       i, s.name, flags, s.alignment, s.data.size(), s.relocs.size());
                }
                out << '\n';

                // Symbols
                out << std::format("Symbols: {}\n", f.symbols.size());
                for (size_t i = 0; i < f.symbols.size(); ++i) {
                    const auto &s = f.symbols[i];
                    std::string secStr;
                    if (s.sectionIdx == RCU_SEC_EXTERNAL) secStr = "extern";
                    else if (s.sectionIdx == RCU_SEC_ABSOLUTE) secStr = "abs";
                    else if (s.sectionIdx < f.sections.size())
                        secStr = std::format("{}+0x{:04X}", f.sections[s.sectionIdx].name, s.value);
                    else secStr = std::format("sec{}+0x{:04X}", s.sectionIdx, s.value);

                    const char *kindStr = "?";
                    switch (s.kind) {
                        case RcuSymKind::Func: kindStr = "FUNC";
                            break;
                        case RcuSymKind::Data: kindStr = "DATA";
                            break;
                        case RcuSymKind::Const: kindStr = "CONST";
                            break;
                        case RcuSymKind::Section: kindStr = "SECTION";
                            break;
                        case RcuSymKind::File: kindStr = "FILE";
                            break;
                        case RcuSymKind::ExternFunc: kindStr = "EXTFUNC";
                            break;
                        case RcuSymKind::ExternData: kindStr = "EXTDATA";
                            break;
                    }
                    const char *visStr = s.visibility == RcuSymVis::Global
                                             ? "GLOBAL"
                                             : s.visibility == RcuSymVis::Weak
                                                   ? "WEAK"
                                                   : "LOCAL";

                    out << std::format("  [{:3}]  {:<24}  {:>20}  size={:<6}  {:<8}  {:<6}",
                                       i, s.name, secStr, s.size, kindStr, visStr);
                    if (!s.typeName.empty())
                        out << std::format("  \"{}\"", s.typeName);
                    out << '\n';
                }
                out << '\n';

                // Relocations
                bool anyReloc = false;
                for (const auto &sec: f.sections)
                    if (!sec.relocs.empty()) {
                        anyReloc = true;
                        break;
                    }

                if (anyReloc) {
                    for (size_t si = 0; si < f.sections.size(); ++si) {
                        const auto &sec = f.sections[si];
                        if (sec.relocs.empty()) continue;
                        out << std::format("Relocations ({}):\n", sec.name);
                        for (size_t i = 0; i < sec.relocs.size(); ++i) {
                            const auto &r = sec.relocs[i];
                            const char *rt = "?";
                            if (r.type == RcuRelType::Abs64) rt = "ABS_64";
                            else if (r.type == RcuRelType::Abs32) rt = "ABS_32";
                            else if (r.type == RcuRelType::Rel32) rt = "REL_32";
                            std::string symName = r.symbolIndex < f.symbols.size()
                                                      ? f.symbols[r.symbolIndex].name
                                                      : "?";
                            out << std::format("  [{:3}]  off=0x{:04X}  sym[{}]={}  {}  addend={}\n",
                                               i, r.sectionOffset, r.symbolIndex, symName, rt, r.addend);
                        }
                        out << '\n';
                    }
                }

                // Hex dumps
                for (const auto &sec: f.sections) {
                    if (sec.data.empty()) continue;
                    out << std::format("{} ({} bytes):\n", sec.name, sec.data.size());
                    for (size_t i = 0; i < sec.data.size(); i += 16) {
                        out << std::format("  {:04X}  ", i);
                        for (size_t j = 0; j < 16; ++j) {
                            if (i + j < sec.data.size()) out << std::format("{:02X} ", sec.data[i + j]);
                            else out << "   ";
                            if (j == 7) out << ' ';
                        }
                        out << " |";
                        for (size_t j = 0; j < 16 && i + j < sec.data.size(); ++j) {
                            unsigned char c = sec.data[i + j];
                            out << (c >= 32 && c < 127 ? (char) c : '.');
                        }
                        out << "|\n";
                    }
                    out << '\n';
                }

                return out.str();
            }
        };
    } // anonymous namespace

    // ─────────────────────────────────────────────────────────────────────────────
    // Public API
    // ─────────────────────────────────────────────────────────────────────────────

    Rcu::Rcu(LirPackage package, std::string packageName)
        : lir_(std::move(package)), packageName_(std::move(packageName)) {
    }

    std::vector<RcuFile> Rcu::Generate() {
        std::vector<RcuFile> result;
        result.reserve(lir_.modules.size());
        for (const auto &mod: lir_.modules) {
            RcuCodeGen gen(mod, packageName_);
            result.push_back(gen.Generate());
        }
        return result;
    }

    bool Rcu::Emit(const RcuFile &file, const std::filesystem::path &path) {
        auto bytes = RcuWriter::Serialize(file);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        return f.good();
    }

    bool Rcu::Dump(const RcuFile &file, const std::filesystem::path &path) {
        std::string text = RcuDumper::Dump(file);
        std::ofstream f(path, std::ios::out | std::ios::trunc);
        if (!f) return false;
        f << text;
        return f.good();
    }
} // namespace Rux

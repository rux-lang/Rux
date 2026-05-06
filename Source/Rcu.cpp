/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Rcu.h"
#include "Rux/Version.h"

#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace Rux
{
    namespace
    {
        // Type utilities (mirrored from Asm.cpp)
        int SizeOf(const TypeRef& t)
        {
            switch (t.kind)
            {
            case TypeRef::Kind::Bool8: // Bool == Bool8
            case TypeRef::Kind::Char8: // Char8
            case TypeRef::Kind::Int8:
            case TypeRef::Kind::UInt8: return 1;
            case TypeRef::Kind::Bool16:
            case TypeRef::Kind::Char16:
            case TypeRef::Kind::Int16:
            case TypeRef::Kind::UInt16: return 2;
            case TypeRef::Kind::Bool32:
            case TypeRef::Kind::Char32: // Char == Char32
            case TypeRef::Kind::Int32:
            case TypeRef::Kind::UInt32:
            case TypeRef::Kind::Float32: return 4;
            case TypeRef::Kind::Opaque: return 0;
            default: return 8; // int, uint, int64, uint64, float64, pointer, str, named, …
            }
        }

        bool IsFloat(const TypeRef& t)
        {
            return t.kind == TypeRef::Kind::Float32 || t.kind == TypeRef::Kind::Float64;
        }

        int AlignUp(int v, int a) { return (v + a - 1) & ~(a - 1); }

        // String table
        class RcuStringTable
        {
        public:
            RcuStringTable() { data_.push_back('\0'); }

            uint32_t Intern(const std::string& s)
            {
                if (s.empty()) return 0;
                auto it = map_.find(s);
                if (it != map_.end()) return it->second;
                const auto off = static_cast<uint32_t>(data_.size());
                map_[s] = off;
                data_.insert(data_.end(), s.begin(), s.end());
                data_.push_back('\0');
                return off;
            }

            [[nodiscard]] uint32_t Size() const
            {
                return static_cast<uint32_t>(data_.size());
            }

            [[nodiscard]] const char* Data() const
            {
                return data_.data();
            }

            [[nodiscard]] std::string Get(const uint32_t off) const
            {
                if (off >= data_.size()) return {};
                return {data_.data() + off};
            }

        private:
            std::vector<char> data_;
            std::unordered_map<std::string, uint32_t> map_;
        };

        // x86-64 binary encoder
        // All accesses to stack slots use [rbp + disp] where disp is negative.
        // disp = -slotMap[vreg]  (i.e., pass negative displacement directly).
        class X64Enc
        {
        public:
            explicit X64Enc(std::vector<uint8_t>& buf) : out_(buf)
            {
            }

            [[nodiscard]] uint32_t Size() const
            {
                return static_cast<uint32_t>(out_.size());
            }

            void Byte(uint8_t b) const
            {
                out_.push_back(b);
            }

            void Dword(uint32_t d) const
            {
                out_.push_back(d & 0xFF);
                out_.push_back((d >> 8) & 0xFF);
                out_.push_back((d >> 16) & 0xFF);
                out_.push_back((d >> 24) & 0xFF);
            }

            void Qword(uint64_t q) const
            {
                for (int i = 0; i < 8; ++i)
                {
                    out_.push_back(q & 0xFF);
                    q >>= 8;
                }
            }

            void Patch32(uint32_t off, int32_t v) const
            {
                out_[off] = v & 0xFF;
                out_[off + 1] = (v >> 8) & 0xFF;
                out_[off + 2] = (v >> 16) & 0xFF;
                out_[off + 3] = (v >> 24) & 0xFF;
            }

            // Prologue / Epilogue
            void PushRbp() const
            {
                Byte(0x55);
            }

            void MovRbpRsp() const
            {
                Byte(0x48);
                Byte(0x89);
                Byte(0xE5);
            }

            void SubRspImm32(int32_t n) const
            {
                Byte(0x48);
                Byte(0x81);
                Byte(0xEC);
                Dword(static_cast<uint32_t>(n));
            }

            void Leave() const
            {
                Byte(0xC9);
            }

            void Ret() const
            {
                Byte(0xC3);
            }

            // RAX ↔ [RBP + disp32]
            void MovRaxLoad(const int32_t d) const
            {
                Byte(0x48);
                Byte(0x8B);
                Byte(0x85);
                Dword(u(d));
            }

            void MovRaxStore(const int32_t d) const
            {
                Byte(0x48);
                Byte(0x89);
                Byte(0x85);
                Dword(u(d));
            }

            void MovEaxLoad(const int32_t d) const
            {
                Byte(0x8B);
                Byte(0x85);
                Dword(u(d));
            }

            void MovEaxStore(const int32_t d) const
            {
                Byte(0x89);
                Byte(0x85);
                Dword(u(d));
            }

            void MovzxRaxWord(const int32_t d) const
            {
                Byte(0x48);
                Byte(0x0F);
                Byte(0xB7);
                Byte(0x85);
                Dword(u(d));
            }

            void MovzxRaxByte(const int32_t d) const
            {
                Byte(0x48);
                Byte(0x0F);
                Byte(0xB6);
                Byte(0x85);
                Dword(u(d));
            }

            void MovsxdRaxDword(const int32_t d) const
            {
                Byte(0x48);
                Byte(0x63);
                Byte(0x85);
                Dword(u(d));
            }

            void MovsxRaxWord(const int32_t d) const
            {
                Byte(0x48);
                Byte(0x0F);
                Byte(0xBF);
                Byte(0x85);
                Dword(u(d));
            }

            void MovsxRaxByte(const int32_t d) const
            {
                Byte(0x48);
                Byte(0x0F);
                Byte(0xBE);
                Byte(0x85);
                Dword(u(d));
            }

            void MovAxStore(const int32_t d) const
            {
                Byte(0x66);
                Byte(0x89);
                Byte(0x85);
                Dword(u(d));
            }

            void MovAlStore(const int32_t d) const
            {
                Byte(0x88);
                Byte(0x85);
                Dword(u(d));
            }

            // R10 ↔ [RBP + disp32]
            void MovR10Load(const int32_t d) const
            {
                Byte(0x4C);
                Byte(0x8B);
                Byte(0x95);
                Dword(u(d));
            }

            void MovR10Store(const int32_t d) const
            {
                Byte(0x4C);
                Byte(0x89);
                Byte(0x95);
                Dword(u(d));
            }

            void MovzxR10Word(const int32_t d) const
            {
                Byte(0x4C);
                Byte(0x0F);
                Byte(0xB7);
                Byte(0x95);
                Dword(u(d));
            }

            void MovzxR10Byte(const int32_t d) const
            {
                Byte(0x4C);
                Byte(0x0F);
                Byte(0xB6);
                Byte(0x95);
                Dword(u(d));
            }

            void MovsxdR10Dword(const int32_t d) const
            {
                Byte(0x4C);
                Byte(0x63);
                Byte(0x95);
                Dword(u(d));
            }

            void MovsxR10Word(const int32_t d) const
            {
                Byte(0x4C);
                Byte(0x0F);
                Byte(0xBF);
                Byte(0x95);
                Dword(u(d));
            }

            void MovsxR10Byte(const int32_t d) const
            {
                Byte(0x4C);
                Byte(0x0F);
                Byte(0xBE);
                Byte(0x95);
                Dword(u(d));
            }

            void MovR10dLoad(const int32_t d) const
            {
                Byte(0x44);
                Byte(0x8B);
                Byte(0x95);
                Dword(u(d));
            }

            // R11 ↔ [RBP + disp32]
            void MovR11Load(const int32_t d) const
            {
                Byte(0x4C);
                Byte(0x8B);
                Byte(0x9D);
                Dword(u(d));
            }

            void MovR11Store(const int32_t d) const
            {
                Byte(0x4C);
                Byte(0x89);
                Byte(0x9D);
                Dword(u(d));
            }

            // RCX ↔ stack (for shift count)
            void MovRcxLoad(const int32_t d) const
            {
                Byte(0x48);
                Byte(0x8B);
                Byte(0x8D);
                Dword(u(d));
            }

            // ABI arg regs ↔ [RBP + disp32]
            // argIdx: 0=RDI,1=RSI,2=RDX,3=RCX,4=R8,5=R9
            void MovArgLoad(const int idx, int32_t d) const
            {
                static const uint8_t rex[] = {0x48, 0x48, 0x48, 0x48, 0x4C, 0x4C};
                static const uint8_t modrm[] = {0xBD, 0xB5, 0x95, 0x8D, 0x85, 0x8D};
                Byte(rex[idx]);
                Byte(0x8B);
                Byte(modrm[idx]);
                Dword(u(d));
            }

            void MovArgStore(const int idx, int32_t d) const
            {
                static const uint8_t rex[] = {0x48, 0x48, 0x48, 0x48, 0x4C, 0x4C};
                static const uint8_t modrm[] = {0xBD, 0xB5, 0x95, 0x8D, 0x85, 0x8D};
                Byte(rex[idx]);
                Byte(0x89);
                Byte(modrm[idx]);
                Dword(u(d));
            }

            // Win64 ABI arg regs ↔ [RBP + disp32]
            // argIdx: 0=RCX,1=RDX,2=R8,3=R9
            void MovArgLoadWin64(const int idx, const int32_t d) const
            {
                static constexpr uint8_t rex[] = {0x48, 0x48, 0x4C, 0x4C};
                static constexpr uint8_t modrm[] = {0x8D, 0x95, 0x85, 0x8D};
                if (idx >= 4) return;
                Byte(rex[idx]);
                Byte(0x8B);
                Byte(modrm[idx]);
                Dword(u(d));
            }

            void MovArgStoreWin64(const int idx, const int32_t d) const
            {
                static constexpr uint8_t rex[] = {0x48, 0x48, 0x4C, 0x4C};
                static constexpr uint8_t modrm[] = {0x8D, 0x95, 0x85, 0x8D};
                if (idx >= 4) return;
                Byte(rex[idx]);
                Byte(0x89);
                Byte(modrm[idx]);
                Dword(u(d));
            }

            void LeaArgStackWin64(const int idx, const int32_t d) const
            {
                static constexpr uint8_t rex[] = {0x48, 0x48, 0x4C, 0x4C};
                static constexpr uint8_t modrm[] = {0x8D, 0x95, 0x85, 0x8D};
                if (idx >= 4) return;
                Byte(rex[idx]);
                Byte(0x8D);
                Byte(modrm[idx]);
                Dword(u(d));
            }

            void MovR10ArgWin64(const int idx) const
            {
                switch (idx)
                {
                case 0: Byte(0x49);
                    Byte(0x89);
                    Byte(0xCA);
                    break; // mov r10, rcx
                case 1: Byte(0x49);
                    Byte(0x89);
                    Byte(0xD2);
                    break; // mov r10, rdx
                case 2: Byte(0x4D);
                    Byte(0x89);
                    Byte(0xC2);
                    break; // mov r10, r8
                case 3: Byte(0x4D);
                    Byte(0x89);
                    Byte(0xCA);
                    break; // mov r10, r9
                default: break;
                }
            }

            void SubRspShadow() const
            {
                Byte(0x48);
                Byte(0x83);
                Byte(0xEC);
                Byte(0x20);
            }

            void AddRspShadow() const
            {
                Byte(0x48);
                Byte(0x83);
                Byte(0xC4);
                Byte(0x20);
            }

            // XMM arg regs ↔ [RBP + disp32] (N = 0..7)
            // MOVSS xmmN, [rbp + d]
            void MovssXmmNLoad(int n, int32_t d) const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x10);
                Byte(static_cast<uint8_t>(0x80 | (n << 3) | 5));
                Dword(u(d));
            }

            // MOVSD xmmN, [rbp + d]
            void MovsdXmmNLoad(int n, int32_t d) const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x10);
                Byte(static_cast<uint8_t>(0x80 | (n << 3) | 5));
                Dword(u(d));
            }

            // XMM0 / XMM1 ↔ [RBP + disp32]
            void MovssXmm0Load(int32_t d) const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x85);
                Dword(u(d));
            }

            void MovsdXmm0Load(int32_t d) const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x85);
                Dword(u(d));
            }

            void MovssXmm1Load(int32_t d) const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x8D);
                Dword(u(d));
            }

            void MovsdXmm1Load(int32_t d) const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x8D);
                Dword(u(d));
            }

            void MovssXmm0Store(int32_t d) const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x11);
                Byte(0x85);
                Dword(u(d));
            }

            void MovsdXmm0Store(int32_t d) const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x11);
                Byte(0x85);
                Dword(u(d));
            }

            void MovssXmm1Store(int32_t d) const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x11);
                Byte(0x8D);
                Dword(u(d));
            }

            void MovsdXmm1Store(int32_t d) const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x11);
                Byte(0x8D);
                Dword(u(d));
            }

            // XMM0 / XMM1, [RIP + rel32] (RIP-relative rodata load)
            void MovssXmm0Rip(uint32_t& relocOff) const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x05);
                relocOff = Size();
                Dword(0);
            }

            void MovsdXmm0Rip(uint32_t& relocOff) const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x05);
                relocOff = Size();
                Dword(0);
            }

            void MovssXmm1Rip(uint32_t& relocOff) const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x0D);
                relocOff = Size();
                Dword(0);
            }

            void MovsdXmm1Rip(uint32_t& relocOff) const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x10);
                Byte(0x0D);
                relocOff = Size();
                Dword(0);
            }

            // Immediate loads
            void MovRaxImm64(int64_t v) const
            {
                Byte(0x48);
                Byte(0xB8);
                Qword(static_cast<uint64_t>(v));
            }

            void MovEaxImm32(int32_t v) const
            {
                Byte(0xB8);
                Dword(static_cast<uint32_t>(v));
            }

            // LEA / MOV rax, [rip + rel32]
            void LeaRaxRip(uint32_t& relocOff) const
            {
                Byte(0x48);
                Byte(0x8D);
                Byte(0x05);
                relocOff = Size();
                Dword(0);
            }

            void MovRaxRip(uint32_t& relocOff) const
            {
                Byte(0x48);
                Byte(0x8B);
                Byte(0x05);
                relocOff = Size();
                Dword(0);
            }

            void LeaRaxStack(int32_t d) const
            {
                Byte(0x48);
                Byte(0x8D);
                Byte(0x85);
                Dword(u(d));
            }

            // Register-to-register
            void MovRaxR10() const
            {
                Byte(0x4C);
                Byte(0x89);
                Byte(0xD0);
            } // mov rax, r10

            void MovRcxR11() const
            {
                Byte(0x4C);
                Byte(0x89);
                Byte(0xD9);
            } // mov rcx, r11

            void MovRaxRdx() const
            {
                Byte(0x48);
                Byte(0x8B);
                Byte(0xC2);
            } // mov rax, rdx

            // Integer arithmetic (RAX op R10 → RAX)
            void AddRaxR10() const
            {
                Byte(0x4C);
                Byte(0x01);
                Byte(0xD0);
            }

            void SubRaxR10() const
            {
                Byte(0x4C);
                Byte(0x29);
                Byte(0xD0);
            }

            void AndRaxR10() const
            {
                Byte(0x4C);
                Byte(0x21);
                Byte(0xD0);
            }

            void OrRaxR10() const
            {
                Byte(0x4C);
                Byte(0x09);
                Byte(0xD0);
            }

            void XorRaxR10() const
            {
                Byte(0x4C);
                Byte(0x31);
                Byte(0xD0);
            }

            void ImulRaxR10() const
            {
                Byte(0x49);
                Byte(0x0F);
                Byte(0xAF);
                Byte(0xC2);
            }

            void NegRax() const
            {
                Byte(0x48);
                Byte(0xF7);
                Byte(0xD8);
            }

            void NotRax() const
            {
                Byte(0x48);
                Byte(0xF7);
                Byte(0xD0);
            }

            // Division
            void Cqo() const
            {
                Byte(0x48);
                Byte(0x99);
            }

            void XorRdxRdx() const
            {
                Byte(0x48);
                Byte(0x31);
                Byte(0xD2);
            }

            void IdivR10() const
            {
                Byte(0x49);
                Byte(0xF7);
                Byte(0xFA);
            }

            void DivR10() const
            {
                Byte(0x49);
                Byte(0xF7);
                Byte(0xF2);
            }

            // Shifts
            void ShlRaxCl() const
            {
                Byte(0x48);
                Byte(0xD3);
                Byte(0xE0);
            }

            void ShrRaxCl() const
            {
                Byte(0x48);
                Byte(0xD3);
                Byte(0xE8);
            }

            void SarRaxCl() const
            {
                Byte(0x48);
                Byte(0xD3);
                Byte(0xF8);
            }

            // Comparisons
            void TestRaxRax() const
            {
                Byte(0x48);
                Byte(0x85);
                Byte(0xC0);
            }

            void CmpRaxR10() const
            {
                Byte(0x4C);
                Byte(0x39);
                Byte(0xD0);
            }

            void CmpRaxImm32(int32_t v) const
            {
                Byte(0x48);
                Byte(0x81);
                Byte(0xF8);
                Dword(u(v));
            }

            // SETcc AL + MOVZX RAX, AL
            void SeteAl() const
            {
                Byte(0x0F);
                Byte(0x94);
                Byte(0xC0);
            }

            void SetneAl() const
            {
                Byte(0x0F);
                Byte(0x95);
                Byte(0xC0);
            }

            void SetlAl() const
            {
                Byte(0x0F);
                Byte(0x9C);
                Byte(0xC0);
            }

            void SetleAl() const
            {
                Byte(0x0F);
                Byte(0x9E);
                Byte(0xC0);
            }

            void SetgAl() const
            {
                Byte(0x0F);
                Byte(0x9F);
                Byte(0xC0);
            }

            void SetgeAl() const
            {
                Byte(0x0F);
                Byte(0x9D);
                Byte(0xC0);
            }

            void SetbAl() const
            {
                Byte(0x0F);
                Byte(0x92);
                Byte(0xC0);
            }

            void SetbeAl() const
            {
                Byte(0x0F);
                Byte(0x96);
                Byte(0xC0);
            }

            void SetaAl() const
            {
                Byte(0x0F);
                Byte(0x97);
                Byte(0xC0);
            }

            void SetaeAl() const
            {
                Byte(0x0F);
                Byte(0x93);
                Byte(0xC0);
            }

            void MovzxRaxAl() const
            {
                Byte(0x48);
                Byte(0x0F);
                Byte(0xB6);
                Byte(0xC0);
            }

            // Float arithmetic (XMM0 op XMM1 → XMM0)
            void AddssXmm01() const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x58);
                Byte(0xC1);
            }

            void SubssXmm01() const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x5C);
                Byte(0xC1);
            }

            void MulssXmm01() const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x59);
                Byte(0xC1);
            }

            void DivssXmm01() const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x5E);
                Byte(0xC1);
            }

            void AddsdXmm01() const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x58);
                Byte(0xC1);
            }

            void SubsdXmm01() const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x5C);
                Byte(0xC1);
            }

            void MulsdXmm01() const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x59);
                Byte(0xC1);
            }

            void DivsdXmm01() const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x5E);
                Byte(0xC1);
            }

            // Float compare
            void UcomissXmm01() const
            {
                Byte(0x0F);
                Byte(0x2E);
                Byte(0xC1);
            }

            void UcomisdXmm01() const
            {
                Byte(0x66);
                Byte(0x0F);
                Byte(0x2E);
                Byte(0xC1);
            }

            // Float sign negate (XOR with mask)
            void XorpsXmm01() const
            {
                Byte(0x0F);
                Byte(0x57);
                Byte(0xC1);
            }

            void XorpdXmm01() const
            {
                Byte(0x66);
                Byte(0x0F);
                Byte(0x57);
                Byte(0xC1);
            }

            // Float conversions
            void Cvtsi2ssXmm0Rax() const
            {
                Byte(0xF3);
                Byte(0x48);
                Byte(0x0F);
                Byte(0x2A);
                Byte(0xC0);
            }

            void Cvtsi2sdXmm0Rax() const
            {
                Byte(0xF2);
                Byte(0x48);
                Byte(0x0F);
                Byte(0x2A);
                Byte(0xC0);
            }

            void CvttsssiRaxXmm0() const
            {
                Byte(0xF3);
                Byte(0x48);
                Byte(0x0F);
                Byte(0x2C);
                Byte(0xC0);
            }

            void CvttsdsiRaxXmm0() const
            {
                Byte(0xF2);
                Byte(0x48);
                Byte(0x0F);
                Byte(0x2C);
                Byte(0xC0);
            }

            void CvtsssdXmm0() const
            {
                Byte(0xF3);
                Byte(0x0F);
                Byte(0x5A);
                Byte(0xC0);
            }

            void CvtsdssXmm0() const
            {
                Byte(0xF2);
                Byte(0x0F);
                Byte(0x5A);
                Byte(0xC0);
            }

            // Control flow
            void Jmp(uint32_t& patchOff) const
            {
                Byte(0xE9);
                patchOff = Size();
                Dword(0);
            }

            void Jz(uint32_t& patchOff) const
            {
                Byte(0x0F);
                Byte(0x84);
                patchOff = Size();
                Dword(0);
            }

            void Jnz(uint32_t& patchOff) const
            {
                Byte(0x0F);
                Byte(0x85);
                patchOff = Size();
                Dword(0);
            }

            void Je(uint32_t& patchOff) const
            {
                Byte(0x0F);
                Byte(0x84);
                patchOff = Size();
                Dword(0);
            }

            void Call(uint32_t& relocOff) const
            {
                Byte(0xE8);
                relocOff = Size();
                Dword(0);
            }

            void CallR10() const
            {
                Byte(0x41);
                Byte(0xFF);
                Byte(0xD2);
            }

            // Aggregate helpers
            void ImulR11R10Imm32(int32_t v) const
            {
                Byte(0x4D);
                Byte(0x69);
                Byte(0xDA);
                Dword(u(v));
            }

            void AddRaxR11() const
            {
                Byte(0x4C);
                Byte(0x01);
                Byte(0xD8);
            }

            void LeaRaxRaxDisp(int32_t v) const
            {
                Byte(0x48);
                Byte(0x8D);
                Byte(0x80);
                Dword(u(v));
            }

        private:
            std::vector<uint8_t>& out_;

            static uint32_t u(const int32_t v)
            {
                return static_cast<uint32_t>(v);
            }
        };

        // This compiler targets Windows x64 (PE32+) exclusively.
        // CallingConvention::Default therefore resolves to Win64.
        CallingConvention EffectiveConv(const CallingConvention c)
        {
            return c == CallingConvention::Default ? CallingConvention::Win64 : c;
        }

        // RCU Code Generator: LirModule → RcuFile
        struct JumpPatch
        {
            uint32_t patchOff;
            uint32_t targetBlock;
        };

        class RcuCodeGen
        {
        public:
            explicit RcuCodeGen(const LirModule& mod, std::string pkgName)
                : mod(mod), pkgName(std::move(pkgName)), enc(textData)
            {
            }

            RcuFile Generate();

        private:
            const LirModule& mod;
            std::string pkgName;

            // Section data buffers
            std::vector<uint8_t> textData;
            std::vector<uint8_t> rodataData;
            std::vector<uint8_t> dataData;

            // Per-section relocations
            std::vector<RcuReloc> textRelocs;
            std::vector<RcuReloc> rodataRelocs;

            // Symbol table and string table
            std::vector<RcuSymbol> symbols;
            RcuStringTable strings;

            // Encoder writing into textData
            X64Enc enc;

            // Interned rodata constants: key → symbol index
            std::unordered_map<std::string, uint32_t> strSyms;
            std::unordered_map<std::string, uint32_t> f32Syms;
            std::unordered_map<std::string, uint32_t> f64Syms;
            int constIdx = 0;
            uint32_t f32SignMaskSym = ~0u;
            uint32_t f64SignMaskSym = ~0u;

            // Declared extern symbols (by name → symbol index)
            std::unordered_map<std::string, uint32_t> externSyms;

            // Struct field layouts
            struct FieldLayout
            {
                std::string name;
                int offset = 0;
                int size = 0;
            };

            struct StructLayout
            {
                std::vector<FieldLayout> fields;
                int totalSize = 0;
                int alignment = 1;
            };

            using LayoutMap = std::unordered_map<std::string, StructLayout>;
            LayoutMap layouts;

            // Per-function state
            struct PhiMove
            {
                LirReg dst;
                LirReg src;
                TypeRef type;
            };

            std::unordered_map<LirReg, int32_t> slotMap;
            std::unordered_map<LirReg, int32_t> allocaData;
            std::unordered_map<LirReg, TypeRef> regTypes;
            std::unordered_map<uint32_t,
                               std::unordered_map<uint32_t, std::vector<PhiMove>>> phiMoves;
            int32_t nextOff = 0;
            int32_t frameSize = 0;

            std::vector<uint32_t> blockOffsets;
            std::vector<JumpPatch> jumpPatches;

            // Helpers
            [[nodiscard]] int32_t Disp(const LirReg r) const
            {
                return -static_cast<int32_t>(slotMap.at(r));
            }

            static std::string BaseTypeName(const std::string& name)
            {
                const std::size_t pos = name.find('<');
                return pos == std::string::npos ? name : name.substr(0, pos);
            }

            [[nodiscard]] int SizeOfRuntime(const TypeRef& t) const
            {
                if (t.kind == TypeRef::Kind::Named)
                {
                    const std::string base = BaseTypeName(t.name);
                    if (base == "Slice") return 16;
                    auto it = layouts.find(base);
                    if (it != layouts.end()) return it->second.totalSize;
                }
                return SizeOf(t);
            }

            [[nodiscard]] bool IsWin64ByRefAggregate(const TypeRef& t) const
            {
                return SizeOfRuntime(t) == 16;
            }

            [[nodiscard]] bool IsPointerToWin64ByRefAggregate(const TypeRef& t) const
            {
                return t.kind == TypeRef::Kind::Pointer &&
                    !t.inner.empty() &&
                    IsWin64ByRefAggregate(t.inner[0]);
            }

            uint32_t AddSymbol(RcuSymbol s)
            {
                auto idx = static_cast<uint32_t>(symbols.size());
                symbols.push_back(std::move(s));
                return idx;
            }

            uint32_t GetOrAddExtern(const std::string& name, uint8_t kind,
                                    const std::string& dll = {})
            {
                auto it = externSyms.find(name);
                if (it != externSyms.end()) return it->second;
                RcuSymbol s;
                s.name = name;
                s.typeName = dll;
                s.kind = kind;
                s.visibility = RcuSymVis::Global;
                s.sectionIdx = RCU_SEC_EXTERNAL;
                uint32_t idx = AddSymbol(s);
                externSyms[name] = idx;
                return idx;
            }

            // Align rodataData_ to `align` bytes (zero-fill), return current offset.
            uint32_t AlignRodata(int align)
            {
                while (rodataData.size() % align)
                    rodataData.push_back(0);
                return static_cast<uint32_t>(rodataData.size());
            }

            uint32_t InternStr(const std::string& val)
            {
                auto it = strSyms.find(val);
                if (it != strSyms.end()) return it->second;
                auto off = static_cast<uint32_t>(rodataData.size());
                for (unsigned char c : val) rodataData.push_back(c);
                rodataData.push_back(0);
                std::string lbl = std::format("__str{}", constIdx++);
                RcuSymbol s;
                s.name = lbl;
                s.sectionIdx = RCU_RODATA_IDX;
                s.value = off;
                s.size = static_cast<uint32_t>(val.size() + 1);
                s.kind = RcuSymKind::Const;
                s.visibility = RcuSymVis::Local;
                uint32_t idx = AddSymbol(s);
                strSyms[val] = idx;
                return idx;
            }

            uint32_t InternF32(const std::string& val)
            {
                auto it = f32Syms.find(val);
                if (it != f32Syms.end()) return it->second;
                uint32_t off = AlignRodata(4);
                float fv = std::stof(val);
                uint32_t bits;
                std::memcpy(&bits, &fv, 4);
                for (int i = 0; i < 4; ++i)
                {
                    rodataData.push_back(bits & 0xFF);
                    bits >>= 8;
                }
                std::string lbl = std::format("__f32_{}", constIdx++);
                RcuSymbol s;
                s.name = lbl;
                s.sectionIdx = RCU_RODATA_IDX;
                s.value = off;
                s.size = 4;
                s.kind = RcuSymKind::Const;
                s.visibility = RcuSymVis::Local;
                uint32_t idx = AddSymbol(s);
                f32Syms[val] = idx;
                return idx;
            }

            uint32_t InternF64(const std::string& val)
            {
                auto it = f64Syms.find(val);
                if (it != f64Syms.end()) return it->second;
                uint32_t off = AlignRodata(8);
                double dv = std::stod(val);
                uint64_t bits;
                std::memcpy(&bits, &dv, 8);
                for (int i = 0; i < 8; ++i)
                {
                    rodataData.push_back(bits & 0xFF);
                    bits >>= 8;
                }
                std::string lbl = std::format("__f64_{}", constIdx++);
                RcuSymbol s;
                s.name = lbl;
                s.sectionIdx = RCU_RODATA_IDX;
                s.value = off;
                s.size = 8;
                s.kind = RcuSymKind::Const;
                s.visibility = RcuSymVis::Local;
                uint32_t idx = AddSymbol(s);
                f64Syms[val] = idx;
                return idx;
            }

            uint32_t InternF32SignMask()
            {
                if (f32SignMaskSym != ~0u) return f32SignMaskSym;
                uint32_t off = AlignRodata(4);
                // 0x80000000 — sign bit of f32
                rodataData.push_back(0x00);
                rodataData.push_back(0x00);
                rodataData.push_back(0x00);
                rodataData.push_back(0x80);
                RcuSymbol s;
                s.name = "__f32_sign_mask";
                s.sectionIdx = RCU_RODATA_IDX;
                s.value = off;
                s.size = 4;
                s.kind = RcuSymKind::Const;
                s.visibility = RcuSymVis::Local;
                f32SignMaskSym = AddSymbol(s);
                return f32SignMaskSym;
            }

            uint32_t InternF64SignMask()
            {
                if (f64SignMaskSym != ~0u) return f64SignMaskSym;
                uint32_t off = AlignRodata(8);
                // 0x8000000000000000 — sign bit of f64
                for (int i = 0; i < 7; ++i) rodataData.push_back(0x00);
                rodataData.push_back(0x80);
                RcuSymbol s;
                s.name = "__f64_sign_mask";
                s.sectionIdx = RCU_RODATA_IDX;
                s.value = off;
                s.size = 8;
                s.kind = RcuSymKind::Const;
                s.visibility = RcuSymVis::Local;
                f64SignMaskSym = AddSymbol(s);
                return f64SignMaskSym;
            }

            void AddTextReloc(uint32_t sectionOff, uint32_t symIdx, int32_t addend = 0)
            {
                textRelocs.push_back({sectionOff, symIdx, RcuRelType::Rel32, addend});
            }

            void PatchJumps()
            {
                for (const auto& p : jumpPatches)
                {
                    auto target = static_cast<int32_t>(blockOffsets[p.targetBlock]);
                    int32_t rel32 = target - static_cast<int32_t>(p.patchOff + 4);
                    enc.Patch32(p.patchOff, rel32);
                }
                jumpPatches.clear();
            }

            // Load A (rax / xmm0) and B (r10 / xmm1)
            void LoadA(const LirReg reg, const TypeRef& t) const
            {
                const int sz = SizeOf(t);
                const int32_t d = Disp(reg);
                if (IsFloat(t))
                {
                    if (t.kind == TypeRef::Kind::Float32) enc.MovssXmm0Load(d);
                    else enc.MovsdXmm0Load(d);
                }
                else if (sz == 8 || sz == 0)
                {
                    enc.MovRaxLoad(d);
                }
                else if (t.IsSigned())
                {
                    if (sz == 4) enc.MovsxdRaxDword(d);
                    else if (sz == 2) enc.MovsxRaxWord(d);
                    else enc.MovsxRaxByte(d);
                }
                else
                {
                    if (sz == 4) enc.MovEaxLoad(d);
                    else if (sz == 2) enc.MovzxRaxWord(d);
                    else enc.MovzxRaxByte(d);
                }
            }

            void LoadB(LirReg reg, const TypeRef& t) const
            {
                int sz = SizeOf(t);
                int32_t d = Disp(reg);
                if (IsFloat(t))
                {
                    if (t.kind == TypeRef::Kind::Float32) enc.MovssXmm1Load(d);
                    else enc.MovsdXmm1Load(d);
                }
                else if (sz == 8 || sz == 0)
                {
                    enc.MovR10Load(d);
                }
                else if (t.IsSigned())
                {
                    if (sz == 4) enc.MovsxdR10Dword(d);
                    else if (sz == 2) enc.MovsxR10Word(d);
                    else enc.MovsxR10Byte(d);
                }
                else
                {
                    if (sz == 4) enc.MovR10dLoad(d);
                    else if (sz == 2) enc.MovzxR10Word(d);
                    else enc.MovzxR10Byte(d);
                }
            }

            void StoreA(LirReg dst, const TypeRef& t) const
            {
                int sz = SizeOf(t);
                int32_t d = Disp(dst);
                if (IsFloat(t))
                {
                    if (t.kind == TypeRef::Kind::Float32) enc.MovssXmm0Store(d);
                    else enc.MovsdXmm0Store(d);
                }
                else
                {
                    int ss = (sz > 0) ? sz : 8;
                    if (ss == 8) enc.MovRaxStore(d);
                    else if (ss == 4) enc.MovEaxStore(d);
                    else if (ss == 2) enc.MovAxStore(d);
                    else enc.MovAlStore(d);
                }
            }

            // Struct field lookup
            int FieldOffset(LirReg base, const std::string& fieldName)
            {
                auto typeIt = regTypes.find(base);
                if (typeIt == regTypes.end()) return 0;
                const TypeRef& pt = typeIt->second;
                if (pt.kind != TypeRef::Kind::Pointer || pt.inner.empty()) return 0;
                const TypeRef& inner = pt.inner[0];
                if (inner.kind != TypeRef::Kind::Named) return 0;
                const std::string baseName = BaseTypeName(inner.name);
                if (baseName == "Slice")
                {
                    if (fieldName == "data") return 0;
                    if (fieldName == "length") return 8;
                    return 0;
                }
                auto layIt = layouts.find(baseName);
                if (layIt == layouts.end()) return 0;
                for (const auto& field : layIt->second.fields)
                    if (field.name == fieldName) return field.offset;
                return 0;
            }

            // Pre-pass: allocate stack slots
            int32_t AllocSlot(LirReg reg, int bytes)
            {
                if (auto it = slotMap.find(reg); it != slotMap.end()) return it->second;
                int al = (bytes > 0) ? std::min(bytes, 8) : 1;
                nextOff = AlignUp(nextOff, al);
                nextOff += (bytes > 0 ? bytes : 8);
                slotMap[reg] = nextOff;
                return nextOff;
            }

            int32_t AllocRegion(int bytes)
            {
                int al = (bytes > 0) ? std::min(bytes, 8) : 1;
                nextOff = AlignUp(nextOff, al);
                nextOff += (bytes > 0 ? bytes : 8);
                return nextOff;
            }

            void PrepassFunc(const LirFunc& func)
            {
                nextOff = 0;
                frameSize = 0;
                slotMap.clear();
                allocaData.clear();
                regTypes.clear();
                phiMoves.clear();
                for (const auto& p : func.params)
                {
                    int sz = SizeOfRuntime(p.type);
                    AllocSlot(p.reg, sz > 0 ? sz : 8);
                    regTypes[p.reg] = p.type;
                }
                for (uint32_t bi = 0; bi < func.blocks.size(); ++bi)
                {
                    for (const auto& instr : func.blocks[bi].instrs)
                    {
                        if (instr.op == LirOpcode::Phi)
                        {
                            for (const auto& [src, pred] : instr.phiPreds)
                                phiMoves[pred][bi].push_back({instr.dst, src, instr.type});
                        }
                        if (instr.dst == LirNoReg) continue;
                        if (instr.op == LirOpcode::Alloca)
                        {
                            AllocSlot(instr.dst, 8);
                            int dsz;
                            if (!instr.strArg.empty())
                            {
                                int count = 0;
                                try
                                {
                                    count = std::stoi(instr.strArg);
                                }
                                catch (...)
                                {
                                }
                                const TypeRef& elemType =
                                    instr.type.inner.empty() ? instr.type : instr.type.inner[0];
                                int elemSize = SizeOfRuntime(elemType);
                                dsz = count * (elemSize > 0 ? elemSize : 8);
                            }
                            else
                            {
                                dsz = SizeOfRuntime(instr.type);
                            }
                            allocaData[instr.dst] = AllocRegion(dsz > 0 ? dsz : 8);
                            regTypes[instr.dst] = TypeRef::MakePointer(instr.type);
                        }
                        else
                        {
                            int sz = SizeOfRuntime(instr.type);
                            AllocSlot(instr.dst, sz > 0 ? sz : 8);
                            regTypes[instr.dst] = instr.type;
                        }
                    }
                }

                frameSize = AlignUp(nextOff, 16);
                if (frameSize == 0) frameSize = 16;
            }

            // Build struct layouts
            void BuildLayouts()
            {
                for (const auto& s : mod.structs)
                {
                    StructLayout layout;
                    int offset = 0, maxAlign = 1;
                    for (const auto& f : s.fields)
                    {
                        int sz = SizeOfRuntime(f.type);
                        int al = sz > 0 ? std::min(sz, 8) : 1;
                        if (f.type.kind == TypeRef::Kind::Named)
                        {
                            auto it = layouts.find(BaseTypeName(f.type.name));
                            if (it != layouts.end())
                            {
                                sz = it->second.totalSize;
                                al = it->second.alignment;
                            }
                        }
                        if (al > 1) offset = AlignUp(offset, al);
                        layout.fields.push_back({f.name, offset, sz});
                        offset += (sz > 0 ? sz : 8);
                        maxAlign = std::max(maxAlign, al);
                    }
                    layout.totalSize = AlignUp(offset, maxAlign);
                    layout.alignment = maxAlign;
                    layouts[s.name] = std::move(layout);
                }
            }

            // Phi move emission
            bool HasPhiMoves(const uint32_t from, uint32_t to) const
            {
                auto it = phiMoves.find(from);
                if (it == phiMoves.end()) return false;
                return it->second.contains(to);
            }

            void EmitPhiMoves(const uint32_t from, uint32_t to)
            {
                auto it1 = phiMoves.find(from);
                if (it1 == phiMoves.end()) return;
                auto it2 = it1->second.find(to);
                if (it2 == it1->second.end()) return;
                for (const auto& m : it2->second)
                {
                    if (!slotMap.contains(m.src)) continue;
                    LoadA(m.src, m.type);
                    StoreA(m.dst, m.type);
                }
            }

            // Call argument setup
            void EmitCallArgs(const std::vector<LirReg>& args,
                              CallingConvention conv = CallingConvention::Default) const
            {
                if (EffectiveConv(conv) == CallingConvention::Win64)
                {
                    // Unified index: rcx/xmm0=0, rdx/xmm1=1, r8/xmm2=2, r9/xmm3=3
                    int idx = 0;
                    for (LirReg arg : args)
                    {
                        if (idx >= 4) break;
                        TypeRef at = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64();
                        int32_t d = Disp(arg);
                        if (IsFloat(at))
                        {
                            int sz = SizeOf(at);
                            if (sz == 4) enc.MovssXmmNLoad(idx, d);
                            else enc.MovsdXmmNLoad(idx, d);
                        }
                        else if (IsWin64ByRefAggregate(at))
                        {
                            enc.LeaArgStackWin64(idx, d);
                        }
                        else if (IsPointerToWin64ByRefAggregate(at))
                        {
                            enc.MovArgLoadWin64(idx, d);
                        }
                        else
                        {
                            enc.MovArgLoadWin64(idx, d);
                        }
                        ++idx;
                    }
                }
                else
                {
                    int intIdx = 0, fltIdx = 0;
                    for (LirReg arg : args)
                    {
                        TypeRef at = regTypes.contains(arg) ? regTypes.at(arg) : TypeRef::MakeInt64();
                        int32_t d = Disp(arg);
                        if (IsFloat(at))
                        {
                            if (fltIdx < 8)
                            {
                                int sz = SizeOf(at);
                                if (sz == 4) enc.MovssXmmNLoad(fltIdx, d);
                                else enc.MovsdXmmNLoad(fltIdx, d);
                                ++fltIdx;
                            }
                        }
                        else
                        {
                            if (intIdx < 6)
                            {
                                enc.MovArgLoad(intIdx, d);
                                ++intIdx;
                            }
                        }
                    }
                }
            }

            // Instruction code generation
            void GenInstr(const LirInstr& instr)
            {
                switch (instr.op)
                {
                case LirOpcode::Const:
                    {
                        if (instr.dst == LirNoReg) break;
                        const TypeRef& t = instr.type;
                        int sz = SizeOf(t);
                        if (t.kind == TypeRef::Kind::Str)
                        {
                            uint32_t symIdx = InternStr(instr.strArg);
                            uint32_t relocOff;
                            enc.LeaRaxRip(relocOff);
                            AddTextReloc(relocOff, symIdx);
                            enc.MovRaxStore(Disp(instr.dst));
                        }
                        else if (t.kind == TypeRef::Kind::Float32)
                        {
                            uint32_t symIdx = InternF32(instr.strArg);
                            uint32_t relocOff;
                            enc.MovssXmm0Rip(relocOff);
                            AddTextReloc(relocOff, symIdx);
                            enc.MovssXmm0Store(Disp(instr.dst));
                        }
                        else if (t.kind == TypeRef::Kind::Float64)
                        {
                            uint32_t symIdx = InternF64(instr.strArg);
                            uint32_t relocOff;
                            enc.MovsdXmm0Rip(relocOff);
                            AddTextReloc(relocOff, symIdx);
                            enc.MovsdXmm0Store(Disp(instr.dst));
                        }
                        else if (t.IsBool())
                        {
                            enc.MovEaxImm32(instr.strArg == "true" ? 1 : 0);
                            StoreA(instr.dst, t);
                        }
                        else
                        {
                            const std::string& sv = instr.strArg.empty() ? "0" : instr.strArg;
                            int64_t v = 0;
                            try { v = std::stoll(sv); }
                            catch (...)
                            {
                            }
                            if (v >= 0 && v <= 0x7FFFFFFF)
                                enc.MovEaxImm32(static_cast<int32_t>(v));
                            else
                                enc.MovRaxImm64(v);
                            StoreA(instr.dst, sz > 0 ? t : TypeRef::MakeInt64());
                        }
                        break;
                    }
                case LirOpcode::Alloca:
                    {
                        int32_t dataOff = allocaData.at(instr.dst);
                        enc.LeaRaxStack(-dataOff);
                        enc.MovRaxStore(Disp(instr.dst));
                        break;
                    }
                case LirOpcode::Load:
                    {
                        const TypeRef& t = instr.type;
                        int sz = SizeOf(t);
                        int runtimeSz = SizeOfRuntime(t);
                        if (!instr.strArg.empty())
                        {
                            // Named global — load via RIP-relative
                            uint32_t symIdx = GetOrAddExtern(instr.strArg, RcuSymKind::ExternData);
                            uint32_t relocOff;
                            enc.MovRaxRip(relocOff);
                            AddTextReloc(relocOff, symIdx);
                        }
                        else
                        {
                            LirReg ptr = instr.srcs[0];
                            enc.MovR10Load(Disp(ptr));
                            if (runtimeSz == 16)
                            {
                                enc.Byte(0x49);
                                enc.Byte(0x8B);
                                enc.Byte(0x02); // mov rax, [r10]
                                enc.MovRaxStore(Disp(instr.dst));
                                enc.Byte(0x49);
                                enc.Byte(0x8B);
                                enc.Byte(0x42);
                                enc.Byte(0x08); // mov rax, [r10 + 8]
                                enc.MovRaxStore(Disp(instr.dst) + 8);
                                break;
                            }
                            // Load through pointer: use r10 as base
                            // Emit: mov rax, [r10]  (49 8B 02)
                            if (IsFloat(t))
                            {
                                // movss/movsd xmm0, [r10]
                                if (sz == 4)
                                {
                                    enc.Byte(0xF3);
                                    enc.Byte(0x41);
                                    enc.Byte(0x0F);
                                    enc.Byte(0x10);
                                    enc.Byte(0x02);
                                }
                                else
                                {
                                    enc.Byte(0xF2);
                                    enc.Byte(0x41);
                                    enc.Byte(0x0F);
                                    enc.Byte(0x10);
                                    enc.Byte(0x02);
                                }
                                StoreA(instr.dst, t);
                                break;
                            }
                            else if (sz == 8 || sz == 0)
                            {
                                enc.Byte(0x49);
                                enc.Byte(0x8B);
                                enc.Byte(0x02); // mov rax, [r10]
                            }
                            else if (t.IsSigned())
                            {
                                if (sz == 4)
                                {
                                    enc.Byte(0x49);
                                    enc.Byte(0x63);
                                    enc.Byte(0x02); // movsxd rax,[r10]
                                }
                                else if (sz == 2)
                                {
                                    enc.Byte(0x49);
                                    enc.Byte(0x0F);
                                    enc.Byte(0xBF);
                                    enc.Byte(0x02);
                                }
                                else
                                {
                                    enc.Byte(0x49);
                                    enc.Byte(0x0F);
                                    enc.Byte(0xBE);
                                    enc.Byte(0x02);
                                }
                            }
                            else
                            {
                                if (sz == 4)
                                {
                                    enc.Byte(0x41);
                                    enc.Byte(0x8B);
                                    enc.Byte(0x02); // mov eax, [r10]  (zero-extends to rax)
                                }
                                else if (sz == 2)
                                {
                                    enc.Byte(0x49);
                                    enc.Byte(0x0F);
                                    enc.Byte(0xB7);
                                    enc.Byte(0x02); // movzx rax, word [r10]
                                }
                                else
                                {
                                    enc.Byte(0x49);
                                    enc.Byte(0x0F);
                                    enc.Byte(0xB6);
                                    enc.Byte(0x02); // movzx rax, byte [r10]
                                }
                            }
                        }
                        StoreA(instr.dst, sz > 0 ? t : TypeRef::MakeInt64());
                        break;
                    }
                case LirOpcode::Store:
                    {
                        LirReg val = instr.srcs[0];
                        LirReg ptr = instr.srcs[1];
                        const TypeRef& t = instr.type;
                        int sz = SizeOf(t);
                        int runtimeSz = SizeOfRuntime(t);
                        enc.MovR11Load(Disp(ptr));
                        if (runtimeSz == 16)
                        {
                            enc.MovRaxLoad(Disp(val));
                            enc.Byte(0x49);
                            enc.Byte(0x89);
                            enc.Byte(0x03); // mov [r11], rax
                            enc.MovRaxLoad(Disp(val) + 8);
                            enc.Byte(0x49);
                            enc.Byte(0x89);
                            enc.Byte(0x43);
                            enc.Byte(0x08); // mov [r11 + 8], rax
                            break;
                        }
                        if (IsFloat(t))
                        {
                            LoadA(val, t);
                            // movss/movsd [r11], xmm0
                            if (sz == 4)
                            {
                                enc.Byte(0xF3);
                                enc.Byte(0x41);
                                enc.Byte(0x0F);
                                enc.Byte(0x11);
                                enc.Byte(0x03);
                            }
                            else
                            {
                                enc.Byte(0xF2);
                                enc.Byte(0x41);
                                enc.Byte(0x0F);
                                enc.Byte(0x11);
                                enc.Byte(0x03);
                            }
                        }
                        else
                        {
                            const int ss = (sz > 0) ? sz : 8;
                            LoadA(val, t);
                            // mov [r11], rax/eax/ax/al
                            if (ss == 8)
                            {
                                enc.Byte(0x49);
                                enc.Byte(0x89);
                                enc.Byte(0x03);
                            }
                            else if (ss == 4)
                            {
                                enc.Byte(0x41);
                                enc.Byte(0x89);
                                enc.Byte(0x03);
                            }
                            else if (ss == 2)
                            {
                                enc.Byte(0x66);
                                enc.Byte(0x41);
                                enc.Byte(0x89);
                                enc.Byte(0x03);
                            }
                            else
                            {
                                enc.Byte(0x41);
                                enc.Byte(0x88);
                                enc.Byte(0x03);
                            }
                        }
                        break;
                    }
                case LirOpcode::Add:
                case LirOpcode::Sub:
                case LirOpcode::And:
                case LirOpcode::Or:
                case LirOpcode::Xor:
                    {
                        const TypeRef& t = instr.type;
                        if (IsFloat(t))
                        {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            const bool f32 = (t.kind == TypeRef::Kind::Float32);
                            if (instr.op == LirOpcode::Add)
                            {
                                if (f32) enc.AddssXmm01();
                                else enc.AddsdXmm01();
                            }
                            else if (instr.op == LirOpcode::Sub)
                            {
                                if (f32) enc.SubssXmm01();
                                else enc.SubsdXmm01();
                            }
                            else
                            {
                                if (f32) enc.AddssXmm01();
                                else enc.AddsdXmm01();
                            } // bitwise on float: fallback
                            StoreA(instr.dst, t);
                        }
                        else
                        {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            if (instr.op == LirOpcode::Add) enc.AddRaxR10();
                            else if (instr.op == LirOpcode::Sub) enc.SubRaxR10();
                            else if (instr.op == LirOpcode::And) enc.AndRaxR10();
                            else if (instr.op == LirOpcode::Or) enc.OrRaxR10();
                            else enc.XorRaxR10();
                            StoreA(instr.dst, t);
                        }
                        break;
                    }
                case LirOpcode::Mul:
                    {
                        const TypeRef& t = instr.type;
                        if (IsFloat(t))
                        {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            if (t.kind == TypeRef::Kind::Float32) enc.MulssXmm01();
                            else enc.MulsdXmm01();
                        }
                        else
                        {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            enc.ImulRaxR10();
                        }
                        StoreA(instr.dst, t);
                        break;
                    }
                case LirOpcode::Div:
                case LirOpcode::Mod:
                    {
                        if (const TypeRef& t = instr.type; IsFloat(t))
                        {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            if (t.kind == TypeRef::Kind::Float32) enc.DivssXmm01();
                            else enc.DivsdXmm01();
                            StoreA(instr.dst, t);
                        }
                        else
                        {
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            if (t.IsSigned())
                            {
                                enc.Cqo();
                                enc.IdivR10();
                            }
                            else
                            {
                                enc.XorRdxRdx();
                                enc.DivR10();
                            }
                            if (instr.op == LirOpcode::Mod) enc.MovRaxRdx();
                            StoreA(instr.dst, t);
                        }
                        break;
                    }
                case LirOpcode::Pow:
                    {
                        const TypeRef& t = instr.type;
                        if (IsFloat(t))
                        {
                            uint32_t sym = GetOrAddExtern("pow", RcuSymKind::ExternFunc);
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            enc.MovssXmmNLoad(0, Disp(instr.srcs[0]));
                            enc.MovssXmmNLoad(1, Disp(instr.srcs[1]));
                            uint32_t ro;
                            enc.Call(ro);
                            AddTextReloc(ro, sym);
                        }
                        else
                        {
                            uint32_t sym = GetOrAddExtern("__rux_ipow", RcuSymKind::ExternFunc);
                            LoadA(instr.srcs[0], t);
                            LoadB(instr.srcs[1], t);
                            enc.MovArgLoad(0, Disp(instr.srcs[0]));
                            enc.MovArgLoad(1, Disp(instr.srcs[1]));
                            uint32_t ro;
                            enc.Call(ro);
                            AddTextReloc(ro, sym);
                        }
                        StoreA(instr.dst, t);
                        break;
                    }
                case LirOpcode::Shl:
                case LirOpcode::Shr:
                    {
                        const TypeRef& t = instr.type;
                        LoadA(instr.srcs[0], t);
                        enc.MovR11Load(Disp(instr.srcs[1]));
                        enc.MovRcxR11();
                        bool isShr = (instr.op == LirOpcode::Shr);
                        if (isShr && t.IsSigned()) enc.SarRaxCl();
                        else if (isShr) enc.ShrRaxCl();
                        else enc.ShlRaxCl();
                        StoreA(instr.dst, t);
                        break;
                    }

                case LirOpcode::Neg:
                    {
                        const TypeRef& t = instr.type;
                        if (IsFloat(t))
                        {
                            LoadA(instr.srcs[0], t);
                            const bool f32 = (t.kind == TypeRef::Kind::Float32);
                            const uint32_t maskSym = f32 ? InternF32SignMask() : InternF64SignMask();
                            uint32_t ro;
                            if (f32) enc.MovssXmm1Rip(ro);
                            else enc.MovsdXmm1Rip(ro);
                            AddTextReloc(ro, maskSym);
                            if (f32) enc.XorpsXmm01();
                            else enc.XorpdXmm01();
                            StoreA(instr.dst, t);
                        }
                        else
                        {
                            LoadA(instr.srcs[0], t);
                            enc.NegRax();
                            StoreA(instr.dst, t);
                        }
                        break;
                    }

                case LirOpcode::Not:
                    {
                        LoadA(instr.srcs[0], instr.type);
                        enc.TestRaxRax();
                        enc.SeteAl();
                        enc.MovzxRaxAl();
                        StoreA(instr.dst, TypeRef::MakeBool());
                        break;
                    }
                case LirOpcode::BitNot:
                    {
                        LoadA(instr.srcs[0], instr.type);
                        enc.NotRax();
                        StoreA(instr.dst, instr.type);
                        break;
                    }
                case LirOpcode::CmpEq:
                case LirOpcode::CmpNe:
                case LirOpcode::CmpLt:
                case LirOpcode::CmpLe:
                case LirOpcode::CmpGt:
                case LirOpcode::CmpGe:
                    {
                        const TypeRef& lhsT = regTypes.contains(instr.srcs[0])
                                                  ? regTypes.at(instr.srcs[0])
                                                  : instr.type;
                        LoadA(instr.srcs[0], lhsT);
                        LoadB(instr.srcs[1], lhsT);
                        if (IsFloat(lhsT))
                        {
                            if (lhsT.kind == TypeRef::Kind::Float32) enc.UcomissXmm01();
                            else enc.UcomisdXmm01();
                            switch (instr.op)
                            {
                            case LirOpcode::CmpEq: enc.SeteAl();
                                break;
                            case LirOpcode::CmpNe: enc.SetneAl();
                                break;
                            case LirOpcode::CmpLt: enc.SetbAl();
                                break;
                            case LirOpcode::CmpLe: enc.SetbeAl();
                                break;
                            case LirOpcode::CmpGt: enc.SetaAl();
                                break;
                            default: enc.SetaeAl();
                                break;
                            }
                        }
                        else
                        {
                            enc.CmpRaxR10();
                            bool sig = lhsT.IsSigned();
                            switch (instr.op)
                            {
                            case LirOpcode::CmpEq: enc.SeteAl();
                                break;
                            case LirOpcode::CmpNe: enc.SetneAl();
                                break;
                            case LirOpcode::CmpLt: sig ? enc.SetlAl() : enc.SetbAl();
                                break;
                            case LirOpcode::CmpLe: sig ? enc.SetleAl() : enc.SetbeAl();
                                break;
                            case LirOpcode::CmpGt: sig ? enc.SetgAl() : enc.SetaAl();
                                break;
                            default: sig ? enc.SetgeAl() : enc.SetaeAl();
                                break;
                            }
                        }
                        enc.MovzxRaxAl();
                        StoreA(instr.dst, TypeRef::MakeBool());
                        break;
                    }
                case LirOpcode::Cast:
                    {
                        const TypeRef& dstT = instr.type;
                        TypeRef srcT = regTypes.contains(instr.srcs[0])
                                           ? regTypes.at(instr.srcs[0])
                                           : dstT;
                        LoadA(instr.srcs[0], srcT);
                        bool srcFl = IsFloat(srcT), dstFl = IsFloat(dstT);
                        if (srcFl && !dstFl)
                        {
                            if (srcT.kind == TypeRef::Kind::Float32) enc.CvttsssiRaxXmm0();
                            else enc.CvttsdsiRaxXmm0();
                        }
                        else if (!srcFl && dstFl)
                        {
                            if (dstT.kind == TypeRef::Kind::Float32) enc.Cvtsi2ssXmm0Rax();
                            else enc.Cvtsi2sdXmm0Rax();
                        }
                        else if (srcFl && dstFl)
                        {
                            if (srcT.kind == TypeRef::Kind::Float32 && dstT.kind == TypeRef::Kind::Float64)
                                enc.CvtsssdXmm0();
                            else if (srcT.kind == TypeRef::Kind::Float64 && dstT.kind == TypeRef::Kind::Float32)
                                enc.CvtsdssXmm0();
                        }
                        StoreA(instr.dst, dstT);
                        break;
                    }
                case LirOpcode::Call:
                    {
                        bool win64Call = EffectiveConv(instr.callConv) == CallingConvention::Win64;
                        if (win64Call) enc.SubRspShadow();
                        EmitCallArgs(instr.srcs, instr.callConv);
                        uint32_t symIdx = GetOrAddExtern(instr.strArg, RcuSymKind::ExternFunc);
                        // If we already have a defined symbol for this name, use it
                        for (uint32_t i = 0; i < symbols.size(); ++i)
                        {
                            if (symbols[i].name == instr.strArg && symbols[i].kind == RcuSymKind::Func)
                            {
                                symIdx = i;
                                break;
                            }
                        }
                        uint32_t ro;
                        enc.Call(ro);
                        AddTextReloc(ro, symIdx);
                        if (win64Call) enc.AddRspShadow();
                        if (instr.dst != LirNoReg && !instr.type.IsOpaque())
                            StoreA(instr.dst, instr.type);
                        break;
                    }
                case LirOpcode::CallIndirect:
                    {
                        if (instr.srcs.empty()) break;
                        LirReg callee = instr.srcs[0];
                        std::vector<LirReg> args(instr.srcs.begin() + 1, instr.srcs.end());
                        enc.MovR10Load(Disp(callee));
                        EmitCallArgs(args, instr.callConv);
                        enc.CallR10();
                        if (instr.dst != LirNoReg && !instr.type.IsOpaque())
                            StoreA(instr.dst, instr.type);
                        break;
                    }
                case LirOpcode::FieldPtr:
                    {
                        LirReg base = instr.srcs[0];
                        enc.MovRaxLoad(Disp(base));
                        int off = FieldOffset(base, instr.strArg);
                        if (off != 0) enc.LeaRaxRaxDisp(off);
                        enc.MovRaxStore(Disp(instr.dst));
                        break;
                    }
                case LirOpcode::IndexPtr:
                    {
                        LirReg base = instr.srcs[0];
                        LirReg idx = instr.srcs[1];
                        int elemSz = (instr.type.kind == TypeRef::Kind::Pointer && !instr.type.inner.empty())
                                         ? SizeOfRuntime(instr.type.inner[0])
                                         : 8;
                        if (elemSz < 1) elemSz = 1;
                        enc.MovRaxLoad(Disp(base));
                        LoadB(idx, regTypes.at(idx));
                        enc.ImulR11R10Imm32(elemSz);
                        enc.AddRaxR11();
                        enc.MovRaxStore(Disp(instr.dst));
                        break;
                    }
                case LirOpcode::Phi:
                    break; // handled by phi-move pre-emission
                default:
                    break;
                }
            }

            // Terminator
            void GenTerm(uint32_t blockIdx, const LirTerminator& term, const LirFunc& func)
            {
                switch (term.kind)
                {
                case LirTermKind::Jump:
                    {
                        EmitPhiMoves(blockIdx, term.trueTarget);
                        uint32_t po;
                        enc.Jmp(po);
                        jumpPatches.push_back({po, term.trueTarget});
                        break;
                    }
                case LirTermKind::Branch:
                    {
                        // Load condition with correct width to avoid reading stack garbage
                        {
                            const TypeRef condT = regTypes.contains(term.cond)
                                                      ? regTypes.at(term.cond)
                                                      : TypeRef::MakeBool();
                            const int condSz = SizeOf(condT);
                            if (condSz <= 1) enc.MovzxRaxByte(Disp(term.cond));
                            else if (condSz == 2) enc.MovzxRaxWord(Disp(term.cond));
                            else if (condSz == 4) enc.MovEaxLoad(Disp(term.cond));
                            else enc.MovRaxLoad(Disp(term.cond));
                        }
                        enc.TestRaxRax();
                        const bool truePhi = HasPhiMoves(blockIdx, term.trueTarget);
                        if (const bool falsePhi = HasPhiMoves(blockIdx, term.falseTarget); !truePhi && !falsePhi)
                        {
                            uint32_t po;
                            enc.Jz(po);
                            jumpPatches.push_back({po, term.falseTarget});
                            uint32_t po2;
                            enc.Jmp(po2);
                            jumpPatches.push_back({po2, term.trueTarget});
                        }
                        else
                        {
                            uint32_t jzOff;
                            enc.Jz(jzOff);
                            // true trampoline
                            EmitPhiMoves(blockIdx, term.trueTarget);
                            uint32_t jmpTrue;
                            enc.Jmp(jmpTrue);
                            jumpPatches.push_back({jmpTrue, term.trueTarget});
                            // patch jz to here (false trampoline)
                            auto here = static_cast<int32_t>(enc.Size());
                            enc.Patch32(jzOff, here - static_cast<int32_t>(jzOff + 4));
                            EmitPhiMoves(blockIdx, term.falseTarget);
                            uint32_t jmpFalse;
                            enc.Jmp(jmpFalse);
                            jumpPatches.push_back({jmpFalse, term.falseTarget});
                        }
                        break;
                    }
                case LirTermKind::Return:
                    {
                        if (term.retVal && *term.retVal != LirNoReg)
                            LoadA(*term.retVal, term.retType);
                        enc.Leave();
                        enc.Ret();
                        break;
                    }
                case LirTermKind::Switch:
                    {
                        enc.MovRaxLoad(Disp(term.cond));
                        for (const auto& c : term.cases)
                        {
                            int64_t v = 0;
                            try { v = std::stoll(c.value); }
                            catch (...)
                            {
                            }
                            enc.CmpRaxImm32(static_cast<int32_t>(v));
                            uint32_t po;
                            enc.Je(po);
                            jumpPatches.push_back({po, c.target});
                        }
                        EmitPhiMoves(blockIdx, term.defaultTarget);
                        uint32_t po;
                        enc.Jmp(po);
                        jumpPatches.push_back({po, term.defaultTarget});
                        break;
                    }
                }
            }

            // Function generation
            void GenFunc(const LirFunc& func)
            {
                if (func.isExtern)
                {
                    GetOrAddExtern(func.name, RcuSymKind::ExternFunc, func.dll);
                    return;
                }
                PrepassFunc(func);
                jumpPatches.clear();
                uint32_t funcStart = enc.Size();
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
                enc.PushRbp();
                enc.MovRbpRsp();
                enc.SubRspImm32(frameSize);
                // Spill ABI param registers to stack slots
                bool win64Func = EffectiveConv(func.callConv) == CallingConvention::Win64;
                int intIdx = 0, fltIdx = 0, win64Idx = 0;
                for (const auto& p : func.params)
                {
                    int sz = SizeOf(p.type);
                    int32_t d = Disp(p.reg);
                    if (win64Func)
                    {
                        // Win64: unified index, max 4 register args
                        if (win64Idx >= 4) break;
                        if (IsWin64ByRefAggregate(p.type))
                        {
                            enc.MovR10ArgWin64(win64Idx);
                            enc.Byte(0x49);
                            enc.Byte(0x8B);
                            enc.Byte(0x02); // mov rax, [r10]
                            enc.MovRaxStore(d);
                            enc.Byte(0x49);
                            enc.Byte(0x8B);
                            enc.Byte(0x42);
                            enc.Byte(0x08); // mov rax, [r10 + 8]
                            enc.MovRaxStore(d + 8);
                        }
                        else if (IsFloat(p.type))
                        {
                            // MOVSS/MOVSD [rbp+d], xmmN
                            enc.Byte(sz == 4 ? 0xF3 : 0xF2);
                            enc.Byte(0x0F);
                            enc.Byte(0x11);
                            enc.Byte(static_cast<uint8_t>(0x80 | (win64Idx << 3) | 5));
                            enc.Dword(static_cast<uint32_t>(d));
                        }
                        else
                        {
                            enc.MovArgStoreWin64(win64Idx, d);
                        }
                        ++win64Idx;
                    }
                    else
                    {
                        if (IsFloat(p.type))
                        {
                            if (fltIdx < 8)
                            {
                                // MOVSS/MOVSD [rbp+d], xmmN
                                enc.Byte(sz == 4 ? 0xF3 : 0xF2);
                                enc.Byte(0x0F);
                                enc.Byte(0x11);
                                enc.Byte(static_cast<uint8_t>(0x80 | (fltIdx << 3) | 5));
                                enc.Dword(static_cast<uint32_t>(d));
                                ++fltIdx;
                            }
                        }
                        else
                        {
                            if (intIdx < 6)
                            {
                                enc.MovArgStore(intIdx, d);
                                ++intIdx;
                            }
                        }
                    }
                }
                // Basic blocks
                blockOffsets.assign(func.blocks.size(), 0);
                for (uint32_t bi = 0; bi < func.blocks.size(); ++bi)
                {
                    blockOffsets[bi] = enc.Size();
                    const auto& block = func.blocks[bi];
                    for (const auto& instr : block.instrs)
                        GenInstr(instr);
                    if (block.term)
                        GenTerm(bi, *block.term, func);
                }
                PatchJumps();
                // Update symbol size
                for (auto& s : symbols)
                {
                    if (s.name == func.name && s.sectionIdx == RCU_TEXT_IDX && s.value == funcStart)
                    {
                        s.size = enc.Size() - funcStart;
                        break;
                    }
                }
            }

            // Module generation
            void GenModule()
            {
                BuildLayouts();
                // Extern vars
                for (const auto& ev : mod.externVars)
                    GetOrAddExtern(ev.name, RcuSymKind::ExternData);
                // Module constants → .data symbols
                for (const auto& c : mod.consts)
                {
                    RcuSymbol s;
                    s.name = c.name;
                    s.sectionIdx = RCU_DATA_IDX;
                    s.value = static_cast<uint32_t>(dataData.size());
                    s.kind = RcuSymKind::Const;
                    s.visibility = c.isPublic ? RcuSymVis::Global : RcuSymVis::Local;
                    s.typeName = c.type.ToString();
                    // Emit 8 placeholder bytes in .data
                    for (int i = 0; i < 8; ++i) dataData.push_back(0);
                    s.size = 8;
                    AddSymbol(s);
                }
                // Functions
                for (const auto& func : mod.funcs)
                    GenFunc(func);
            }
        };

        // RcuCodeGen::Generate
        RcuFile RcuCodeGen::Generate()
        {
            GenModule();
            RcuFile file;
            file.sourcePath = mod.name;
            file.packageName = pkgName;
            file.buildTimestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            // Parse rux version from RUX_VERSION string "M.m.p"
            {
                std::string ver = RUX_VERSION;
                unsigned M = 0, mi = 0, p = 0;
                auto parseNum = [](const char* s, unsigned& out) -> const char*
                {
                    while (*s && (*s < '0' || *s > '9')) ++s;
                    while (*s >= '0' && *s <= '9')
                    {
                        out = out * 10 + static_cast<unsigned>(*s - '0');
                        ++s;
                    }
                    return s;
                };
                const char* c1 = parseNum(ver.c_str(), M);
                const char* c2 = parseNum(c1, mi);
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
                text.data = std::move(textData);
                text.relocs = std::move(textRelocs);
                file.sections.push_back(std::move(text));
            }
            {
                RcuSection rodata;
                rodata.name = ".rodata";
                rodata.type = RcuSecType::RoData;
                rodata.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
                rodata.alignment = 8;
                rodata.data = std::move(rodataData);
                rodata.relocs = std::move(rodataRelocs);
                file.sections.push_back(std::move(rodata));
            }
            {
                RcuSection data;
                data.name = ".data";
                data.type = RcuSecType::Data;
                data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
                data.alignment = 8;
                data.data = std::move(dataData);
                file.sections.push_back(std::move(data));
            }

            file.symbols = std::move(symbols);
            // Build string table offsets (intern all names into the file's string table)
            // (done during Emit/Dump)
            file.flags = 0x01; // F_HAS_METADATA
            file.hasMetadata = true;
            return file;
        }

        // CRC-32C (Castagnoli)
        uint32_t Crc32cTable[256];
        bool Crc32cReady = false;

        void InitCrc32c()
        {
            if (Crc32cReady) return;
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
                Crc32cTable[i] = c;
            }
            Crc32cReady = true;
        }

        uint32_t Crc32c(const std::vector<uint8_t>& data)
        {
            InitCrc32c();
            uint32_t crc = 0xFFFFFFFFu;
            for (uint8_t b : data) crc = Crc32cTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }

        // Binary writer
        class RcuWriter
        {
        public:
            static std::vector<uint8_t> Serialize(const RcuFile& f)
            {
                RcuWriter w(f);
                return w.Build();
            }

        private:
            const RcuFile& f_;
            RcuStringTable st_;

            explicit RcuWriter(const RcuFile& f) : f_(f)
            {
            }

            // Intern all strings first so offsets are stable
            void InternStrings()
            {
                st_.Intern(f_.sourcePath);
                st_.Intern(f_.packageName);
                for (const auto& s : f_.symbols)
                {
                    st_.Intern(s.name);
                    st_.Intern(s.typeName);
                }
                for (const auto& sec : f_.sections)
                    st_.Intern(sec.name);
            }

            static void AppendU8(std::vector<uint8_t>& buf, uint8_t v)
            {
                buf.push_back(v);
            }

            static void AppendU16(std::vector<uint8_t>& buf, uint16_t v)
            {
                buf.push_back(v & 0xFF);
                buf.push_back(v >> 8);
            }

            static void AppendU32(std::vector<uint8_t>& buf, uint32_t v)
            {
                for (int i = 0; i < 4; ++i)
                {
                    buf.push_back(v & 0xFF);
                    v >>= 8;
                }
            }

            static void AppendI32(std::vector<uint8_t>& buf, int32_t v)
            {
                AppendU32(buf, static_cast<uint32_t>(v));
            }

            static void AppendU64(std::vector<uint8_t>& buf, uint64_t v)
            {
                for (int i = 0; i < 8; ++i)
                {
                    buf.push_back(v & 0xFF);
                    v >>= 8;
                }
            }

            static void Patch32At(std::vector<uint8_t>& buf, uint32_t off, uint32_t v)
            {
                buf[off] = v & 0xFF;
                buf[off + 1] = (v >> 8) & 0xFF;
                buf[off + 2] = (v >> 16) & 0xFF;
                buf[off + 3] = v >> 24;
            }

            static void AlignTo(std::vector<uint8_t>& buf, int a)
            {
                while (buf.size() % a) buf.push_back(0);
            }

            std::vector<uint8_t> Build()
            {
                InternStrings();
                std::vector<uint8_t> out;
                out.reserve(1024);
                auto secCount = static_cast<uint16_t>(f_.sections.size());
                auto symCount = static_cast<uint32_t>(f_.symbols.size());
                // File Header (32 bytes)
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
                auto stOffPatch = static_cast<uint32_t>(out.size());
                AppendU32(out, 0);
                // [20-23] string_table_size (placeholder)
                auto stSizePatch = static_cast<uint32_t>(out.size());
                AppendU32(out, 0);
                // [24-27] metadata_offset (placeholder)
                auto metaOffPatch = static_cast<uint32_t>(out.size());
                AppendU32(out, 0);
                // [28-31] checksum (placeholder)
                auto checksumPatch = static_cast<uint32_t>(out.size());
                AppendU32(out, 0);
                // Section Table (secCount × 40 bytes)
                // We need to write reloc offsets later; track patch positions.
                std::vector<uint32_t> secRelocOffPatches(secCount);
                std::vector<uint32_t> secRawOffPatches(secCount);
                for (uint16_t i = 0; i < secCount; ++i)
                {
                    const auto& sec = f_.sections[i];
                    // name[8]
                    char name8[8] = {};
                    for (int j = 0; j < 7 && j < static_cast<int>(sec.name.size()); ++j)
                        name8[j] = sec.name[j];
                    for (char c : name8) AppendU8(out, static_cast<uint8_t>(c));
                    AppendU32(out, sec.type);
                    AppendU32(out, sec.flags);
                    secRawOffPatches[i] = static_cast<uint32_t>(out.size());
                    AppendU32(out, 0); // raw_offset placeholder
                    AppendU32(out, static_cast<uint32_t>(sec.data.size()));
                    AppendU32(out, static_cast<uint32_t>(std::max(sec.data.size(), static_cast<size_t>(1))));
                    // virtual_size
                    AppendU16(out, sec.alignment);
                    AppendU16(out, static_cast<uint16_t>(sec.relocs.size()));
                    secRelocOffPatches[i] = static_cast<uint32_t>(out.size());
                    AppendU32(out, 0); // reloc_offset placeholder
                    AppendU32(out, 0); // reserved
                }
                // Symbol Table (symCount × 20 bytes)
                for (const auto& sym : f_.symbols)
                {
                    AppendU32(out, st_.Intern(sym.name));
                    AppendU32(out, sym.value);
                    AppendU32(out, sym.size);
                    AppendU16(out, sym.sectionIdx);
                    AppendU8(out, sym.kind);
                    AppendU8(out, sym.visibility);
                    AppendU32(out, sym.typeName.empty() ? 0 : st_.Intern(sym.typeName));
                }
                // Section Data + Relocations
                for (uint16_t i = 0; i < secCount; ++i)
                {
                    const auto& sec = f_.sections[i];
                    // Align to section alignment
                    AlignTo(out, sec.alignment);
                    Patch32At(out, secRawOffPatches[i], static_cast<uint32_t>(out.size()));
                    // Raw data
                    out.insert(out.end(), sec.data.begin(), sec.data.end());
                    // Relocations (4-byte aligned)
                    if (!sec.relocs.empty())
                    {
                        AlignTo(out, 4);
                        Patch32At(out, secRelocOffPatches[i], static_cast<uint32_t>(out.size()));
                        for (const auto& r : sec.relocs)
                        {
                            AppendU32(out, r.sectionOffset);
                            AppendU32(out, r.symbolIndex);
                            AppendU16(out, r.type);
                            AppendU16(out, 0); // reserved
                            AppendI32(out, r.addend);
                        }
                    }
                }
                // String Table
                Patch32At(out, stOffPatch, static_cast<uint32_t>(out.size()));
                Patch32At(out, stSizePatch, st_.Size());
                const char* stData = st_.Data();
                for (uint32_t i = 0; i < st_.Size(); ++i) out.push_back(static_cast<uint8_t>(stData[i]));

                // Rux Metadata (64 bytes, 8-byte aligned)
                if (f_.hasMetadata)
                {
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
                    for (uint8_t b : f_.sourceHash) AppendU8(out, b);
                }

                // CRC-32C
                uint32_t crc = Crc32c(out);
                Patch32At(out, checksumPatch, crc);

                return out;
            }
        };

        // Text dumper
        class RcuDumper
        {
        public:
            static std::string Dump(const RcuFile& f)
            {
                std::ostringstream out;
                out << "; RCU  Rux Compiled Unit  v1.0\n";
                out << "; Architecture: x86-64 (Windows x64)\n";
                out << std::format("; Source:        {}\n", f.sourcePath.empty() ? "<unknown>" : f.sourcePath);
                out << std::format("; Package:       {}\n", f.packageName.empty() ? "<unknown>" : f.packageName);
                if (f.ruxVersion)
                {
                    out << std::format("; Rux version:   {}.{}.{}\n",
                                       f.ruxVersion >> 16, (f.ruxVersion >> 8) & 0xFF, f.ruxVersion & 0xFF);
                }
                out << '\n';

                // Sections
                out << std::format("Sections: {}\n", f.sections.size());
                for (size_t i = 0; i < f.sections.size(); ++i)
                {
                    const auto& s = f.sections[i];
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
                for (size_t i = 0; i < f.symbols.size(); ++i)
                {
                    const auto& s = f.symbols[i];
                    std::string secStr;
                    if (s.sectionIdx == RCU_SEC_EXTERNAL) secStr = "extern";
                    else if (s.sectionIdx == RCU_SEC_ABSOLUTE) secStr = "abs";
                    else if (s.sectionIdx < f.sections.size())
                        secStr = std::format("{}+0x{:04X}", f.sections[s.sectionIdx].name, s.value);
                    else secStr = std::format("sec{}+0x{:04X}", s.sectionIdx, s.value);

                    auto kindStr = "?";
                    switch (s.kind)
                    {
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
                    default: ;
                    }
                    const char* visStr = s.visibility == RcuSymVis::Global
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
                for (const auto& sec : f.sections)
                    if (!sec.relocs.empty())
                    {
                        anyReloc = true;
                        break;
                    }

                if (anyReloc)
                {
                    for (size_t si = 0; si < f.sections.size(); ++si)
                    {
                        const auto& sec = f.sections[si];
                        if (sec.relocs.empty()) continue;
                        out << std::format("Relocations ({}):\n", sec.name);
                        for (size_t i = 0; i < sec.relocs.size(); ++i)
                        {
                            const auto& r = sec.relocs[i];
                            const char* rt = "?";
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
                for (const auto& sec : f.sections)
                {
                    if (sec.data.empty()) continue;
                    out << std::format("{} ({} bytes):\n", sec.name, sec.data.size());
                    for (size_t i = 0; i < sec.data.size(); i += 16)
                    {
                        out << std::format("  {:04X}  ", i);
                        for (size_t j = 0; j < 16; ++j)
                        {
                            if (i + j < sec.data.size()) out << std::format("{:02X} ", sec.data[i + j]);
                            else out << "   ";
                            if (j == 7) out << ' ';
                        }
                        out << " |";
                        for (size_t j = 0; j < 16 && i + j < sec.data.size(); ++j)
                        {
                            unsigned char c = sec.data[i + j];
                            out << (c >= 32 && c < 127 ? static_cast<char>(c) : '.');
                        }
                        out << "|\n";
                    }
                    out << '\n';
                }

                return out.str();
            }
        };
    }

    // Public API
    Rcu::Rcu(LirPackage package, std::string packageName)
        : lir(std::move(package)), packageName(std::move(packageName))
    {
    }

    std::vector<RcuFile> Rcu::Generate() const
    {
        std::vector<RcuFile> result;
        result.reserve(lir.modules.size());
        for (const auto& mod : lir.modules)
        {
            RcuCodeGen gen(mod, packageName);
            result.push_back(gen.Generate());
        }
        return result;
    }

    bool Rcu::Emit(const RcuFile& file, const std::filesystem::path& path)
    {
        auto bytes = RcuWriter::Serialize(file);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        return f.good();
    }

    bool Rcu::Dump(const RcuFile& file, const std::filesystem::path& path)
    {
        std::string text = RcuDumper::Dump(file);
        std::ofstream f(path, std::ios::out | std::ios::trunc);
        if (!f) return false;
        f << text;
        return f.good();
    }
}

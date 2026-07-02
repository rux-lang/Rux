// ELF64 object writer for Linux, the BSDs, Solaris, and illumos.

#include "Backend/Link/Linker.h"
#include "Backend/Link/LinkerInternal.h"
#include "Platform/Platform.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Rux {

static std::optional<Buf> LinuxCompatThunk(const std::string &name) {
    static const std::unordered_map<std::string, Buf> thunks = {
        {"ExitProcess",
         {0x48, 0x89, 0xCF, 0xB8, RUX_IS_BSD || RUX_IS_SUNOS ? 0x01 : 0x3C, 0x00, 0x00, 0x00, 0x0F, 0x05}},
        {"GetStdHandle",
         {
             0x81, 0xF9, 0xF6, 0xFF, 0xFF,
             0xFF,       // cmp ecx, -10 (STD_INPUT_HANDLE)
             0x74, 0x0E, // je +14 (return 0)
             0x81, 0xF9, 0xF5, 0xFF, 0xFF,
             0xFF,                         // cmp ecx, -11 (STD_OUTPUT_HANDLE)
             0x74, 0x09,                   // je +9 (return 1)
             0xB8, 0x02, 0x00, 0x00, 0x00, // mov eax, 2
             0xC3,                         // ret
             0x31, 0xC0,                   // xor eax, eax
             0xC3,                         // ret
             0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
             0xC3,                         // ret
         }},
        {"GetProcessHeap", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}},
        {"HeapFree", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}},
        {"HeapAlloc", {0x4C, 0x89, 0xC6, 0x31, 0xFF, 0xBA, 0x03, 0x00, 0x00, 0x00, 0x41, 0xBA,

#if RUX_IS_BSD
                       0x02, 0x10, 0x00, 0x00,
#elif RUX_IS_SUNOS
                       0x02, 0x01, 0x00, 0x00,
#else
                       0x22, 0x00, 0x00, 0x00,
#endif
                       0x49, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0x45, 0x31, 0xC9,
#if RUX_OS_FREEBSD
                       0xB8, 0xDD, 0x01, 0x00, 0x00, 0x0F,
#elif RUX_OS_OPENBSD
                       0xB8, 0x31, 0x00, 0x00, 0x00, 0x0F,
#elif RUX_OS_DRAGONFLY || RUX_OS_NETBSD
                       0xB8, 0xC5, 0x00, 0x00, 0x00, 0x0F,
#elif RUX_IS_SUNOS
                       0xB8, 0x73, 0x00, 0x00, 0x00, 0x0F,
#else
                       0xB8, 0x09, 0x00, 0x00, 0x00, 0x0F,
#endif
                       0x05, 0xC3}},
        {"HeapReAlloc", {0x48, 0x8B, 0x74, 0x24, 0x28, 0x31, 0xFF, 0xBA, 0x03, 0x00, 0x00, 0x00, 0x41, 0xBA,
#if RUX_IS_BSD
                         0x02, 0x10, 0x00, 0x00,
#elif RUX_IS_SUNOS
                         0x02, 0x01, 0x00, 0x00,
#else
                         0x22, 0x00, 0x00, 0x00,
#endif
                         0x49, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0x45, 0x31, 0xC9,
#if RUX_OS_FREEBSD
                         0xB8, 0xDD, 0x01, 0x00, 0x00, 0x0F,
#elif RUX_OS_OPENBSD
                         0xB8, 0x31, 0x00, 0x00, 0x00, 0x0F,
#elif RUX_OS_DRAGONFLY || RUX_OS_NETBSD
                         0xB8, 0xC5, 0x00, 0x00, 0x00, 0x0F,
#elif RUX_IS_SUNOS
                         0xB8, 0x73, 0x00, 0x00, 0x00, 0x0F,
#else
                         0xB8, 0x09, 0x00, 0x00, 0x00, 0x0F,
#endif
                         0x05, 0xC3}},
        {"RtlCopyMemory", {0x4D, 0x85, 0xC0, 0x74, 0x0F, 0x8A, 0x02, 0x88, 0x01, 0x48, 0xFF,
                           0xC2, 0x48, 0xFF, 0xC1, 0x49, 0xFF, 0xC8, 0x75, 0xF1, 0xC3}},
        {"RtlCompareMemory",
         {
             0x49,
             0x85,
             0xC0, // test r8, r8
             0x74,
             0x2A, // jz done_zero

             0x48,
             0x31,
             0xC0, // xor rax, rax (result = 0)

             // --- align loop (16 bytes SIMD) ---
             0x49,
             0x83,
             0xF8,
             0x10, // cmp r8, 16
             0x72,
             0x1E, // jb byte_tail

             0xF3,
             0x0F,
             0x6F,
             0x01, // movdqu xmm0, [rcx]
             0xF3,
             0x0F,
             0x6F,
             0x12, // movdqu xmm1, [rdx]

             0x66,
             0x0F,
             0x74,
             0xC1, // pcmpeqb xmm0, xmm1
             0x66,
             0x0F,
             0xD7,
             0xC0, // pmovmskb eax, xmm0

             0x3D,
             0xFF,
             0xFF,
             0x00,
             0x00, // cmp eax, 0xFFFF
             0x75,
             0x1A, // jne mismatch

             0x48,
             0x83,
             0xC1,
             0x10, // rcx += 16
             0x48,
             0x83,
             0xC2,
             0x10, // rdx += 16
             0x48,
             0x83,
             0xC0,
             0x10, // rax += 16
             0x49,
             0x83,
             0xE8,
             0x10, // r8  -= 16

             0xEB,
             0xDA, // jmp loop
         }},
        {"RtlFillMemory",
         {0x48, 0x85, 0xD2, 0x74, 0x0B, 0x44, 0x88, 0x01, 0x48, 0xFF, 0xC1, 0x48, 0xFF, 0xCA, 0x75, 0xF5, 0xC3}},
        {"RtlZeroMemory", {0x45, 0x31, 0xC0, 0x48, 0x85, 0xD2, 0x74, 0x0B, 0x44, 0x88,
                           0x01, 0x48, 0xFF, 0xC1, 0x48, 0xFF, 0xCA, 0x75, 0xF5, 0xC3}},
        {"MultiByteToWideChar", {0x4C, 0x89, 0xC8, 0x4C, 0x8B, 0x54, 0x24, 0x28, 0x4D, 0x85, 0xD2, 0x74, 0x19,
                                 0x4D, 0x85, 0xC9, 0x7E, 0x14, 0x45, 0x0F, 0xB6, 0x18, 0x66, 0x45, 0x89, 0x1A,
                                 0x49, 0xFF, 0xC0, 0x49, 0x83, 0xC2, 0x02, 0x49, 0xFF, 0xC9, 0x75, 0xEC, 0xC3}},
        {"WriteConsoleW", {0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x08, 0x49, 0x89, 0xD4, 0x4D, 0x89,
                           0xC5, 0x4D, 0x85, 0xED, 0x74, 0x24, 0x41, 0x8A, 0x04, 0x24, 0x88, 0x04, 0x24,
#if RUX_IS_BSD || RUX_IS_SUNOS
                           0xB8, 0x04, 0x00, 0x00, 0x00, 0xBF,
#else
                           0xB8, 0x01, 0x00, 0x00, 0x00, 0xBF,
#endif
                           0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0xE6, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x0F,
                           0x05, 0x49, 0x83, 0xC4, 0x02, 0x49, 0xFF, 0xCD, 0xEB, 0xD7, 0x48, 0x83, 0xC4,
                           0x08, 0x41, 0x5D, 0x41, 0x5C, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}},
        {"ReadFile",
         {
             0x89,
             0xCF, // mov edi, ecx  (fd)
             0x48, 0x89,
             0xD6, // mov rsi, rdx  (buf)
             0x4C, 0x89,
             0xC2, // mov rdx, r8   (count)
#if RUX_IS_BSD || RUX_IS_SUNOS
             0xB8, 0x03, 0x00, 0x00,
             0x00, // mov eax, 3 (SYS_read)
#else
             0x31,
             0xC0, // xor eax, eax (SYS_read = 0)
#endif
             0x4D, 0x89,
             0xC8, // mov r8, r9  (save output pointer to r8
                   // before syscall)
             0x0F,
             0x05, // syscall
             0x85,
             0xC0, // test eax, eax
             0x78,
             0x11, // js +17 (error)
             0x4D, 0x89,
             0xC1, // mov r9, r8  (restore output pointer)
             0x4D, 0x85,
             0xC9, // test r9, r9
             0x74,
             0x03, // jz +3 (skip if null)
             0x41, 0x89,
             0x01, // mov [r9], eax  (*bytesRead = result)
             0xB8, 0x01, 0x00, 0x00,
             0x00, // mov eax, 1 (TRUE)
             0xC3, // ret
             0x31,
             0xC0, // xor eax, eax (FALSE)
             0xC3  // ret
         }},
        // WriteFile(handle, buf, count, *bytesWritten, overlapped) ->
        // write(fd, buf, count). Same shape as ReadFile; only the syscall
        // number differs.
        {"WriteFile",
         {
             0x89,
             0xCF, // mov edi, ecx  (fd)
             0x48, 0x89,
             0xD6, // mov rsi, rdx  (buf)
             0x4C, 0x89,
             0xC2, // mov rdx, r8   (count)
#if RUX_IS_BSD || RUX_IS_SUNOS
             0xB8, 0x04, 0x00, 0x00,
             0x00, // mov eax, 4 (SYS_write)
#else
             0xB8, 0x01, 0x00, 0x00,
             0x00, // mov eax, 1 (SYS_write)
#endif
             0x4D, 0x89,
             0xC8, // mov r8, r9  (save output pointer to r8
                   // before syscall)
             0x0F,
             0x05, // syscall
             0x85,
             0xC0, // test eax, eax
             0x78,
             0x11, // js +17 (error)
             0x4D, 0x89,
             0xC1, // mov r9, r8  (restore output pointer)
             0x4D, 0x85,
             0xC9, // test r9, r9
             0x74,
             0x03, // jz +3 (skip if null)
             0x41, 0x89,
             0x01, // mov [r9], eax  (*bytesWritten = result)
             0xB8, 0x01, 0x00, 0x00,
             0x00, // mov eax, 1 (TRUE)
             0xC3, // ret
             0x31,
             0xC0, // xor eax, eax (FALSE)
             0xC3  // ret
         }},
#if RUX_IS_ELF_OS
        // Rux extern calls currently use the Win64 register layout. These
        // thunks move that layout into Linux x86_64 syscall registers:
        // rax=number, rdi/rsi/rdx/r10/r8/r9=args.
        {"__rux_linux_syscall0",
         {
             0x48, 0x89,
             0xC8, // mov rax, rcx
             0x0F,
             0x05, // syscall
             0xC3  // ret
         }},
        {"__rux_linux_syscall1",
         {
             0x48, 0x89,
             0xC8, // mov rax, rcx
             0x48, 0x89,
             0xD7, // mov rdi, rdx
             0x0F,
             0x05, // syscall
             0xC3  // ret
         }},
        {"__rux_linux_syscall2",
         {
             0x48, 0x89,
             0xC8, // mov rax, rcx
             0x48, 0x89,
             0xD7, // mov rdi, rdx
             0x4C, 0x89,
             0xC6, // mov rsi, r8
             0x0F,
             0x05, // syscall
             0xC3  // ret
         }},
        {"__rux_linux_syscall3",
         {
             0x48, 0x89,
             0xC8, // mov rax, rcx
             0x48, 0x89,
             0xD7, // mov rdi, rdx
             0x4C, 0x89,
             0xC6, // mov rsi, r8
             0x4C, 0x89,
             0xCA, // mov rdx, r9
             0x0F,
             0x05, // syscall
             0xC3  // ret
         }},
        {"__rux_linux_syscall4",
         {
             0x48, 0x89, 0xC8,             // mov rax, rcx
             0x48, 0x89, 0xD7,             // mov rdi, rdx
             0x4C, 0x89, 0xC6,             // mov rsi, r8
             0x4C, 0x89, 0xCA,             // mov rdx, r9
             0x4C, 0x8B, 0x54, 0x24, 0x28, // mov r10, [rsp + 40]
             0x0F, 0x05,                   // syscall
             0xC3                          // ret
         }},
        {"__rux_linux_syscall5",
         {
             0x48, 0x89, 0xC8,             // mov rax, rcx
             0x48, 0x89, 0xD7,             // mov rdi, rdx
             0x4C, 0x89, 0xC6,             // mov rsi, r8
             0x4C, 0x89, 0xCA,             // mov rdx, r9
             0x4C, 0x8B, 0x54, 0x24, 0x28, // mov r10, [rsp + 40]
             0x4C, 0x8B, 0x44, 0x24, 0x30, // mov r8, [rsp + 48]
             0x0F, 0x05,                   // syscall
             0xC3                          // ret
         }},
        {"__rux_linux_syscall6",
         {
             0x48, 0x89, 0xC8,             // mov rax, rcx
             0x48, 0x89, 0xD7,             // mov rdi, rdx
             0x4C, 0x89, 0xC6,             // mov rsi, r8
             0x4C, 0x89, 0xCA,             // mov rdx, r9
             0x4C, 0x8B, 0x54, 0x24, 0x28, // mov r10, [rsp + 40]
             0x4C, 0x8B, 0x44, 0x24, 0x30, // mov r8, [rsp + 48]
             0x4C, 0x8B, 0x4C, 0x24, 0x38, // mov r9, [rsp + 56]
             0x0F, 0x05,                   // syscall
             0xC3                          // ret
         }},
        {"__rux_linux_nanosleep",
         {
             0x48, 0xC7, 0xC0, 0x23, 0x00, 0x00,
             0x00, // mov rax, 35
             0x48, 0x89,
             0xCF, // mov rdi, rcx
             0x48, 0x89,
             0xD6, // mov rsi, rdx
             0x0F,
             0x05, // syscall
             0xC3  // ret
         }},
        {"__rux_linux_clock_gettime",
         {
             0x48, 0xC7, 0xC0, 0xE4, 0x00, 0x00,
             0x00, // mov rax, 228
             0x48, 0x63,
             0xF9, // movsxd rdi, ecx
             0x48, 0x89,
             0xD6, // mov rsi, rdx
             0x0F,
             0x05, // syscall
             0xC3  // ret
         }},
        {"__rux_bsd_nanosleep",
         {
    #if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
             0x48, 0xC7, 0xC0, 0xF0, 0x00, 0x00,
             0x00, // mov rax, 240
    #elif defined(__OpenBSD__)
             0x48, 0xC7, 0xC0, 0x5B, 0x00, 0x00,
             0x00, // mov rax, 91
    #endif
             0x48, 0x89,
             0xCF, // mov rdi, rcx
             0x48, 0x89,
             0xD6, // mov rsi, rdx
             0x0F,
             0x05, // syscall
             0xC3  // ret
         }},
        {"__rux_bsd_clock_gettime",
         {
    #if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
             0x48, 0xC7, 0xC0, 0xE8, 0x00, 0x00,
             0x00, // mov rax, 232
    #elif defined(__OpenBSD__)
             0x48, 0xC7, 0xC0, 0x57, 0x00, 0x00,
             0x00, // mov rax, 87
    #endif
             0x48, 0x63,
             0xF9, // movsxd rdi, ecx
             0x48, 0x89,
             0xD6, // mov rsi, rdx
             0x0F,
             0x05, // syscall
             0xC3  // ret
         }},
        {"__rux_bsd_mmap",
         {
    #if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
             0x48, 0xC7, 0xC0, 0xDD, 0x01, 0x00, 0x00, // mov rax, 477
    #elif defined(__OpenBSD__)
             0x48, 0xC7, 0xC0, 0xC5, 0x00, 0x00, 0x00, // mov rax, 197
    #endif
             0x48, 0x89, 0xCF,             // mov rdi, rcx
             0x48, 0x89, 0xD6,             // mov rsi, rdx
             0x4C, 0x89, 0xC2,             // mov rdx, r8
             0x4D, 0x89, 0xCA,             // mov r10, r9
             0x4C, 0x8B, 0x44, 0x24, 0x28, // mov r8, [rsp + 40]
             0x4C, 0x8B, 0x4C, 0x24, 0x30, // mov r9, [rsp + 48]
             0x0F, 0x05,                   // syscall
             0xC3                          // ret
         }},
        {"__rux_bsd_const_MAP_ANONYMOUS",
         {
    #if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
             0xB8, 0x00, 0x10, 0x00,
             0x00, // mov eax, 4096
    #elif defined(__OpenBSD__)
             0xB8, 0x20, 0x00, 0x00,
             0x00, // mov eax, 32
    #endif
             0xC3 // ret
         }},
        {"__rux_bsd_const_CLOCK_MONOTONIC",
         {
    #if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
             0xB8, 0x04, 0x00, 0x00,
             0x00, // mov eax, 4
    #elif defined(__OpenBSD__)
             0xB8, 0x03, 0x00, 0x00,
             0x00, // mov eax, 3
    #endif
             0xC3 // ret
         }},
#endif
    };

    const auto it = thunks.find(name);
    if (it == thunks.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool Linker::LinkElf64(const std::filesystem::path &outputPath) {
    static constexpr uint64_t kBase = 0x400000;
    static constexpr uint64_t kPage = 0x1000;
    static constexpr uint32_t kPfX = 0x1;
    static constexpr uint32_t kPfW = 0x2;
    static constexpr uint32_t kPfR = 0x4;

    const auto alignUp64 = [](const uint64_t v, const uint64_t a) { return (v + a - 1) & ~(a - 1); };

    std::unordered_set<std::string> definedSymbols;
    std::unordered_set<std::string> linuxCompatExterns;
    for (const auto &obj : objects) {
        for (const auto &sym : obj.symbols) {
            if (sym.kind != RcuSymKind::ExternFunc && sym.kind != RcuSymKind::ExternData && !sym.name.empty()) {
                definedSymbols.insert(sym.name);
            }
        }
    }

    for (const auto &obj : objects) {
        for (const auto &sec : obj.sections) {
            for (const auto &reloc : sec.relocs) {
                if (reloc.symbolIndex >= obj.symbols.size()) {
                    continue;
                }
                const auto &sym = obj.symbols[reloc.symbolIndex];
                if ((sym.kind == RcuSymKind::ExternFunc || sym.kind == RcuSymKind::ExternData) &&
                    !definedSymbols.contains(sym.name)) {
                    if (LinuxCompatThunk(sym.name)) {
                        linuxCompatExterns.insert(sym.name);
                    }
                    else {
                        Error("external symbol '" + sym.name + "' is not supported by the ELF linker yet");
                    }
                }
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }

    Buf textPre;
    textPre.insert(textPre.end(), {0x48, 0x83, 0xE4, 0xF0}); // and rsp, -16 (align stack)
    textPre.insert(textPre.end(), {0x48, 0x83, 0xEC, 0x08}); // sub rsp, 8 (16-byte align after call)
    const size_t kCallMainDisp = textPre.size() + 1;
    textPre.insert(textPre.end(), {0xE8, 0x00, 0x00, 0x00, 0x00}); // call Main
    textPre.insert(textPre.end(), {0x48, 0x83, 0xC4, 0x08});       // add rsp, 8 (undo sub)
    textPre.insert(textPre.end(), {0x89, 0xC7});                   // mov edi, eax
#if RUX_IS_BSD || RUX_IS_SUNOS
    textPre.insert(textPre.end(), {0xB8, 0x01, 0x00, 0x00, 0x00}); // mov eax, 1  (BSD/Illumos exit)
#else
    textPre.insert(textPre.end(), {0xB8, 0x3C, 0x00, 0x00, 0x00}); // mov eax, 60 (Linux exit)
#endif
    textPre.insert(textPre.end(), {0x0F, 0x05}); // syscall

    std::unordered_map<std::string, uint32_t> linuxCompatThunkOff;
    std::vector<std::string> linuxCompatNames(linuxCompatExterns.begin(), linuxCompatExterns.end());
    std::sort(linuxCompatNames.begin(), linuxCompatNames.end());
    for (const auto &name : linuxCompatNames) {
        auto thunk = LinuxCompatThunk(name);
        if (!thunk) {
            continue;
        }
        linuxCompatThunkOff[name] = static_cast<uint32_t>(textPre.size());
        textPre.insert(textPre.end(), thunk->begin(), thunk->end());
    }
    const uint32_t preambleSize = static_cast<uint32_t>(textPre.size());

    struct ObjLayout {
        uint32_t textOff, rodataOff, dataOff;
    };

    std::vector<ObjLayout> layouts(objects.size());
    Buf mergedText, mergedRodata, mergedData;

#if RUX_OS_NETBSD
    // Prepend NetBSD ELF Note
    mergedRodata.insert(mergedRodata.end(), {
                                                0x07, 0x00, 0x00, 0x00,                 // Name size (7)
                                                0x04, 0x00, 0x00, 0x00,                 // Desc size (4)
                                                0x01, 0x00, 0x00, 0x00,                 // Type (1 = OS version)
                                                'N',  'e',  't',  'B',  'S', 'D', 0, 0, // Name (NetBSD\0\0)
                                                0x00, 0xCA, 0x9A, 0x3B                  // Desc (1000000000)
                                            });
#elif defined(__OpenBSD__)
    // Prepend OpenBSD ELF Note (required for execve to accept the binary)
    mergedRodata.insert(mergedRodata.end(), {
                                                0x08, 0x00, 0x00, 0x00, // Name size (7 + null, padded to 8)
                                                0x04, 0x00, 0x00, 0x00, // Desc size (4)
                                                0x01, 0x00, 0x00, 0x00, // Type (NT_OPENBSD_IDENT)
                                                'O',  'p',  'e',  'n',  'B', 'S', 'D', 0, // Name (OpenBSD\0)
                                                0x00, 0x00, 0x00, 0x00                    // Desc (0 = any version)
                                            });
#elif defined(__DragonFly__)
    // Prepend DragonFly ELF Note (required for execve to accept the binary)
    mergedRodata.insert(mergedRodata.end(),
                        {
                            0x0A, 0x00, 0x00, 0x00, // Name size (9 + null, padded to 12)
                            0x04, 0x00, 0x00, 0x00, // Desc size (4)
                            0x01, 0x00, 0x00, 0x00, // Type
                            'D',  'r',  'a',  'g',  'o', 'n', 'F', 'l', 'y', 0, 0, 0, // Name (DragonFly\0\0\0)
                            0x00, 0x00, 0x00, 0x00                                    // Desc
                        });
#endif

    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        layouts[i] = {static_cast<uint32_t>(mergedText.size()), static_cast<uint32_t>(mergedRodata.size()),
                      static_cast<uint32_t>(mergedData.size())};
        for (const auto &sec : obj.sections) {
            if (sec.type == RcuSecType::Text) {
                mergedText.insert(mergedText.end(), sec.data.begin(), sec.data.end());
            }
            else if (sec.type == RcuSecType::RoData) {
                mergedRodata.insert(mergedRodata.end(), sec.data.begin(), sec.data.end());
            }
            else if (sec.type == RcuSecType::Data) {
                mergedData.insert(mergedData.end(), sec.data.begin(), sec.data.end());
            }
        }
    }

    Buf textBuf;
    textBuf.insert(textBuf.end(), textPre.begin(), textPre.end());
    textBuf.insert(textBuf.end(), mergedText.begin(), mergedText.end());

    const uint16_t phnum = static_cast<uint16_t>(2 + (!mergedData.empty() ? 1 : 0)
#if RUX_OS_NETBSD || RUX_OS_OPENBSD || RUX_OS_DRAGONFLY
                                                 + 1 // PT_NOTE
#endif

    );
    const uint64_t phoff = 64;
    const uint64_t textOff = alignUp64(phoff + static_cast<uint64_t>(phnum) * 56, kPage);
    const uint64_t textVA = kBase + textOff;
    const uint64_t rdataOff = alignUp64(textOff + textBuf.size(), kPage);
    const uint64_t rdataVA = kBase + rdataOff;
    const uint64_t dataOff = alignUp64(rdataOff + mergedRodata.size(), kPage);
    const uint64_t dataVA = kBase + dataOff;

    std::unordered_map<std::string, uint64_t> symMap;
    for (const auto &[name, off] : linuxCompatThunkOff) {
        symMap[name] = textVA + off;
    }

    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        const auto &lay = layouts[i];
        for (const auto &sym : obj.symbols) {
            if (sym.name.empty()) {
                continue;
            }
            if (sym.kind == RcuSymKind::ExternFunc || sym.kind == RcuSymKind::ExternData) {
                continue;
            }
            if (sym.visibility == RcuSymVis::Local && sym.kind != RcuSymKind::Func && sym.name != "Main") {
                continue;
            }

            uint64_t va = 0;
            if (sym.sectionIdx == RCU_TEXT_IDX) {
                va = textVA + preambleSize + lay.textOff + sym.value;
            }
            else if (sym.sectionIdx == RCU_RODATA_IDX) {
                va = rdataVA + lay.rodataOff + sym.value;
            }
            else if (sym.sectionIdx == RCU_DATA_IDX) {
                va = dataVA + lay.dataOff + sym.value;
            }
            else {
                continue;
            }
            symMap.try_emplace(sym.name, va);
        }
    }

    {
        auto it = symMap.find("Main");
        if (it == symMap.end()) {
            Error("undefined symbol 'Main' — no entry point found");
            return false;
        }
        const uint64_t nextInst = textVA + kCallMainDisp + 4;
        Patch32(textBuf, kCallMainDisp, static_cast<uint32_t>(it->second - nextInst));
    }

    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        const auto &lay = layouts[i];
        for (const auto &sec : obj.sections) {
            Buf *buf = nullptr;
            uint32_t baseInBuf = 0;
            uint64_t secBaseVA = 0;
            if (sec.type == RcuSecType::Text) {
                buf = &textBuf;
                baseInBuf = preambleSize + lay.textOff;
                secBaseVA = textVA + preambleSize + lay.textOff;
            }
            else if (sec.type == RcuSecType::RoData) {
                buf = &mergedRodata;
                baseInBuf = lay.rodataOff;
                secBaseVA = rdataVA + lay.rodataOff;
            }
            else if (sec.type == RcuSecType::Data) {
                buf = &mergedData;
                baseInBuf = lay.dataOff;
                secBaseVA = dataVA + lay.dataOff;
            }
            else {
                continue;
            }

            for (const auto &reloc : sec.relocs) {
                if (reloc.symbolIndex >= obj.symbols.size()) {
                    continue;
                }
                const auto &sym = obj.symbols[reloc.symbolIndex];
                uint64_t targetVA = 0;
                if (sym.kind == RcuSymKind::ExternFunc || sym.kind == RcuSymKind::ExternData) {
                    auto it = symMap.find(sym.name);
                    if (it == symMap.end()) {
                        Error("undefined external symbol '" + sym.name + "'");
                        continue;
                    }
                    targetVA = it->second;
                }
                else if (sym.visibility != RcuSymVis::Local && !sym.name.empty() && symMap.contains(sym.name)) {
                    targetVA = symMap[sym.name];
                }
                else if (sym.sectionIdx == RCU_TEXT_IDX) {
                    targetVA = textVA + preambleSize + lay.textOff + sym.value;
                }
                else if (sym.sectionIdx == RCU_RODATA_IDX) {
                    targetVA = rdataVA + lay.rodataOff + sym.value;
                }
                else if (sym.sectionIdx == RCU_DATA_IDX) {
                    targetVA = dataVA + lay.dataOff + sym.value;
                }
                else {
                    continue;
                }

                const size_t patchAt = baseInBuf + reloc.sectionOffset;
                const uint64_t siteVA = secBaseVA + reloc.sectionOffset;
                if (reloc.type == RcuRelType::Rel32) {
                    if (patchAt + 4 > buf->size()) {
                        continue;
                    }
                    const int32_t disp = static_cast<int32_t>(targetVA + reloc.addend - (siteVA + 4));
                    Patch32(*buf, patchAt, static_cast<uint32_t>(disp));
                }
                else if (reloc.type == RcuRelType::Abs64) {
                    if (patchAt + 8 > buf->size()) {
                        continue;
                    }
                    Patch64(*buf, patchAt, targetVA + static_cast<uint64_t>(reloc.addend));
                }
                else if (reloc.type == RcuRelType::Abs32) {
                    if (patchAt + 4 > buf->size()) {
                        continue;
                    }
                    Patch32(*buf, patchAt, static_cast<uint32_t>(targetVA + reloc.addend));
                }
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }

    std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        Error("cannot open output file: " + outputPath.string());
        return false;
    }

    const auto writeRaw = [&](const void *d, size_t n) {
        out.write(static_cast<const char *>(d), static_cast<std::streamsize>(n));
    };
    [[maybe_unused]] const auto wU8 = [&](uint8_t v) { writeRaw(&v, 1); };
    const auto wU16 = [&](uint16_t v) { writeRaw(&v, 2); };
    const auto wU32 = [&](uint32_t v) { writeRaw(&v, 4); };
    const auto wU64 = [&](uint64_t v) { writeRaw(&v, 8); };
    const auto wBuf = [&](const Buf &b) {
        if (!b.empty()) {
            writeRaw(b.data(), b.size());
        }
    };
    const auto padToOffset = [&](uint64_t offset) {
        static constexpr uint8_t zeros[4096] = {};
        while (static_cast<uint64_t>(out.tellp()) < offset) {
            const uint64_t remaining = offset - static_cast<uint64_t>(out.tellp());
            writeRaw(zeros, static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(zeros))));
        }
    };
    const auto writePhdr = [&](uint32_t flags, uint64_t off, uint64_t vaddr, uint64_t fileSize, uint64_t memSize) {
        wU32(1); // PT_LOAD
        wU32(flags);
        wU64(off);
        wU64(vaddr);
        wU64(vaddr);
        wU64(fileSize);
        wU64(memSize);
        wU64(kPage);
    };

    uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1,
#if RUX_OS_FREEBSD
                         9, // EI_OSABI: FreeBSD
#elif RUX_OS_DRAGONFLY
                         0, // EI_OSABI: System V
#elif RUX_OS_OPENBSD
                         12, // EI_OSABI: OpenBSD
#elif RUX_OS_NETBSD
                         2, // EI_OSABI: NetBSD
#elif RUX_IS_SUNOS
                         6, // EI_OSABI: Solaris/Illumos
#else
                         0, // EI_OSABI: System V
#endif
                         0,    0,   0,   0,   0, 0, 0, 0};
    writeRaw(ident, sizeof(ident));
    wU16(2);    // ET_EXEC
    wU16(0x3E); // EM_X86_64
    wU32(1);
    wU64(textVA); // e_entry
    wU64(phoff);
    wU64(0);
    wU32(0);
    wU16(64);
    wU16(56);
    wU16(phnum);
    wU16(0);
    wU16(0);
    wU16(0);

    writePhdr(kPfR | kPfX, textOff, textVA, textBuf.size(), textBuf.size());
    writePhdr(kPfR, rdataOff, rdataVA, mergedRodata.size(), mergedRodata.size());
#if RUX_OS_NETBSD || RUX_OS_OPENBSD || RUX_OS_DRAGONFLY
    // Write PT_NOTE program header pointing to the OS note at the start of
    // .rodata
    wU32(4);        // p_type: PT_NOTE
    wU32(kPfR);     // p_flags: PF_R
    wU64(rdataOff); // p_offset
    wU64(rdataVA);  // p_vaddr
    wU64(rdataVA);  // p_paddr
    wU64(24);       // p_filesz: 24 bytes
    wU64(24);       // p_memsz: 24 bytes
    wU64(4);        // p_align: 4 bytes
#endif
    if (!mergedData.empty()) {
        writePhdr(kPfR | kPfW, dataOff, dataVA, mergedData.size(), mergedData.size());
    }

    padToOffset(textOff);
    wBuf(textBuf);
    padToOffset(rdataOff);
    wBuf(mergedRodata);
    if (!mergedData.empty()) {
        padToOffset(dataOff);
        wBuf(mergedData);
    }

    out.close();
    if (!out) {
        Error("cannot write output file: " + outputPath.string());
        return false;
    }

    std::error_code ec;
    std::filesystem::permissions(outputPath,
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
    if (ec) {
        Error("cannot mark output executable: " + ec.message());
        return false;
    }
    return true;
}

} // namespace Rux

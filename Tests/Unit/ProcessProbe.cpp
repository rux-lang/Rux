#include "ProcessProbe.h"

#include "System/WinApi.h"
#include "Target/Platform.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#if !RUX_OS_WINDOWS
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace Rux::Testing {
namespace {
bool WriteProbeBytes(const bool standardError, std::string_view bytes) {
    while (!bytes.empty()) {
#if RUX_OS_WINDOWS
        DWORD written = 0;
        if (!WriteFile(GetStdHandle(standardError ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE), bytes.data(),
                       static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written == 0)
            return false;
#else
        const ssize_t written = write(standardError ? STDERR_FILENO : STDOUT_FILENO, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
#endif
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}
} // namespace

std::optional<int> RunProcessProbe(const int argc, char **argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--rux-abrupt-process-probe") {
        const std::string_view marker(argv[2]);
        const std::string_view mode(argv[3]);
        const std::size_t size = mode == "large" ? 128 * 1024 : mode == "small" ? 17 : 0;
        // Native writes report failure through the exit code, independently of what the parent captures. _Exit
        // bypasses stdio flushing and destructors, so the capture cannot depend on normal child shutdown.
        const std::string prefix = "begin:" + std::string(marker) + '\n';
        constexpr std::string_view binary("\0\xff\r\n", 4);
        const std::string payload(size, 'x');
        const std::string suffix = "end:" + std::string(marker) + '\n';
        if (!WriteProbeBytes(false, prefix) || !WriteProbeBytes(true, binary) || !WriteProbeBytes(false, payload) ||
            !WriteProbeBytes(true, suffix))
            std::_Exit(98);
        std::_Exit(23);
    }
    if (argc > 1 && std::string_view(argv[1]) == "--rux-process-probe") {
        if (std::getchar() != EOF)
            return 98;
        for (int index = 2; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            std::fprintf(stdout, "%zu:", argument.size());
            std::fwrite(argument.data(), 1, argument.size(), stdout);
            std::fflush(stdout);
            std::fputs(";", stderr);
            std::fflush(stderr);
        }
        // More than a pipe's capacity, so capture must drain while the child is running.
        const std::string payload(128 * 1024, 'x');
        std::fwrite(payload.data(), 1, payload.size(), stdout);
        return 7;
    }
#if RUX_OS_WINDOWS
    if (argc == 3 && std::string_view(argv[1]) == "--rux-handle-probe") {
        // A valid handle value proves nothing on its own: Windows reuses freed slots, so a handle this process created
        // while starting up can carry the parent's number. Only the token the parent wrote identifies its pipe.
        const auto value = static_cast<std::uintptr_t>(std::strtoull(argv[2], nullptr, 10));
        char buffer[64] = {};
        DWORD count = 0;
        if (!PeekNamedPipe(reinterpret_cast<HANDLE>(value), buffer, sizeof(buffer), &count, nullptr, nullptr))
            return 0;
        return std::string_view(buffer, count) == handleProbeToken ? 1 : 0;
    }
#else
    if (argc == 4 && std::string_view(argv[1]) == "--rux-descriptor-probe") {
        // Exit code 2 reports a lost standard stream, 1 an inherited descriptor, and 0 the expected isolation. A
        // descriptor number proves nothing on its own, since this process reuses freed numbers; the inode identifies
        // the parent's pipe.
        if (fcntl(STDOUT_FILENO, F_GETFD) < 0 || fcntl(STDERR_FILENO, F_GETFD) < 0)
            return 2;
        const int descriptor = std::atoi(argv[2]);
        const auto inode = std::strtoull(argv[3], nullptr, 10);
        struct stat status{};
        return fstat(descriptor, &status) == 0 && static_cast<unsigned long long>(status.st_ino) == inode ? 1 : 0;
    }
#endif
    return std::nullopt;
}
} // namespace Rux::Testing

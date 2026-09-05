#include "ProcessProbe.h"

#include "System/WinApi.h"
#include "Target/Platform.h"

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
std::optional<int> RunProcessProbe(const int argc, char **argv) {
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

#include "Rux/Cli/Cli.h"

#include "Rux/Asm.h"
#include "Rux/Ast.h"
#include "Rux/Cli/CliInternals.h"
#include "Rux/Hir.h"
#include "Rux/Lexer.h"
#include "Rux/Linker.h"
#include "Rux/Lir.h"
#include "Rux/Manifest.h"
#include "Rux/Package.h"
#include "Rux/Parser.h"
#include "Rux/Platform/Defines.h"
#include "Rux/Platform/Host.h"
#include "Rux/Rcu.h"
#include "Rux/Sema.h"
#include "Rux/Version.h"

#include <string_view>

using namespace Rux;

void Cli::PrintHelp() {
    std::print("Rux compiler and package manager\n"
               "\n"
               "Usage: rux [command] [options] [-- args...]\n"
               "\n"
               "Commands:\n"
               "  add            Add a dependency to the manifest\n"
               "  build          Build the current package\n"
               "  clean          Remove build artifacts\n"
               "  doc            Generate package documentation\n"
               "  fmt            Format source files and manifests\n"
               "  help           Show help information\n"
               "  init           Initialize a Rux package in the current directory\n"
               "  install        Install dependencies\n"
               "  list           List dependencies\n"
               "  new            Create a new Rux package\n"
               "  remove         Remove a dependency from the manifest\n"
               "  run            Build and run the main executable\n"
               "  test           Run all test targets\n"
               "  uninstall      Uninstall dependencies\n"
               "  update         Update dependencies\n"
               "  version        Show version information\n"
               "  info           Show package metadata and manifest information.\n"
               "  check          Check package source code for errors without building\n"
               "\n"
               "Options:\n"
               "  --color <auto|on|off>  Control colored output\n"
               "  -h, --help             Show help information\n"
               "  -q, --quiet            Do not show log messages\n"
               "  -v, --verbose          Use verbose output\n"
               "  -V, --version          Show version information\n"
               "\n"
               "Use 'rux help <command>' for more information about a command.\n"
    );
}

void Cli::PrintHelpFor(std::string_view command) {
    if (command == "add") {
        PrintHelpAdd();
        return;
    }
    if (command == "build") {
        PrintHelpBuild();
        return;
    }
    if (command == "clean") {
        PrintHelpClean();
        return;
    }
    if (command == "doc") {
        PrintHelpDoc();
        return;
    }
    if (command == "fmt") {
        PrintHelpFmt();
        return;
    }
    if (command == "help") {
        PrintHelp();
        return;
    }
    if (command == "init") {
        PrintHelpInit();
        return;
    }
    if (command == "install") {
        PrintHelpInstall();
        return;
    }
    if (command == "uninstall") {
        PrintHelpUninstall();
        return;
    }
    if (command == "list") {
        PrintHelpList();
        return;
    }
    if (command == "new") {
        PrintHelpNew();
        return;
    }
    if (command == "remove") {
        PrintHelpRemove();
        return;
    }
    if (command == "run") {
        PrintHelpRun();
        return;
    }
    if (command == "test") {
        PrintHelpTest();
        return;
    }
    if (command == "update") {
        PrintHelpUpdate();
        return;
    }
    if (command == "info") {
        PrintHelpInfo();
        return;
    }
    if (command == "check") {
        PrintHelpCheck();
        return;
    }
    if (command == "version") {
        PrintHelpVersion();
        return;
    }
    PrintUnknownCommand(command);
}

void Cli::PrintHelpAdd() {
    std::print("Add a dependency to the current package\n"
               "\n"
               "Usage: rux add [package]\n"
               "       rux add [package]@[version]\n"
               "       rux add [package] --path [path]\n"
               "\n"
               "Options:\n"
               "  --path <path>        Add a local path-based dependency\n"
               "\n"
               "This command updates Rux.toml accordingly.\n"
               "\n"
               "Examples:\n"
               "  rux add Std\n"
               "  rux add Std@0.1.0\n"
               "  rux add Json --path ../Json\n");
}

void Cli::PrintHelpBuild() {
    std::print("Build the current package\n"
               "\n"
               "Usage: rux build [options]\n"
               "\n"
               "Options:\n"
               "  --debug              Build with debug symbols (unoptimized output)\n"
               "  --profile <n>        Build using a custom profile defined in Rux.toml\n"
               "  --release            Build with release profile (optimized, no debug info)\n"
               "  --stats              Print build timing, source, performance, and output statistics\n"
               "  --target <triple>    Build for the specified target platform (e.g. x86, x64)\n"
               "  -q, --quiet          Suppress non-essential output (only errors are shown)\n"
               "  -v, --verbose        Enable verbose output for detailed build information\n"
               "  --dump-asm           Write x86-64 assembly to Temp/Asm/out.asm\n"
               "  --dump-ast           Write the parsed AST to Temp/Ast/<file>.ast\n"
               "  --dump-hir           Write the high-level IR to Temp/Hir/hir.txt\n"
               "  --dump-lir           Write the low-level IR to Temp/Lir/lir.txt\n"
               "  --dump-rcu           Write RCU object files to Temp/Obj/ and text dumps to Temp/Rcu/\n"
               "  --dump-sema          Write semantic analysis results to Temp/Sema/sema.txt\n"
               "  --dump-tokens        Write the token stream to Temp/Tokens/<file>.tokens\n"
               "\n"
               "Artifacts are stored under [Build].Output, defaulting to Bin/Debug/ or Bin/Release/.\n"
               "\n"
               "Examples:\n"
               "  rux build\n"
               "  rux build --debug\n"
               "  rux build --release\n"
               "  rux build --stats\n"
               "  rux build --verbose --release\n"
               "  rux build --dump-ast\n"
               "  rux build --dump-hir\n"
               "  rux build --dump-lir\n"
               "  rux build --dump-asm\n"
               "  rux build --dump-rcu\n");
}

void Cli::PrintHelpClean() {
    std::print("Remove all build artifacts and temporary files\n"
               "\n"
               "Usage: rux clean [options]\n"
               "\n"
               "Removes the configured build output directory and Temp/ folder.\n"
               "\n"
               "Options:\n"
               "  --temp    Removes only Temp/ directory\n"
               "\n"
               "Examples:\n"
               "  rux clean\n"
               "  rux clean --temp\n");
}

void Cli::PrintHelpDoc() {
    std::print("Generate documentation for the package\n"
               "\n"
               "Usage: rux doc [options]\n"
               "\n"
               "Options:\n"
               "  --open    Open documentation after the generation\n"
               "\n"
               "Examples:\n"
               "  rux doc\n"
               "  rux doc --open\n");
}

void Cli::PrintHelpFmt() {
    std::print("Format all *.rux source files\n"
               "\n"
               "Usage: rux fmt [options]\n"
               "\n"
               "Options:\n"
               "  --check       Check formatting without modifying files\n"
               "  --manifest    Format only the manifest (Rux.toml)\n"
               "\n"
               "Examples:\n"
               "  rux fmt\n"
               "  rux fmt --check\n"
               "  rux fmt --manifest\n");
}

void Cli::PrintHelpInit() {
    std::print("Initialize a new package in the current directory\n"
               "\n"
               "Usage: rux init [options]\n"
               "\n"
               "If Rux.toml does not exist, it will be created.\n"
               "\n"
               "Options:\n"
               "  --bin    Create a binary package\n"
               "  --lib    Create a library package\n"
               "\n"
               "Examples:\n"
               "  rux init\n"
               "  rux init --bin\n");
}

void Cli::PrintHelpInstall() {
    std::print("Install dependencies\n"
               "\n"
               "Usage: rux install\n"
               "       rux install [package]\n"
               "       rux install [package]@[version]\n"
               "       rux install --dev [package]\n"
               "\n"
               "Without a package name, downloads all registry dependencies listed in Rux.toml.\n"
               "With a package name, adds it to Rux.toml and downloads it to the local cache.\n"
               "Use --dev to clone the package repository's dev branch instead of the default branch.\n"
               "\n"
               "Examples:\n"
               "  rux install\n"
               "  rux install Std\n"
               "  rux install Std@0.1.0\n"
               "  rux install --dev Std\n"
               "  rux install --dev Windows\n");
}

void Cli::PrintHelpUninstall() {
    std::print("Uninstall dependencies from the local cache\n"
               "\n"
               "Usage: rux uninstall\n"
               "       rux uninstall [package]\n"
               "\n"
               "Without a package name, removes all registry dependencies listed in Rux.toml\n"
               "from the local cache. With a package name, removes only that package.\n"
               "\n"
               "Examples:\n"
               "  rux uninstall\n"
               "  rux uninstall Json\n");
}

void Cli::PrintHelpList() {
    std::print("List packages in the manifest file\n"
               "\n"
               "Usage: rux list [options]\n"
               "\n"
               "Options:\n"
               "  --global    List all packages in the global cache instead of the manifest\n"
               "\n"
               "Examples:\n"
               "  rux list\n"
               "  rux list --global\n");
}

void Cli::PrintHelpNew() {
    std::print("Create a new Rux package in a new directory\n"
               "\n"
               "Usage: rux new [name] [options]\n"
               "\n"
               "Options:\n"
               "  --bin           Create a binary application (default)\n"
               "  --lib           Create a library package\n"
               "  --path <dir>    Create in a specific directory\n"
               "\n"
               "Examples:\n"
               "  rux new Program\n"
               "  rux new Program --bin\n");
}

void Cli::PrintHelpRemove() {
    std::print("Remove a dependency from the manifest\n"
               "\n"
               "Usage: rux remove [name]\n"
               "\n"
               "Examples:\n"
               "  rux remove Json\n"
               "  rux remove Random\n");
}

void Cli::PrintHelpRun() {
    std::print("Build and execute a runnable target\n"
               "\n"
               "Usage: rux run [options] [-- args...]\n"
               "\n"
               "Arguments after '--' are forwarded to the executable.\n"
               "\n"
               "Options:\n"
               "  --release    Build with release profile\n"
               "\n"
               "Examples:\n"
               "  rux run\n"
               "  rux run --release\n"
               "  rux run -- --port 8080\n");
}

void Cli::PrintHelpTest() {
    std::print("Run package unit tests\n"
               "\n"
               "Usage: rux test [options]\n"
               "\n"
               "Options:\n"
               "  --release    Build with release profile\n"
               "\n"
               "Examples:\n"
               "  rux test\n"
               "  rux test --release\n");
}

void Cli::PrintHelpUpdate() {
    std::print("Update dependencies\n"
               "\n"
               "Usage: rux update [options]\n"
               "\n"
               "Options:\n"
               "  --global    Update all packages in the global cache instead of only those\n"
               "              listed in the manifest\n"
               "\n"
               "Without --global, checks all registry dependencies listed in Rux.toml and\n"
               "pulls the latest changes. Missing packages are cloned from the registry.\n"
               "With --global, updates every package present in the local cache.\n"
               "\n"
               "Examples:\n"
               "  rux update\n"
               "  rux update --global\n");
}

void Cli::PrintHelpVersion() {
    std::print("Show information about the Rux toolchain version\n"
               "\n"
               "Usage: rux version\n"
               "\n"
               "Examples:\n"
               "  rux version\n"
               "  rux -V\n"
               "  rux --version\n");
}

void Cli::PrintHelpInfo() {
    std::print("Show information about an installed Rux package\n"
               "\n"
               "Usage: rux info [package name]\n"
               "\n"
               "Options:\n"
               "  --json    Returns a json instead of a string\n"
               "Examples:\n"
               "  rux info Std\n"
               "  rux info Windows\n");
}

void Cli::PrintHelpCheck() {
    std::print("Check package source code for errors.\n\n"

               "Usage:\n"
               "  rux check [options]\n\n"

               "Options:\n"
               "  --json            Output diagnostics as JSON\n"
               "  --target <triple> Check for a specific target\n"

               "Examples:\n"
               "  rux check\n"
               "  rux check --json\n"
               "  rux check --target windows-x64\n");
}

void Cli::PrintVersion() {
    std::print("Rux {} ({} {})\n", RUX_VERSION, RUX_BUILD_DATE, RUX_BUILD_TIME);
}

void Cli::PrintUnknownCommand(std::string_view command) {
    std::print(stderr,
               "error: unknown command '{}'\n\n"
               "Use 'rux help' for a list of available commands.\n",
               command);
}

void Cli::PrintUnknownOption(std::string_view option, std::string_view command) {
    if (command.empty())
        std::print(stderr, "error: unknown option '{}'\n", option);
    else
        std::print(stderr, "error: unknown option '{}' for command '{}'\n", option, command);
}

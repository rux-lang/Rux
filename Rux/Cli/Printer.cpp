#include "Printer.h"
#include <iostream>
#include "../Build/Info.h"
#include "../Build/Platform.h"

namespace Rux::Cli
{
	// Escape codes for foreground colors
	const char* Printer::Blue = "\033[34m";
	const char* Printer::Cyan = "\033[36m";
	const char* Printer::Gray = "\033[90m";
	const char* Printer::Green = "\033[32m";
	const char* Printer::Magenta = "\033[35m";
	const char* Printer::Red = "\033[31m";
	const char* Printer::Yellow = "\033[33m";
	const char* Printer::Reset = "\033[0m";

	Printer::Printer()
	{
	}

	void Printer::ShowHelp()
	{
		// TODO: Add --color off option to disable colors
		using std::cout;
		cout << Green << "Usage: " << Reset << "rux <command> [options] [args]\n";
		cout << Green << "Commands:" << Reset << "\n";
		cout << Magenta << "    build     " << Reset << "Compile the current package\n";
		cout << Magenta << "    clean     " << Reset << "Remove the target directory\n";
		cout << Magenta << "    help      " << Reset << "Show help information\n";
		cout << Magenta << "    init      " << Reset << "Create a new package in the current directory\n";
		cout << Magenta << "    new       " << Reset << "Create a new package\n";
		cout << Magenta << "    run       " << Reset << "Run a binary\n";
		cout << Magenta << "    version   " << Reset << "Show detailed version info\n";
		cout << Magenta << "    -h        " << Reset << "Show help information\n";
		cout << Magenta << "    -v        " << Reset << "Show brief version information\n";
		cout << Magenta << "    --help    " << Reset << "Show help information\n";
		cout << Magenta << "    --version " << Reset << "Show brief version information\n";
		cout << Green << "Options:" << Reset << "\n";
		cout << Magenta << "   --option   " << Reset << "Each command has specific options\n";
		cout << Green << "Arguments:" << Reset << "\n";
		cout << Magenta << "   arg1 arg2  " << Reset << "Some commands can take additional args\n";
		cout << "For more information, visit: " << Blue << "https://rux-lang.dev/cli" << Reset << std::endl;
	}

	void Printer::ShowVersion()
	{
		using std::cout;
		cout << "Rux " << BUILD_VERSION << std::endl;
	}

	void Printer::ShowVersionDetailed()
	{
		using std::cout;
		cout << "Name:     " << "Rux\n";
		cout << "Version:  " << BUILD_VERSION << "\n";
		cout << "Commit:   " << BUILD_HASH << "\n";
		cout << "Branch:   " << BUILD_BRANCH << "\n";
		cout << "Built:    " << BUILD_DATETIME << "\n";
		cout << "Profile:  " << BUILD_PROFILE << "\n";
		cout << "Compiler: " << "MSVC " << _MSC_VER << "\n";
		cout << "Target:   " << TARGET_FULL << std::endl;
	}
}

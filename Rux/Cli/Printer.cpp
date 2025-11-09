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

	void Printer::PrintHelp()
	{
		// TODO: Add --color off option to disable colors
		using std::cout;
		cout << Green << "Usage: " << Magenta << "rux " << Cyan << "[options] [command] [args]" << Reset << "\n";
		cout << Green << "Options:" << Reset << "\n";
		cout << Magenta << "   -h         " << Reset << "Print help and exit\n";
		cout << Magenta << "   -v         " << Reset << "Print version info and exit\n";
		cout << Magenta << "   --help     " << Reset << "Print help and exit\n";
		cout << Magenta << "   --version  " << Reset << "Print version info and exit\n";
		cout << Green << "Commands:" << Reset << "\n";
		cout << Magenta << "    build     " << Reset << "Compile the current package\n";
		cout << Magenta << "    clean     " << Reset << "Remove the target directory\n";
		cout << Magenta << "    help      " << Reset << "Print help\n";
		cout << Magenta << "    init      " << Reset << "Create a new package in the current directory\n";
		cout << Magenta << "    new       " << Reset << "Create a new package\n";
		cout << Magenta << "    run       " << Reset << "Run a binary\n";
		cout << Magenta << "    version   " << Reset << "Print detailed version info" << std::endl;
	}

	void Printer::PrintVersion()
	{
		using std::cout;
		cout << "Rux " << BUILD_VERSION << std::endl;
	}

	void Printer::PrintVersionDetailed()
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

#include "Printer.h"
#include <iostream>
#include "../Build/Info.h"
#include "../Build/Platform.h"

namespace Rux::Cli
{
	Printer::Printer()
	{
	}

	void Printer::PrintHelp()
	{
		using std::cout;
		cout << "Usage: rux [global options] <command> [options] [args]\n";
		cout << "Options:\n";
		cout << "   -h, --help      Show help\n";
		cout << "   -v, --version   Show version\n";
		cout << "Commands\n";
		cout << "    build          Compile the current package\n";
		cout << "    clean          Remove the target directory\n";
		cout << "    init           Create a new package in the current directory\n";
		cout << "    new            Create a new package\n";
		cout << "    run            Run a binary" << std::endl;
	}

	void Printer::PrintVersion()
	{
		using std::cout;
		cout << "Rux " << BUILD_VERSION << std::endl;
	}

	void Printer::PrintVersionDetailed()
	{
		using std::cout;
		cout << "Version:  " << BUILD_VERSION << "\n";
		cout << "Commit:   " << BUILD_HASH << "\n";
		cout << "Branch:   " << BUILD_BRANCH << "\n";
		cout << "Built:    " << BUILD_DATETIME << "\n";
		cout << "Profile:  " << BUILD_PROFILE << "\n";
		cout << "Compiler: " << "MSVC " << _MSC_VER << "\n";
		cout << "Target:   " << TARGET_FULL << std::endl;
	}
}

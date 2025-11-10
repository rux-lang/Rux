#include <iostream>
#include "Cli/Parser.h"
#include "Cli/Printer.h"

using std::cout;

int main(int argc, const char* argv[])
{
	Rux::Cli::Parser parser(argc, argv);
	Rux::Cli::Printer printer;
	if (argc == 1)
		printer.ShowHelp();
	return 0;
}

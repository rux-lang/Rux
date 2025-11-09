#include <iostream>
#include "Cli/Parser.h"
#include "Cli/Printer.h"

using std::cout;


int main(int argc, const char* argv[])
{
	Rux::Cli::Parser parser(argc, argv);
	Rux::Cli::Printer printer;

	if (true)
	{
		printer.PrintVersionDetailed();
	}

	/*if (argc == 1)
		ShowHelp();*/
	return 0;
}
#include "pch.h"

void ShowHelp()
{
	using std::cout;
	cout << "Usage: rux [options] [commands] input\n";
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


int main(int argc, char* argv[])
{
	if (argc == 1)
		ShowHelp();
	return 0;
}
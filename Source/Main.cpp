/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Cli.h"

int main(const int argc, char* argv[]) {
    return Rux::Cli(argc, argv).Run();
}

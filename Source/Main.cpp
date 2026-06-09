// Copyright (c) Rux contributors
// SPDX-License-Identifier: MIT

#include "Rux/Cli/Cli.h"

int main(const int argc, char* argv[]) {
    return Rux::Cli(argc, argv).Run();
}

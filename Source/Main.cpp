// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

#include "Rux/Cli/Cli.h"

int main(int const argc, char *argv[]) {
    return Rux::Cli(argc, argv).Run();
}

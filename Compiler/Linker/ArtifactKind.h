#pragma once

// Compatibility include. `ArtifactKind` is owned by BuildInfo, which the linker
// and the optimizer both depend on; this header only lets linker code include it
// under the path it lives beside. Include "BuildInfo/ArtifactKind.h" in new code.

#include "BuildInfo/ArtifactKind.h"

#pragma once

#include "Evaluation/TypedConstant.h"

namespace Rux::Optimization {
// The optimizer folds with the typed scalar model that compile-time evaluation also uses, which `RuxEvaluation` owns.
using Evaluation::CastConstant;
using Evaluation::CastConstantExactly;
using Evaluation::EvaluateBinary;
using Evaluation::EvaluateUnary;
using Evaluation::ParseConstant;
using Evaluation::TypedConstant;
} // namespace Rux::Optimization

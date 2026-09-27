#pragma once

// Internal pieces of ear::fit shared between its translation units.

#include "broaudio/ear/fit.h"

#include <vector>

namespace broaudio::ear::detail {

// Throws std::invalid_argument ("measures.<name>: ...") for an unknown
// measurement, a non-finite target or a bad weight / scale.
void validateMeasureTargets(const std::vector<FitMeasureTarget>& measures);

} // namespace broaudio::ear::detail

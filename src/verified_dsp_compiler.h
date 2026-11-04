#pragma once

// JS-subset tokenizer, recursive-descent parser, and Brass MIR compiler for
// Verified JS Audio DSP Kernels.

#include "broaudio/verified_dsp_kernel.h"

#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>

#include <string>
#include <string_view>

namespace broaudio {

bool compileJsDspToMir(
    std::string_view jsSourceOrExpr,
    const VerifiedDspCompileOptions& opts,
    brass::Module& mod,
    brass::Function*& outFn,
    std::string& outError);

} // namespace broaudio

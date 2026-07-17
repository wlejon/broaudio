// dr_libs single-header implementations, compiled in one translation unit.
//
// These vendored decoders (dr_wav / dr_flac / dr_mp3) live in their own target
// (broaudio_codecs) rather than inside a broaudio translation unit so that
// CodeQL's manual build can pre-build them *before* it starts tracing and never
// extract their implementation bodies. That build-order step — not paths-ignore,
// which does not filter compiled languages built with build-mode: manual — is
// what keeps third_party out of broaudio's Security tab. See
// .github/workflows/codeql.yml. broaudio's own TUs include these headers
// without the *_IMPLEMENTATION defines, so they see declarations only.

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

// Backend selection (createAudioBackend) and the kind <-> name helpers.

#include "broaudio/device.h"
#include "broaudio/log.h"
#include "backends.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace broaudio {

const char* audioBackendKindName(AudioBackendKind kind)
{
    switch (kind) {
        case AudioBackendKind::Auto:     return "auto";
        case AudioBackendKind::PipeWire: return "pipewire";
        case AudioBackendKind::Sdl:      return "sdl";
        case AudioBackendKind::Null:     return "null";
    }
    return "auto";
}

bool parseAudioBackendKind(const char* name, AudioBackendKind* out)
{
    if (!name || !out) return false;
    std::string s;
    for (const char* p = name; *p; ++p)
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    if (s == "auto")                      { *out = AudioBackendKind::Auto; return true; }
    if (s == "pipewire" || s == "pw")     { *out = AudioBackendKind::PipeWire; return true; }
    if (s == "sdl")                       { *out = AudioBackendKind::Sdl; return true; }
    if (s == "null" || s == "dummy" || s == "none") { *out = AudioBackendKind::Null; return true; }
    return false;
}

bool audioBackendCompiled(AudioBackendKind kind)
{
    switch (kind) {
        case AudioBackendKind::PipeWire:
#if BROAUDIO_HAS_PIPEWIRE
            return true;
#else
            return false;
#endif
        default:
            return true;
    }
}

std::unique_ptr<AudioBackend> createAudioBackend(AudioBackendKind kind, std::string* error)
{
    bool explicitChoice = kind != AudioBackendKind::Auto;
    if (kind == AudioBackendKind::Auto) {
        const char* env = std::getenv("BROAUDIO_BACKEND");
        AudioBackendKind fromEnv;
        if (env && *env) {
            if (parseAudioBackendKind(env, &fromEnv)) {
                kind = fromEnv;
                explicitChoice = kind != AudioBackendKind::Auto;
            } else {
                log(LogLevel::Warn, "broaudio: ignoring BROAUDIO_BACKEND=%s (want pipewire, sdl or null)", env);
            }
        }
    }
    if (kind == AudioBackendKind::Auto) {
        const char* sdlDriver = std::getenv("SDL_AUDIODRIVER");
        if (sdlDriver && *sdlDriver) kind = AudioBackendKind::Sdl;
    }

    if (kind == AudioBackendKind::Null) return detail::createNullAudioBackend();

#if BROAUDIO_HAS_PIPEWIRE
    if (kind == AudioBackendKind::Auto || kind == AudioBackendKind::PipeWire) {
        std::string why;
        if (auto pw = detail::createPipeWireAudioBackend(&why)) return pw;
        log(explicitChoice ? LogLevel::Warn : LogLevel::Info,
            "broaudio: PipeWire unavailable (%s); using SDL audio", why.c_str());
    }
#else
    if (kind == AudioBackendKind::PipeWire)
        log(LogLevel::Warn, "broaudio: PipeWire backend not compiled in; using SDL audio");
#endif
    (void)explicitChoice;
    return detail::createSdlAudioBackend(error);
}

} // namespace broaudio

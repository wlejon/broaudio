#pragma once

#include <functional>

namespace broaudio {

enum class LogLevel {
    Info,
    Warn,
    Error
};

using LogCallback = std::function<void(LogLevel, const char*)>;

// Override the destination for broaudio's diagnostic output. An application
// that has somewhere to put logs — a file, its own logger — should set this,
// and everything broaudio has to say goes there instead of the console.
//
// Pass an empty std::function to restore the default sink.
void setLogCallback(LogCallback cb);

// Verbosity of the DEFAULT sink — what broaudio prints when no callback is
// installed. Defaults to Warn, because a library has no business writing
// routine status lines to a process's console, and "broaudio: initialized
// 44100 Hz stereo" appearing under a GUI application's shell prompt is exactly
// that. Problems still get through; silently swallowing those would be worse.
//
// Does NOT affect an installed callback — a host that asked for the messages
// gets all of them and decides for itself.
void setDefaultLogLevel(LogLevel minLevel);

// Internal: format and dispatch through the active callback. Uses printf-style
// formatting. Truncates at 2KB.
void log(LogLevel level, const char* fmt, ...);

} // namespace broaudio

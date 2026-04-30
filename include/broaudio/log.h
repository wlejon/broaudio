#pragma once

#include <functional>

namespace broaudio {

enum class LogLevel {
    Info,
    Warn,
    Error
};

using LogCallback = std::function<void(LogLevel, const char*)>;

// Override the destination for broaudio's diagnostic output. The default sink
// uses SDL_Log, which on Windows pops up a parent console for GUI-subsystem
// apps — embedders that redirect stdio elsewhere (e.g. to a log file) should
// set their own callback so broaudio messages land in the same place.
//
// Pass an empty std::function to restore the SDL_Log default.
void setLogCallback(LogCallback cb);

// Internal: format and dispatch through the active callback. Uses printf-style
// formatting. Truncates at 2KB.
void log(LogLevel level, const char* fmt, ...);

} // namespace broaudio

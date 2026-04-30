#include "broaudio/log.h"

#include <SDL3/SDL.h>
#include <cstdarg>
#include <cstdio>

namespace broaudio {

namespace {
LogCallback g_callback;
}

void setLogCallback(LogCallback cb)
{
    g_callback = std::move(cb);
}

void log(LogLevel level, const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (g_callback) {
        g_callback(level, buf);
        return;
    }

    switch (level) {
        case LogLevel::Info:  SDL_Log("%s", buf); break;
        case LogLevel::Warn:  SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", buf); break;
        case LogLevel::Error: SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", buf); break;
    }
}

} // namespace broaudio

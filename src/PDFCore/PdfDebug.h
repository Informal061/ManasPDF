#pragma once

// No-op debug macros for release builds.
// Define PDF_ENABLE_DEBUG before including this header to enable logging.

#ifdef PDF_ENABLE_DEBUG

#include <cstdio>
#include <cstdarg>
#ifdef _WIN32
#include <windows.h>
#endif

inline void LogDebugImpl(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
#ifdef _WIN32
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
#else
    fprintf(stderr, "%s\n", buf);
#endif
}

#define LogDebug(...) LogDebugImpl(__VA_ARGS__)

#else

#define LogDebug(...) ((void)0)

#endif

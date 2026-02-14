#pragma once

// ============================================
// DEBUG LOGGING ENABLED - Encryption debugging
// ============================================

#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdio>
#include <cstdarg>

namespace pdf
{
    class PdfDebug
    {
    public:
        static inline void Init() {}
        static inline void Log(const char*, ...) {}
        static inline void Close() {}
    };
}

// Active debug macro - outputs to Visual Studio Output window AND file
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
#endif

    // Also write to a log file
    static FILE* logFile = nullptr;
    if (!logFile) {
        fopen_s(&logFile, "C:\\Temp\\pdf_debug.log", "w");
    }
    if (logFile) {
        fprintf(logFile, "%s\n", buf);
        fflush(logFile);
    }
}

#define LogDebug(...) LogDebugImpl(__VA_ARGS__)
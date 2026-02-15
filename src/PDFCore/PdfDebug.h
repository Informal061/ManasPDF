#pragma once

// ============================================
// Production build: all logging disabled
// ============================================

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

// No-op: all LogDebug calls compile to nothing
#define LogDebug(...) ((void)0)

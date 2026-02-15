#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace pdf
{
    // ============================================
    // GLYPH CACHE v2 - Uses font hash instead of pointer
    //
    // Problem: Old cache used FT_Face pointer as key
    //          When fonts are reloaded, pointers change = cache miss
    // Solution: Use font program hash + glyph ID + pixel size
    // ============================================

    struct GlyphCacheKey
    {
        size_t fontHash;        // Hash of font program (stable across reloads)
        uint32_t glyphId;       // Glyph index
        uint16_t pixelSize;     // Pixel size (height)

        bool operator==(const GlyphCacheKey& other) const
        {
            return fontHash == other.fontHash &&
                glyphId == other.glyphId &&
                pixelSize == other.pixelSize;
        }
    };

    struct GlyphCacheKeyHash
    {
        size_t operator()(const GlyphCacheKey& k) const
        {
            return k.fontHash ^
                (std::hash<uint32_t>()(k.glyphId) << 1) ^
                (std::hash<uint16_t>()(k.pixelSize) << 2);
        }
    };

    struct CachedGlyph
    {
        std::vector<uint8_t> bitmap;    // Grayscale bitmap
        int width = 0;
        int height = 0;
        int pitch = 0;
        int bearingX = 0;               // bitmap_left
        int bearingY = 0;               // bitmap_top
        int advanceX = 0;               // advance.x >> 6
    };

    class GlyphCache
    {
    public:
        static GlyphCache& instance()
        {
            static GlyphCache inst;
            return inst;
        }

        // Get cached glyph or render and cache it
        // fontHash: hash of font program (from FontCache::getFontHash)
        const CachedGlyph* getOrRender(FT_Face face, size_t fontHash, FT_UInt glyphId, int pixelSize);

        // Legacy API (computes hash from face pointer - less effective)
        const CachedGlyph* getOrRender(FT_Face face, FT_UInt glyphId, int pixelSize)
        {
            // Fallback: use face pointer as hash (not ideal)
            return getOrRender(face, reinterpret_cast<size_t>(face), glyphId, pixelSize);
        }

        void clear();

        size_t hitCount() const { return _hits; }
        size_t missCount() const { return _misses; }
        size_t cacheSize() const { return _cache.size(); }

    private:
        GlyphCache() = default;
        ~GlyphCache() = default;
        GlyphCache(const GlyphCache&) = delete;
        GlyphCache& operator=(const GlyphCache&) = delete;

        std::unordered_map<GlyphCacheKey, CachedGlyph, GlyphCacheKeyHash> _cache;
        std::mutex _mutex;

        size_t _totalMemory = 0;  // Track total memory usage

        size_t _hits = 0;
        size_t _misses = 0;

        static constexpr size_t MAX_CACHE_SIZE = 20000;       // Max cached glyphs (reduced from 100K)
        static constexpr size_t MAX_MEMORY_BYTES = 128 * 1024 * 1024;  // 128MB max memory
    };

} // namespace pdf

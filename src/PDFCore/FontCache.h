#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace pdf
{
    // ============================================
    // FONT CACHE - Document-level FT_Face caching
    //
    // Problem: Her sayfa i�in FT_New_Memory_Face �a�r�l�yor (~100ms per font)
    // Solution: Font program hash ile cache, ayn� font i�in ayn� FT_Face kullan
    // ============================================

    // Hash font program data
    inline size_t hashFontProgram(const std::vector<uint8_t>& data)
    {
        if (data.empty()) return 0;

        // Fast hash using first/last/middle bytes + size
        size_t h = data.size();
        if (data.size() >= 8)
        {
            h ^= *reinterpret_cast<const size_t*>(data.data());
            h ^= *reinterpret_cast<const size_t*>(data.data() + data.size() - 8);
            if (data.size() >= 16)
                h ^= *reinterpret_cast<const size_t*>(data.data() + data.size() / 2);
        }
        return h;
    }

    struct CachedFont
    {
        FT_Face face = nullptr;
        std::vector<uint8_t> fontData;  // Keep data alive for FreeType
        size_t hash = 0;

        ~CachedFont()
        {
            if (face)
            {
                FT_Done_Face(face);
                face = nullptr;
            }
        }
    };

    class FontCache
    {
    public:
        static FontCache& instance()
        {
            static FontCache inst;
            return inst;
        }

        // Get or create FT_Face for font program
        FT_Face getOrCreate(FT_Library ftLib, const std::vector<uint8_t>& fontProgram)
        {
            if (fontProgram.empty() || !ftLib)
                return nullptr;

            size_t hash = hashFontProgram(fontProgram);

            // Check cache
            {
                std::lock_guard<std::mutex> lock(_mutex);
                auto it = _cache.find(hash);
                if (it != _cache.end())
                {
                    ++_hits;
                    return it->second->face;
                }
            }

            // Cache miss - create new FT_Face
            ++_misses;

            auto cached = std::make_unique<CachedFont>();
            cached->fontData = fontProgram;  // Copy to keep alive
            cached->hash = hash;

            FT_Error err = FT_New_Memory_Face(
                ftLib,
                cached->fontData.data(),
                (FT_Long)cached->fontData.size(),
                0,
                &cached->face
            );

            if (err != 0 || !cached->face)
                return nullptr;

            FT_Face face = cached->face;

            // Add to cache
            {
                std::lock_guard<std::mutex> lock(_mutex);

                // Evict if too large
                if (_cache.size() >= MAX_CACHE_SIZE)
                {
                    auto it = _cache.begin();
                    if (it != _cache.end())
                        _cache.erase(it);
                }

                _cache[hash] = std::move(cached);
            }

            return face;
        }

        // Get font hash for use in GlyphCache
        size_t getFontHash(const std::vector<uint8_t>& fontProgram)
        {
            return hashFontProgram(fontProgram);
        }

        void clear()
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _cache.clear();
            _hits = 0;
            _misses = 0;
        }

        size_t hitCount() const { return _hits; }
        size_t missCount() const { return _misses; }
        size_t cacheSize() const { return _cache.size(); }

    private:
        FontCache() = default;
        ~FontCache() { clear(); }
        FontCache(const FontCache&) = delete;
        FontCache& operator=(const FontCache&) = delete;

        std::unordered_map<size_t, std::unique_ptr<CachedFont>> _cache;
        std::mutex _mutex;
        size_t _hits = 0;
        size_t _misses = 0;

        static constexpr size_t MAX_CACHE_SIZE = 100;  // Max cached fonts
    };

} // namespace pdf

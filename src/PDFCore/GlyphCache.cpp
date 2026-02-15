#include "pch.h"
#include "GlyphCache.h"
#include <algorithm>
#include <cstring>

namespace pdf
{
    const CachedGlyph* GlyphCache::getOrRender(FT_Face face, size_t fontHash, FT_UInt glyphId, int pixelSize)
    {
        if (!face || glyphId == 0 || pixelSize <= 0)
            return nullptr;

        // Minimum pixel size - çok küçük fontlar render edilemez
        // Minimum 4px, çünkü daha küçük fontlar okunamaz
        const int MIN_PIXEL_SIZE = 4;
        const int MAX_PIXEL_SIZE = 512;

        int effectivePixelSize = std::max(MIN_PIXEL_SIZE, std::min(pixelSize, MAX_PIXEL_SIZE));

        GlyphCacheKey key;
        key.fontHash = fontHash;
        key.glyphId = glyphId;
        key.pixelSize = static_cast<uint16_t>(effectivePixelSize);

        // Check cache first
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _cache.find(key);
            if (it != _cache.end())
            {
                ++_hits;
                return &it->second;
            }
        }

        // Cache miss - render the glyph
        ++_misses;

        // Set pixel size
        FT_Error err = FT_Set_Pixel_Sizes(face, 0, effectivePixelSize);
        if (err != 0)
            return nullptr;

        // Load glyph
        err = FT_Load_Glyph(face, glyphId, FT_LOAD_DEFAULT);
        if (err != 0)
            return nullptr;

        // Render glyph
        err = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
        if (err != 0)
            return nullptr;

        FT_GlyphSlot g = face->glyph;
        FT_Bitmap& bm = g->bitmap;

        // Support both grayscale and mono bitmaps
        // Convert mono to grayscale if needed
        if (bm.pixel_mode != FT_PIXEL_MODE_GRAY && bm.pixel_mode != FT_PIXEL_MODE_MONO)
            return nullptr;

        // Create cached glyph
        CachedGlyph cached;
        cached.width = bm.width;
        cached.height = bm.rows;
        cached.bearingX = g->bitmap_left;
        cached.bearingY = g->bitmap_top;
        cached.advanceX = static_cast<int>(g->advance.x >> 6);

        // Copy bitmap data - convert mono to grayscale if needed
        if (bm.buffer && bm.rows > 0 && bm.width > 0)
        {
            if (bm.pixel_mode == FT_PIXEL_MODE_MONO)
            {
                // Convert 1-bit mono to 8-bit grayscale
                cached.pitch = bm.width;
                cached.bitmap.resize(bm.rows * bm.width);

                for (unsigned int row = 0; row < bm.rows; ++row)
                {
                    unsigned char* srcRow = bm.buffer + row * bm.pitch;
                    unsigned char* dstRow = cached.bitmap.data() + row * bm.width;

                    for (unsigned int col = 0; col < bm.width; ++col)
                    {
                        int byteIndex = col / 8;
                        int bitIndex = 7 - (col % 8);
                        bool isSet = (srcRow[byteIndex] >> bitIndex) & 1;
                        dstRow[col] = isSet ? 255 : 0;
                    }
                }
            }
            else
            {
                // Grayscale - copy directly
                cached.pitch = bm.pitch;
                size_t dataSize = static_cast<size_t>(bm.rows) * static_cast<size_t>(std::abs(bm.pitch));
                cached.bitmap.resize(dataSize);

                if (bm.pitch > 0)
                {
                    std::memcpy(cached.bitmap.data(), bm.buffer, dataSize);
                }
                else
                {
                    int absPitch = std::abs(bm.pitch);
                    for (unsigned int row = 0; row < bm.rows; ++row)
                    {
                        std::memcpy(
                            cached.bitmap.data() + row * absPitch,
                            bm.buffer + row * bm.pitch,
                            absPitch);
                    }
                }
            }
        }

        // Add to cache
        {
            std::lock_guard<std::mutex> lock(_mutex);

            // Calculate memory for this glyph
            size_t glyphMemory = cached.bitmap.size() + sizeof(CachedGlyph);

            // Evict if cache too large (count or memory)
            while ((_cache.size() >= MAX_CACHE_SIZE || _totalMemory + glyphMemory > MAX_MEMORY_BYTES)
                && !_cache.empty())
            {
                // Clear 25% of cache
                auto it = _cache.begin();
                size_t toRemove = std::max(size_t(1), _cache.size() / 4);
                for (size_t i = 0; i < toRemove && it != _cache.end(); ++i)
                {
                    _totalMemory -= (it->second.bitmap.size() + sizeof(CachedGlyph));
                    it = _cache.erase(it);
                }
            }

            auto [insertIt, inserted] = _cache.emplace(key, std::move(cached));
            if (inserted)
            {
                _totalMemory += glyphMemory;
            }
            return &insertIt->second;
        }
    }

    void GlyphCache::clear()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _cache.clear();
        _totalMemory = 0;
        _hits = 0;
        _misses = 0;
    }

} // namespace pdf
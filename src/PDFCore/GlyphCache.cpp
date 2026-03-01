#include "pch.h"
#include "GlyphCache.h"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace pdf
{
    const CachedGlyph* GlyphCache::getOrRender(FT_Face face, size_t fontHash, FT_UInt glyphId, int pixelSize)
    {
        if (!face || glyphId == 0 || pixelSize <= 0)
            return nullptr;

        const int MIN_PIXEL_SIZE = 4;
        const int MAX_PIXEL_SIZE = 512;
        const int MIN_QUALITY_SIZE = 20;  // Supersampling: render at least this size for quality

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

        // Render at higher resolution for better anti-aliasing at small sizes
        int renderSize = std::max(effectivePixelSize, MIN_QUALITY_SIZE);
        renderSize = std::min(renderSize, MAX_PIXEL_SIZE);
        bool needsDownsample = (renderSize > effectivePixelSize);

        // Set pixel size
        FT_Error err = FT_Set_Pixel_Sizes(face, 0, renderSize);
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
        if (bm.pixel_mode != FT_PIXEL_MODE_GRAY && bm.pixel_mode != FT_PIXEL_MODE_MONO)
            return nullptr;

        // Create cached glyph
        CachedGlyph cached;

        if (needsDownsample && bm.buffer && bm.rows > 0 && bm.width > 0)
        {
            // === SUPERSAMPLING PATH ===
            // Render was at renderSize, downsample to effectivePixelSize
            double ratio = (double)effectivePixelSize / (double)renderSize;

            // Get source bitmap as grayscale
            const uint8_t* srcData;
            int srcPitch;
            int srcW = (int)bm.width, srcH = (int)bm.rows;
            std::vector<uint8_t> monoConverted;

            if (bm.pixel_mode == FT_PIXEL_MODE_MONO)
            {
                srcPitch = srcW;
                monoConverted.resize((size_t)srcH * srcW);
                for (int row = 0; row < srcH; ++row)
                {
                    unsigned char* srcRow = bm.buffer + row * bm.pitch;
                    for (int col = 0; col < srcW; ++col)
                        monoConverted[row * srcW + col] = ((srcRow[col / 8] >> (7 - col % 8)) & 1) ? 255 : 0;
                }
                srcData = monoConverted.data();
            }
            else
            {
                srcData = bm.buffer;
                srcPitch = bm.pitch;
            }

            // Compute downsampled dimensions
            int dstW = std::max(1, (int)std::round(srcW * ratio));
            int dstH = std::max(1, (int)std::round(srcH * ratio));

            cached.width = dstW;
            cached.height = dstH;
            cached.pitch = dstW;
            cached.bearingX = (int)std::round(g->bitmap_left * ratio);
            cached.bearingY = (int)std::round(g->bitmap_top * ratio);
            cached.advanceX = (int)std::round((g->advance.x >> 6) * ratio);

            // Box-filter area-average downsample
            cached.bitmap.resize((size_t)dstW * dstH, 0);
            double xScale = (double)srcW / dstW;
            double yScale = (double)srcH / dstH;

            for (int dy = 0; dy < dstH; ++dy)
            {
                double sy0 = dy * yScale;
                double sy1 = (dy + 1) * yScale;
                int isy0 = (int)sy0;
                int isy1 = std::min((int)std::ceil(sy1), srcH);

                for (int dx = 0; dx < dstW; ++dx)
                {
                    double sx0 = dx * xScale;
                    double sx1 = (dx + 1) * xScale;
                    int isx0 = (int)sx0;
                    int isx1 = std::min((int)std::ceil(sx1), srcW);

                    double sum = 0.0, area = 0.0;
                    for (int sy = isy0; sy < isy1; ++sy)
                    {
                        double cy0 = std::max((double)sy, sy0);
                        double cy1 = std::min((double)(sy + 1), sy1);
                        double ch = cy1 - cy0;

                        for (int sx = isx0; sx < isx1; ++sx)
                        {
                            double cx0 = std::max((double)sx, sx0);
                            double cx1 = std::min((double)(sx + 1), sx1);
                            double cov = (cx1 - cx0) * ch;
                            sum += srcData[sy * srcPitch + sx] * cov;
                            area += cov;
                        }
                    }
                    cached.bitmap[dy * dstW + dx] = (area > 0.0)
                        ? (uint8_t)std::min(255.0, std::round(sum / area))
                        : 0;
                }
            }
        }
        else
        {
            // === NORMAL PATH (no downsampling needed) ===
            cached.width = bm.width;
            cached.height = bm.rows;
            cached.bearingX = g->bitmap_left;
            cached.bearingY = g->bitmap_top;
            cached.advanceX = static_cast<int>(g->advance.x >> 6);

            if (bm.buffer && bm.rows > 0 && bm.width > 0)
            {
                if (bm.pixel_mode == FT_PIXEL_MODE_MONO)
                {
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
        }

        // Add to cache
        {
            std::lock_guard<std::mutex> lock(_mutex);

            size_t glyphMemory = cached.bitmap.size() + sizeof(CachedGlyph);

            while ((_cache.size() >= MAX_CACHE_SIZE || _totalMemory + glyphMemory > MAX_MEMORY_BYTES)
                && !_cache.empty())
            {
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
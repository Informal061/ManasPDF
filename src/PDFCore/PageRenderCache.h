#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <chrono>

namespace pdf
{
    // ============================================
    // PAGE RENDER CACHE
    // Caches rendered page bitmaps to avoid re-rendering
    // ============================================

    struct CachedPage
    {
        std::vector<uint8_t> bitmap;
        int width = 0;
        int height = 0;
        double zoom = 0;
        std::chrono::steady_clock::time_point lastAccess;
        size_t memorySize = 0;
    };

    struct PageCacheKey
    {
        const void* docPtr;  // Document pointer
        int pageIndex;
        int width;
        int height;

        bool operator<(const PageCacheKey& other) const
        {
            if (docPtr != other.docPtr) return docPtr < other.docPtr;
            if (pageIndex != other.pageIndex) return pageIndex < other.pageIndex;
            if (width != other.width) return width < other.width;
            return height < other.height;
        }
    };

    class PageRenderCache
    {
    public:
        static PageRenderCache& instance()
        {
            static PageRenderCache inst;
            return inst;
        }

        // Try to get cached page (COPY version - legacy)
        bool get(const void* docPtr, int pageIndex, int width, int height,
                std::vector<uint8_t>& outBitmap)
        {
            std::lock_guard<std::mutex> lock(_mutex);

            PageCacheKey key = { docPtr, pageIndex, width, height };
            auto it = _cache.find(key);

            if (it != _cache.end())
            {
                it->second.lastAccess = std::chrono::steady_clock::now();
                outBitmap = it->second.bitmap;
                ++_hits;
                return true;
            }

            ++_misses;
            return false;
        }

        // Zero-copy cache access - copies directly to output buffer without intermediate vector
        // Returns true if cache hit and data was copied to outBuffer
        bool getDirect(const void* docPtr, int pageIndex, int width, int height,
                      uint8_t* outBuffer, size_t outBufferSize)
        {
            std::lock_guard<std::mutex> lock(_mutex);

            PageCacheKey key = { docPtr, pageIndex, width, height };
            auto it = _cache.find(key);

            if (it != _cache.end())
            {
                const auto& cached = it->second.bitmap;
                if (cached.size() <= outBufferSize)
                {
                    it->second.lastAccess = std::chrono::steady_clock::now();
                    std::memcpy(outBuffer, cached.data(), cached.size());
                    ++_hits;
                    return true;
                }
            }

            ++_misses;
            return false;
        }

        // Store rendered page
        void store(const void* docPtr, int pageIndex, int width, int height,
                  double zoom, const std::vector<uint8_t>& bitmap)
        {
            if (bitmap.empty()) return;

            std::lock_guard<std::mutex> lock(_mutex);

            // Check memory limit
            size_t newSize = bitmap.size();
            while (_totalMemory + newSize > MAX_MEMORY && !_cache.empty())
            {
                evictOldest();
            }

            PageCacheKey key = { docPtr, pageIndex, width, height };

            CachedPage page;
            page.bitmap = bitmap;
            page.width = width;
            page.height = height;
            page.zoom = zoom;
            page.lastAccess = std::chrono::steady_clock::now();
            page.memorySize = newSize;

            // Remove old entry if exists
            auto it = _cache.find(key);
            if (it != _cache.end())
            {
                _totalMemory -= it->second.memorySize;
                _cache.erase(it);
            }

            _cache[key] = std::move(page);
            _totalMemory += newSize;
        }

        // Clear cache for specific document
        void clearDocument(const void* docPtr)
        {
            std::lock_guard<std::mutex> lock(_mutex);

            for (auto it = _cache.begin(); it != _cache.end(); )
            {
                if (it->first.docPtr == docPtr)
                {
                    _totalMemory -= it->second.memorySize;
                    it = _cache.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // Clear all cache
        void clear()
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _cache.clear();
            _totalMemory = 0;
            _hits = 0;
            _misses = 0;
        }

        // Stats
        size_t hitCount() const { return _hits; }
        size_t missCount() const { return _misses; }
        size_t cacheSize() const { return _cache.size(); }
        size_t memoryUsage() const { return _totalMemory; }

    private:
        PageRenderCache() = default;
        ~PageRenderCache() = default;
        PageRenderCache(const PageRenderCache&) = delete;
        PageRenderCache& operator=(const PageRenderCache&) = delete;

        void evictOldest()
        {
            if (_cache.empty()) return;

            auto oldest = _cache.begin();
            for (auto it = _cache.begin(); it != _cache.end(); ++it)
            {
                if (it->second.lastAccess < oldest->second.lastAccess)
                    oldest = it;
            }

            _totalMemory -= oldest->second.memorySize;
            _cache.erase(oldest);
        }

        std::map<PageCacheKey, CachedPage> _cache;
        std::mutex _mutex;
        size_t _totalMemory = 0;
        size_t _hits = 0;
        size_t _misses = 0;

        // 500MB max cache
        static constexpr size_t MAX_MEMORY = 500 * 1024 * 1024;
    };

} // namespace pdf

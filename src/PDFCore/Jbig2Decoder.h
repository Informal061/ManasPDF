#pragma once
#include <vector>
#include <cstdint>
#include <memory>

namespace pdf
{
    // JBIG2 bitmap
    struct Jbig2Bitmap {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> data; // packed bits, MSB first, row-padded to byte
        int stride = 0; // bytes per row

        void allocate(int w, int h);
        int getPixel(int x, int y) const;
        void setPixel(int x, int y, int val);
        void fill(int val);
        void compositeFrom(const Jbig2Bitmap& src, int dx, int dy, int op);
    };

    // JBIG2 decoder - decodes JBIG2 stream to 1-bit bitmap
    class Jbig2Decoder
    {
    public:
        // Decode JBIG2 data (PDF embedded format - no file header)
        // globals: optional JBIG2Globals stream
        // Returns packed 1-bit bitmap (MSB first), sets outW/outH
        static bool decode(
            const std::vector<uint8_t>& data,
            const std::vector<uint8_t>& globals,
            std::vector<uint8_t>& output,
            int& outW, int& outH);
    };

} // namespace pdf

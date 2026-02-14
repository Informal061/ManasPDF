#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <map>

namespace pdf
{
    class PdfFilters
    {
    public:

        // --- Flate Decode (ZIP / zlib) ---
        static bool FlateDecode(const std::vector<uint8_t>& input,
            std::vector<uint8_t>& output);

        // --- ASCII85 Decode ---
        static bool ASCII85Decode(const std::vector<uint8_t>& input,
            std::vector<uint8_t>& output);

        // --- RunLength Decode ---
        static bool RunLengthDecode(const std::vector<uint8_t>& input,
            std::vector<uint8_t>& output);

        // --- LZW Decode ---
        static bool LZWDecode(const std::vector<uint8_t>& input,
            std::vector<uint8_t>& output);

        // --- JPEG Decode (DCTDecode) ---
        static bool JPEGDecode(const std::vector<uint8_t>& input,
            std::vector<uint8_t>& argbOut,
            int& width,
            int& height);

        // --- JPEG2000 Decode (JPXDecode) ---
        static bool JPEG2000Decode(const std::vector<uint8_t>& input,
            std::vector<uint8_t>& argbOut,
            int& width,
            int& height);

        // --- PNG Predictor Algorithm ---
        static bool ApplyPredictor(int predictor,
            int colors,
            int bitsPerComponent,
            int columns,
            std::vector<uint8_t>& data);

        // --- CCITT Fax Decode (Group 3 / Group 4) ---
        static bool CCITTFaxDecode(
            const std::vector<uint8_t>& input,
            std::vector<uint8_t>& output,
            int columns,
            int rows,
            int k,
            bool blackIs1,
            bool endOfLine,
            bool encodedByteAlign
        );

        // --- Filter Chain Processing ---
        static bool Decode(const std::vector<uint8_t>& input,
            const std::vector<std::string>& filters,
            const std::vector<std::map<std::string, int>>& params,
            std::vector<uint8_t>& output);
    };
}

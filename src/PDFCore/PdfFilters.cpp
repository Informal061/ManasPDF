#include "pch.h"
#include "PdfFilters.h"
#include <zlib.h>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <jpeglib.h>

#ifdef _WIN32
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#endif

// OpenJPEG for JPEG 2000 support
#ifdef USE_OPENJPEG
#include <openjpeg.h>
#endif

namespace pdf
{

    // ---------------------------------------------------------
    // FlateDecode (ZIP/zlib)
    // ---------------------------------------------------------

    static std::string NormalizeFilterName(const std::string& f)
    {
        if (!f.empty() && f[0] == '/')
            return f;
        return "/" + f;
    }

    static int GetParam(const std::map<std::string, int>& p, const std::string& key, int defaultVal)
    {
        auto it = p.find(key);
        if (it != p.end())
            return it->second;

        it = p.find("/" + key);
        if (it != p.end())
            return it->second;

        if (!key.empty() && key[0] == '/')
        {
            it = p.find(key.substr(1));
            if (it != p.end())
                return it->second;
        }

        return defaultVal;
    }

    static bool HasParam(const std::map<std::string, int>& p, const std::string& key)
    {
        if (p.count(key)) return true;
        if (p.count("/" + key)) return true;
        if (!key.empty() && key[0] == '/' && p.count(key.substr(1))) return true;
        return false;
    }

    bool PdfFilters::FlateDecode(const std::vector<uint8_t>& input,
        std::vector<uint8_t>& output)
    {
        if (input.empty())
            return true;

        z_stream strm{};
        strm.next_in = (Bytef*)input.data();
        strm.avail_in = (uInt)input.size();

        if (inflateInit(&strm) != Z_OK)
            return false;

        output.clear();
        output.reserve(input.size() * 3);

        const size_t CHUNK = 4096;
        uint8_t buffer[CHUNK];

        int ret = Z_OK;

        while (ret != Z_STREAM_END)
        {
            strm.next_out = buffer;
            strm.avail_out = CHUNK;

            ret = inflate(&strm, Z_NO_FLUSH);

            if (ret != Z_OK && ret != Z_STREAM_END)
            {
                inflateEnd(&strm);
                return false;
            }

            size_t produced = CHUNK - strm.avail_out;
            output.insert(output.end(), buffer, buffer + produced);
        }

        inflateEnd(&strm);
        return true;
    }

    // ---------------------------------------------------------
    // ASCII85Decode
    // ---------------------------------------------------------
    bool PdfFilters::ASCII85Decode(const std::vector<uint8_t>& input,
        std::vector<uint8_t>& output)
    {
        output.clear();
        uint32_t tuple = 0;
        int count = 0;

        for (uint8_t ch : input)
        {
            if (ch == '~') break;

            if (ch == 'z' && count == 0)
            {
                output.insert(output.end(), { 0, 0, 0, 0 });
                continue;
            }

            if (ch < '!' || ch > 'u')
                continue;

            tuple = tuple * 85 + (ch - '!');
            count++;

            if (count == 5)
            {
                output.push_back((tuple >> 24) & 0xFF);
                output.push_back((tuple >> 16) & 0xFF);
                output.push_back((tuple >> 8) & 0xFF);
                output.push_back(tuple & 0xFF);

                tuple = 0;
                count = 0;
            }
        }

        if (count > 1)
        {
            for (int i = count; i < 5; i++)
                tuple = tuple * 85 + 84;

            for (int i = 0; i < count - 1; i++)
            {
                output.push_back((tuple >> (24 - 8 * i)) & 0xFF);
            }
        }

        return true;
    }

    // ---------------------------------------------------------
    // RunLengthDecode
    // ---------------------------------------------------------
    bool PdfFilters::RunLengthDecode(const std::vector<uint8_t>& input,
        std::vector<uint8_t>& output)
    {
        output.clear();
        size_t i = 0;

        while (i < input.size())
        {
            uint8_t len = input[i++];

            if (len == 128)
                break;

            if (len < 128)
            {
                int count = len + 1;
                if (i + count > input.size())
                    break;

                output.insert(output.end(),
                    input.begin() + i,
                    input.begin() + i + count);
                i += count;
            }
            else
            {
                int count = 257 - len;
                if (i >= input.size())
                    break;

                uint8_t val = input[i++];
                for (int j = 0; j < count; j++)
                    output.push_back(val);
            }
        }

        return true;
    }

    // ---------------------------------------------------------
    // LZWDecode
    // ---------------------------------------------------------
    class LZWDecoder
    {
    public:
        std::vector<uint8_t> decode(const std::vector<uint8_t>& input)
        {
            std::vector<uint8_t> result;

            if (input.empty())
                return result;

            _data = &input;
            _bytePos = 0;
            _bitPos = 0;

            initTable();

            int prevCode = -1;
            while (!eof())
            {
                int code = readCode();
                if (code == 257)
                    break;
                if (code == 256)
                {
                    initTable();
                    prevCode = -1;
                    continue;
                }

                if (code < (int)_table.size())
                {
                    result.insert(result.end(),
                        _table[code].begin(), _table[code].end());

                    if (prevCode >= 0 && _table.size() < 4096)
                    {
                        std::vector<uint8_t> entry = _table[prevCode];
                        entry.push_back(_table[code][0]);
                        _table.push_back(entry);
                    }
                }
                else
                {
                    if (prevCode >= 0)
                    {
                        std::vector<uint8_t> entry = _table[prevCode];
                        entry.push_back(_table[prevCode][0]);
                        _table.push_back(entry);

                        result.insert(result.end(),
                            entry.begin(), entry.end());
                    }
                }

                prevCode = code;
                updateBits();
            }

            return result;
        }

    private:
        void initTable()
        {
            _table.clear();
            _table.reserve(4096);
            for (int i = 0; i < 256; ++i)
                _table.push_back({ (uint8_t)i });
            _table.push_back({});
            _table.push_back({});
            _bits = 9;
        }

        void updateBits()
        {
            size_t sz = _table.size();
            if (sz >= 511 && _bits < 10) _bits = 10;
            else if (sz >= 1023 && _bits < 11) _bits = 11;
            else if (sz >= 2047 && _bits < 12) _bits = 12;
        }

        int readCode()
        {
            int code = 0;
            for (int i = 0; i < _bits; i++)
            {
                if (eof())
                    return 257;

                int bit = ((*_data)[_bytePos] >> (7 - _bitPos)) & 1;
                code = (code << 1) | bit;
                _bitPos++;
                if (_bitPos == 8)
                {
                    _bitPos = 0;
                    _bytePos++;
                }
            }
            return code;
        }

        bool eof() const
        {
            return _bytePos >= _data->size();
        }

        const std::vector<uint8_t>* _data = nullptr;
        size_t _bytePos = 0;
        int _bitPos = 0;
        int _bits = 9;
        std::vector<std::vector<uint8_t>> _table;
    };

    bool PdfFilters::LZWDecode(const std::vector<uint8_t>& input,
        std::vector<uint8_t>& output)
    {
        LZWDecoder dec;
        output = dec.decode(input);
        return true;
    }

    // ---------------------------------------------------------
    // JPEGDecode (DCTDecode)
    // ---------------------------------------------------------
    bool PdfFilters::JPEGDecode(const std::vector<uint8_t>& input,
        std::vector<uint8_t>& argbOut,
        int& width,
        int& height)
    {
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;

        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);

        jpeg_mem_src(&cinfo, input.data(), (unsigned long)input.size());

        if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK)
        {
            jpeg_destroy_decompress(&cinfo);
            return false;
        }

        jpeg_start_decompress(&cinfo);

        width = cinfo.output_width;
        height = cinfo.output_height;
        int numChannels = cinfo.output_components;

        std::vector<uint8_t> row(width * numChannels);
        argbOut.resize(width * height * 4);

        int y = 0;
        while (cinfo.output_scanline < cinfo.output_height)
        {
            uint8_t* rowPtr = row.data();
            jpeg_read_scanlines(&cinfo, &rowPtr, 1);

            for (int x = 0; x < width; ++x)
            {
                int i = (y * width + x) * 4;
                if (numChannels == 1)
                {
                    uint8_t g = row[x];
                    argbOut[i + 0] = g;
                    argbOut[i + 1] = g;
                    argbOut[i + 2] = g;
                    argbOut[i + 3] = 255;
                }
                else if (numChannels == 3)
                {
                    argbOut[i + 0] = row[x * 3 + 0];
                    argbOut[i + 1] = row[x * 3 + 1];
                    argbOut[i + 2] = row[x * 3 + 2];
                    argbOut[i + 3] = 255;
                }
                else if (numChannels == 4)
                {
                    double c = row[x * 4 + 0] / 255.0;
                    double m = row[x * 4 + 1] / 255.0;
                    double yy = row[x * 4 + 2] / 255.0;
                    double k = row[x * 4 + 3] / 255.0;

                    uint8_t r = (uint8_t)(255 * (1 - c) * (1 - k));
                    uint8_t g = (uint8_t)(255 * (1 - m) * (1 - k));
                    uint8_t b = (uint8_t)(255 * (1 - yy) * (1 - k));

                    argbOut[i + 0] = r;
                    argbOut[i + 1] = g;
                    argbOut[i + 2] = b;
                    argbOut[i + 3] = 255;
                }
            }
            y++;
        }

        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);

        return true;
    }

    // ---------------------------------------------------------
    // JPEG2000Decode (JPXDecode) - Using OpenJPEG library
    // ---------------------------------------------------------

#ifdef USE_OPENJPEG
    // OpenJPEG stream callbacks
    struct OpjMemStream {
        const uint8_t* data;
        size_t size;
        size_t pos;
    };

    static OPJ_SIZE_T opj_mem_read(void* p_buffer, OPJ_SIZE_T p_nb_bytes, void* p_user_data) {
        OpjMemStream* stream = (OpjMemStream*)p_user_data;
        OPJ_SIZE_T remaining = stream->size - stream->pos;
        OPJ_SIZE_T toRead = (p_nb_bytes < remaining) ? p_nb_bytes : remaining;
        if (toRead > 0) {
            memcpy(p_buffer, stream->data + stream->pos, toRead);
            stream->pos += toRead;
        }
        return toRead ? toRead : (OPJ_SIZE_T)-1;
    }

    static OPJ_OFF_T opj_mem_skip(OPJ_OFF_T p_nb_bytes, void* p_user_data) {
        OpjMemStream* stream = (OpjMemStream*)p_user_data;
        if (p_nb_bytes < 0) return -1;
        OPJ_SIZE_T remaining = stream->size - stream->pos;
        OPJ_SIZE_T toSkip = ((OPJ_SIZE_T)p_nb_bytes < remaining) ? (OPJ_SIZE_T)p_nb_bytes : remaining;
        stream->pos += toSkip;
        return toSkip;
    }

    static OPJ_BOOL opj_mem_seek(OPJ_OFF_T p_nb_bytes, void* p_user_data) {
        OpjMemStream* stream = (OpjMemStream*)p_user_data;
        if (p_nb_bytes < 0 || (OPJ_SIZE_T)p_nb_bytes > stream->size) return OPJ_FALSE;
        stream->pos = (size_t)p_nb_bytes;
        return OPJ_TRUE;
    }
#endif

    bool PdfFilters::JPEG2000Decode(const std::vector<uint8_t>& input,
        std::vector<uint8_t>& argbOut,
        int& width,
        int& height)
    {
        if (input.empty()) return false;

#ifdef USE_OPENJPEG
        // Debug log
        static FILE* jp2Debug = nullptr;
        if (!jp2Debug) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat_s(tempPath, MAX_PATH, "jpeg2000_debug.txt");
            fopen_s(&jp2Debug, tempPath, "w");
        }

        if (jp2Debug) {
            fprintf(jp2Debug, "=== JPEG2000Decode (OpenJPEG) ===\n");
            fprintf(jp2Debug, "Input size: %zu bytes\n", input.size());
            fflush(jp2Debug);
        }

        // Determine codec format from signature
        OPJ_CODEC_FORMAT codecFormat = OPJ_CODEC_JP2;  // Default to JP2

        // Check for JP2 signature: 0000 000C 6A50 2020
        if (input.size() >= 12 &&
            input[0] == 0x00 && input[1] == 0x00 && input[2] == 0x00 && input[3] == 0x0C &&
            input[4] == 0x6A && input[5] == 0x50 && input[6] == 0x20 && input[7] == 0x20) {
            codecFormat = OPJ_CODEC_JP2;
        }
        // Check for J2K codestream signature: FF4F FF51
        else if (input.size() >= 2 && input[0] == 0xFF && input[1] == 0x4F) {
            codecFormat = OPJ_CODEC_J2K;
        }

        // Create codec
        opj_codec_t* codec = opj_create_decompress(codecFormat);
        if (!codec) return false;

        // Setup decoder parameters
        opj_dparameters_t params;
        opj_set_default_decoder_parameters(&params);

        if (!opj_setup_decoder(codec, &params)) {
            opj_destroy_codec(codec);
            return false;
        }

        // Create memory stream
        OpjMemStream memStream;
        memStream.data = input.data();
        memStream.size = input.size();
        memStream.pos = 0;

        opj_stream_t* stream = opj_stream_create(input.size(), OPJ_TRUE);
        if (!stream) {
            opj_destroy_codec(codec);
            return false;
        }

        opj_stream_set_user_data(stream, &memStream, nullptr);
        opj_stream_set_user_data_length(stream, input.size());
        opj_stream_set_read_function(stream, opj_mem_read);
        opj_stream_set_skip_function(stream, opj_mem_skip);
        opj_stream_set_seek_function(stream, opj_mem_seek);

        // Read header
        opj_image_t* image = nullptr;
        if (!opj_read_header(stream, codec, &image)) {
            opj_stream_destroy(stream);
            opj_destroy_codec(codec);
            return false;
        }

        // Decode image
        if (!opj_decode(codec, stream, image)) {
            if (jp2Debug) {
                fprintf(jp2Debug, "ERROR: opj_decode failed\n");
                fflush(jp2Debug);
            }
            opj_image_destroy(image);
            opj_stream_destroy(stream);
            opj_destroy_codec(codec);
            return false;
        }

        // Get image dimensions
        width = image->x1 - image->x0;
        height = image->y1 - image->y0;

        if (jp2Debug) {
            fprintf(jp2Debug, "Decoded: %dx%d, numcomps=%d, color_space=%d\n",
                width, height, image->numcomps, image->color_space);
            for (int c = 0; c < image->numcomps && c < 4; c++) {
                fprintf(jp2Debug, "  Comp[%d]: w=%d h=%d prec=%d sgnd=%d dx=%d dy=%d\n",
                    c, image->comps[c].w, image->comps[c].h,
                    image->comps[c].prec, image->comps[c].sgnd,
                    image->comps[c].dx, image->comps[c].dy);
            }
            // Sample some pixel values
            if (image->numcomps >= 3 && image->comps[0].data) {
                int midIdx = (height / 2) * width + (width / 2);
                fprintf(jp2Debug, "  Center pixel raw: R=%d G=%d B=%d\n",
                    image->comps[0].data[midIdx],
                    image->comps[1].data[midIdx],
                    image->comps[2].data[midIdx]);
            }
            fflush(jp2Debug);
        }

        if (width <= 0 || height <= 0 || image->numcomps < 1) {
            opj_image_destroy(image);
            opj_stream_destroy(stream);
            opj_destroy_codec(codec);
            return false;
        }

        // Convert to ARGB
        argbOut.resize(width * height * 4);

        int numComps = image->numcomps;
        bool hasAlpha = (numComps >= 4);

        // Check color space - OPJ_CLRSPC_SRGB = 1, OPJ_CLRSPC_SYCC = 3
        bool isYCC = (image->color_space == OPJ_CLRSPC_SYCC ||
            image->color_space == OPJ_CLRSPC_EYCC);

        if (jp2Debug) {
            fprintf(jp2Debug, "Processing as %s, hasAlpha=%d\n",
                isYCC ? "YCbCr" : "RGB", hasAlpha ? 1 : 0);
            fflush(jp2Debug);
        }

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int dstIdx = (y * width + x) * 4;

                // Get component values (handle different bit depths, signed data, and subsampling)
                auto getComp = [&](int comp) -> int {
                    if (comp >= numComps) return (comp == 3) ? 255 : 0;

                    // Handle subsampling - component may have different dimensions
                    int cx = x;
                    int cy = y;
                    if (image->comps[comp].dx > 1) cx = x / image->comps[comp].dx;
                    if (image->comps[comp].dy > 1) cy = y / image->comps[comp].dy;

                    int compW = image->comps[comp].w;
                    int compH = image->comps[comp].h;
                    if (cx >= compW) cx = compW - 1;
                    if (cy >= compH) cy = compH - 1;

                    int srcIdx = cy * compW + cx;
                    int val = image->comps[comp].data[srcIdx];

                    // Handle signed components
                    if (image->comps[comp].sgnd) {
                        val += (1 << (image->comps[comp].prec - 1));
                    }

                    // Normalize to 8-bit
                    int prec = image->comps[comp].prec;
                    if (prec > 8) val >>= (prec - 8);
                    else if (prec < 8) val <<= (8 - prec);

                    return std::min(255, std::max(0, val));
                    };

                uint8_t r, g, b, a;

                if (numComps == 1) {
                    // Grayscale
                    uint8_t gray = (uint8_t)getComp(0);
                    r = g = b = gray;
                    a = 255;
                }
                else if (isYCC && numComps >= 3) {
                    // YCbCr to RGB conversion
                    int Y = getComp(0);
                    int Cb = getComp(1) - 128;
                    int Cr = getComp(2) - 128;

                    // ITU-R BT.601 conversion
                    int ri = Y + ((1.402 * Cr) + 0.5);
                    int gi = Y - ((0.344136 * Cb) + (0.714136 * Cr) + 0.5);
                    int bi = Y + ((1.772 * Cb) + 0.5);

                    r = (uint8_t)std::min(255, std::max(0, ri));
                    g = (uint8_t)std::min(255, std::max(0, gi));
                    b = (uint8_t)std::min(255, std::max(0, bi));
                    a = hasAlpha ? (uint8_t)getComp(3) : 255;
                }
                else {
                    // RGB or RGBA
                    r = (uint8_t)getComp(0);
                    g = (uint8_t)getComp(1);
                    b = (uint8_t)getComp(2);
                    a = hasAlpha ? (uint8_t)getComp(3) : 255;
                }

                argbOut[dstIdx + 0] = r; // R
                argbOut[dstIdx + 1] = g; // G
                argbOut[dstIdx + 2] = b; // B
                argbOut[dstIdx + 3] = a; // A (RGBA format for createBitmapFromARGB)
            }
        }

        // Debug: log some output pixel values
        if (jp2Debug) {
            int midIdx = ((height / 2) * width + (width / 2)) * 4;
            fprintf(jp2Debug, "Output center pixel: R=%d G=%d B=%d A=%d\n",
                argbOut[midIdx + 0], argbOut[midIdx + 1],
                argbOut[midIdx + 2], argbOut[midIdx + 3]);

            // Check a few more pixels
            int cornerIdx = 0;
            fprintf(jp2Debug, "Output top-left pixel: R=%d G=%d B=%d A=%d\n",
                argbOut[cornerIdx + 0], argbOut[cornerIdx + 1],
                argbOut[cornerIdx + 2], argbOut[cornerIdx + 3]);
            fflush(jp2Debug);
        }

        opj_image_destroy(image);
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);

        return true;
#else
        // OpenJPEG not available - JPEG 2000 not supported
        // Log error for debugging
#ifdef _WIN32
        static FILE* jp2Debug = nullptr;
        if (!jp2Debug) {
            char tempPath[MAX_PATH];
            GetTempPathA(MAX_PATH, tempPath);
            strcat_s(tempPath, MAX_PATH, "jpeg2000_debug.txt");
            fopen_s(&jp2Debug, tempPath, "w");
        }
        if (jp2Debug) {
            fprintf(jp2Debug, "JPEG 2000 decode failed: OpenJPEG not compiled in.\n");
            fprintf(jp2Debug, "To enable JPEG 2000 support:\n");
            fprintf(jp2Debug, "1. Install OpenJPEG: vcpkg install openjpeg:x64-windows\n");
            fprintf(jp2Debug, "2. Add USE_OPENJPEG to preprocessor definitions\n");
            fprintf(jp2Debug, "3. Link against openjp2.lib\n");
            fflush(jp2Debug);
        }
#endif
        return false;
#endif
    }

    // =========================================================================
    // CCITTFaxDecode - Group 4 Fax Decompression (ITU-T T.6)
    // =========================================================================

    // White terminating codes (0-63)
    static const int WhiteTermBits[] = {
        8, 6, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6,
        6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8
    };
    static const int WhiteTermCodes[] = {
        0x35, 0x07, 0x07, 0x08, 0x0B, 0x0C, 0x0E, 0x0F,
        0x13, 0x14, 0x07, 0x08, 0x08, 0x03, 0x34, 0x35,
        0x2A, 0x2B, 0x27, 0x0C, 0x08, 0x17, 0x03, 0x04,
        0x28, 0x2B, 0x13, 0x24, 0x18, 0x02, 0x03, 0x1A,
        0x1B, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x28,
        0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x04, 0x05, 0x0A,
        0x0B, 0x52, 0x53, 0x54, 0x55, 0x24, 0x25, 0x58,
        0x59, 0x5A, 0x5B, 0x4A, 0x4B, 0x32, 0x33, 0x34
    };

    // Black terminating codes (0-63)
    static const int BlackTermBits[] = {
        10, 3, 2, 2, 3, 4, 4, 5, 6, 6, 7, 7, 7, 8, 8, 9,
        10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12
    };
    static const int BlackTermCodes[] = {
        0x37, 0x02, 0x03, 0x02, 0x03, 0x03, 0x02, 0x03,
        0x05, 0x04, 0x04, 0x05, 0x07, 0x04, 0x07, 0x18,
        0x17, 0x18, 0x08, 0x67, 0x68, 0x6C, 0x37, 0x28,
        0x17, 0x18, 0xCA, 0xCB, 0xCC, 0xCD, 0x68, 0x69,
        0x6A, 0x6B, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
        0x6C, 0x6D, 0xDA, 0xDB, 0x54, 0x55, 0x56, 0x57,
        0x64, 0x65, 0x52, 0x53, 0x24, 0x37, 0x38, 0x27,
        0x28, 0x58, 0x59, 0x2B, 0x2C, 0x5A, 0x66, 0x67
    };

    // White make-up codes
    static const int WhiteMakeupBits[] = {
        5, 5, 6, 7, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9,
        9, 9, 9, 9, 9, 9, 9, 9, 9, 6, 9, 11, 11, 11, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 12
    };
    static const int WhiteMakeupCodes[] = {
        0x1B, 0x12, 0x17, 0x37, 0x36, 0x37, 0x64, 0x65,
        0x68, 0x67, 0xCC, 0xCD, 0xD2, 0xD3, 0xD4, 0xD5,
        0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0x98, 0x99,
        0x9A, 0x18, 0x9B, 0x08, 0x0C, 0x0D, 0x12, 0x13,
        0x14, 0x15, 0x16, 0x17, 0x1C, 0x1D, 0x1E, 0x1F
    };
    static const int WhiteMakeupLens[] = {
        64, 128, 192, 256, 320, 384, 448, 512, 576, 640, 704, 768,
        832, 896, 960, 1024, 1088, 1152, 1216, 1280, 1344, 1408,
        1472, 1536, 1600, 1664, 1728, 1792, 1856, 1920, 1984, 2048,
        2112, 2176, 2240, 2304, 2368, 2432, 2496, 2560
    };

    // Black make-up codes
    static const int BlackMakeupBits[] = {
        10, 12, 12, 12, 12, 12, 12, 13, 13, 13, 13, 13,
        13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
        13, 13, 13, 11, 11, 11, 12, 12, 12, 12, 12, 12,
        12, 12, 12, 12
    };
    static const int BlackMakeupCodes[] = {
        0x0F, 0xC8, 0xC9, 0x5B, 0x33, 0x34, 0x35, 0x6C,
        0x6D, 0x4A, 0x4B, 0x4C, 0x4D, 0x72, 0x73, 0x74,
        0x75, 0x76, 0x77, 0x52, 0x53, 0x54, 0x55, 0x5A,
        0x5B, 0x64, 0x65, 0x08, 0x0C, 0x0D, 0x12, 0x13,
        0x14, 0x15, 0x16, 0x17, 0x1C, 0x1D, 0x1E, 0x1F
    };
    static const int BlackMakeupLens[] = {
        64, 128, 192, 256, 320, 384, 448, 512, 576, 640, 704, 768,
        832, 896, 960, 1024, 1088, 1152, 1216, 1280, 1344, 1408,
        1472, 1536, 1600, 1664, 1728, 1792, 1856, 1920, 1984, 2048,
        2112, 2176, 2240, 2304, 2368, 2432, 2496, 2560
    };

    class G4BitReader {
    public:
        G4BitReader(const uint8_t* data, size_t len) : _data(data), _len(len), _byte(0), _bit(0) {}

        int getBits(int n) {
            int result = 0;
            for (int i = 0; i < n; i++) {
                if (_byte >= _len) return -1;
                result = (result << 1) | ((_data[_byte] >> (7 - _bit)) & 1);
                if (++_bit == 8) { _bit = 0; _byte++; }
            }
            return result;
        }

        int peekBits(int n) {
            size_t saveByte = _byte;
            int saveBit = _bit;
            int result = getBits(n);
            _byte = saveByte;
            _bit = saveBit;
            return result;
        }

        void skipBits(int n) {
            for (int i = 0; i < n; i++) {
                if (_byte >= _len) return;
                if (++_bit == 8) { _bit = 0; _byte++; }
            }
        }

        bool eof() const { return _byte >= _len; }

        void align() {
            if (_bit != 0) { _bit = 0; _byte++; }
        }

    private:
        const uint8_t* _data;
        size_t _len;
        size_t _byte;
        int _bit;
    };

    static int decodeRun(G4BitReader& r, bool white) {
        int total = 0;

        // Try makeup codes first
        while (true) {
            bool found = false;
            int numMakeup = 40;
            const int* mkBits = white ? WhiteMakeupBits : BlackMakeupBits;
            const int* mkCodes = white ? WhiteMakeupCodes : BlackMakeupCodes;
            const int* mkLens = white ? WhiteMakeupLens : BlackMakeupLens;

            for (int i = 0; i < numMakeup; i++) {
                int bits = mkBits[i];
                int code = r.peekBits(bits);
                if (code == mkCodes[i]) {
                    r.skipBits(bits);
                    total += mkLens[i];
                    found = true;
                    break;
                }
            }
            if (!found) break;
        }

        // Now terminating code
        const int* termBits = white ? WhiteTermBits : BlackTermBits;
        const int* termCodes = white ? WhiteTermCodes : BlackTermCodes;

        for (int i = 0; i < 64; i++) {
            int bits = termBits[i];
            int code = r.peekBits(bits);
            if (code == termCodes[i]) {
                r.skipBits(bits);
                total += i;
                return total;
            }
        }

        // Error - skip one bit
        r.skipBits(1);
        return total;
    }

    // Mode codes for Group 4
    enum G4Mode { PASS, HORIZ, V0, VR1, VL1, VR2, VL2, VR3, VL3, EOFB, ERR };

    static G4Mode decodeMode(G4BitReader& r) {
        if (r.eof()) return ERR;

        // V(0): 1
        if (r.peekBits(1) == 1) {
            r.skipBits(1);
            return V0;
        }

        // 0xx patterns
        int b3 = r.peekBits(3);
        if (b3 == 0x03) { r.skipBits(3); return VR1; }  // 011
        if (b3 == 0x02) { r.skipBits(3); return VL1; }  // 010
        if (b3 == 0x01) { r.skipBits(3); return HORIZ; } // 001

        // 0001 = Pass
        int b4 = r.peekBits(4);
        if (b4 == 0x01) { r.skipBits(4); return PASS; }

        // 000011 = VR2, 000010 = VL2
        int b6 = r.peekBits(6);
        if (b6 == 0x03) { r.skipBits(6); return VR2; }
        if (b6 == 0x02) { r.skipBits(6); return VL2; }

        // 0000011 = VR3, 0000010 = VL3
        int b7 = r.peekBits(7);
        if (b7 == 0x03) { r.skipBits(7); return VR3; }
        if (b7 == 0x02) { r.skipBits(7); return VL3; }

        // EOFB check (many zeros)
        int b12 = r.peekBits(12);
        if (b12 == 0x001) { r.skipBits(12); return EOFB; }

        return ERR;
    }

    bool PdfFilters::CCITTFaxDecode(
        const std::vector<uint8_t>& input,
        std::vector<uint8_t>& output,
        int columns,
        int rows,
        int k,
        bool blackIs1,
        bool endOfLine,
        bool encodedByteAlign)
    {
        if (input.empty() || columns <= 0) return false;

        int rowBytes = (columns + 7) / 8;
        if (rows <= 0) rows = (int)(input.size() * 8 / columns) + 10;

        // DeviceGray 1-bit convention: 0 = black, 1 = white
        // Start with all white (0xFF), then clear bits for black pixels
        output.clear();
        output.resize((size_t)rowBytes * rows, 0xFF);

        G4BitReader reader(input.data(), input.size());

        std::vector<int> refLine;
        refLine.push_back(columns);

        std::vector<int> curLine;

        // Set a pixel to BLACK by clearing the bit (DeviceGray: 0 = black)
        auto setBlackPixel = [&](int r, int x) {
            if (x < 0 || x >= columns || r < 0 || r >= rows) return;
            int idx = r * rowBytes + x / 8;
            int mask = 0x80 >> (x % 8);
            output[idx] &= ~mask;  // Clear bit = black
            };

        auto findB1 = [&](int a0, bool curWhite) -> int {
            for (size_t i = 0; i < refLine.size(); i++) {
                if (refLine[i] > a0) {
                    bool blackStarts = (i % 2 == 0);
                    if (curWhite == blackStarts) {
                        return refLine[i];
                    }
                }
            }
            return columns;
            };

        auto findB2 = [&](int b1) -> int {
            for (size_t i = 0; i < refLine.size(); i++) {
                if (refLine[i] > b1) return refLine[i];
            }
            return columns;
            };

        int row = 0;
        while (row < rows && !reader.eof()) {
            curLine.clear();
            int a0 = -1;       // Current position (-1 = before first pixel)
            bool white = true; // Current color (starts white)

            if (k == 0) {
                // Group 3 1D
                a0 = 0;
                while (a0 < columns && !reader.eof()) {
                    if (endOfLine && reader.peekBits(12) == 0x001) {
                        reader.skipBits(12);
                        break;
                    }

                    int run = decodeRun(reader, white);
                    int a1 = a0 + run;
                    if (a1 > columns) a1 = columns;

                    // Set black pixels
                    if (!white) {
                        for (int x = a0; x < a1; x++) setBlackPixel(row, x);
                    }

                    if (a1 >= 0 && a1 < columns) curLine.push_back(a1);
                    a0 = a1;
                    white = !white;
                }
                if (encodedByteAlign) reader.align();
            }
            else {
                // Group 4 (2D)
                while (a0 < columns && !reader.eof()) {
                    int b1 = findB1(a0, white);
                    int b2 = findB2(b1);

                    G4Mode mode = decodeMode(reader);
                    if (mode == EOFB || mode == ERR) break;

                    int start = (a0 < 0) ? 0 : a0;

                    switch (mode) {
                    case PASS:
                        // Draw from a0 to b2, color unchanged
                        if (!white) {
                            for (int x = start; x < b2 && x < columns; x++)
                                setBlackPixel(row, x);
                        }
                        a0 = b2;
                        // white stays same
                        break;

                    case HORIZ:
                    {
                        int r1 = decodeRun(reader, white);
                        int r2 = decodeRun(reader, !white);

                        int a1 = start + r1;
                        int a2 = a1 + r2;
                        if (a1 > columns) a1 = columns;
                        if (a2 > columns) a2 = columns;

                        // First run: color = white (so if !white, it's black)
                        if (!white) {
                            for (int x = start; x < a1; x++)
                                setBlackPixel(row, x);
                        }
                        // Always add a1 as change point (even if a1 == start == 0)
                        if (a1 >= 0 && a1 < columns) curLine.push_back(a1);

                        // Second run: color = !white (so if white, it's black)
                        if (white) {
                            for (int x = a1; x < a2; x++)
                                setBlackPixel(row, x);
                        }
                        // Add a2 if different from a1
                        if (a2 > a1 && a2 <= columns) curLine.push_back(a2);

                        a0 = a2;
                        // white stays same after horizontal
                    }
                    break;

                    case V0: case VR1: case VL1: case VR2: case VL2: case VR3: case VL3:
                    {
                        int offset = 0;
                        switch (mode) {
                        case V0: offset = 0; break;
                        case VR1: offset = 1; break;
                        case VL1: offset = -1; break;
                        case VR2: offset = 2; break;
                        case VL2: offset = -2; break;
                        case VR3: offset = 3; break;
                        case VL3: offset = -3; break;
                        default: break;
                        }
                        int a1 = b1 + offset;
                        if (a1 < 0) a1 = 0;
                        if (a1 > columns) a1 = columns;

                        // Draw from a0 to a1 with current color
                        if (!white) {
                            for (int x = start; x < a1; x++)
                                setBlackPixel(row, x);
                        }

                        // Always add change point (even if a1 == 0)
                        if (a1 >= 0 && a1 < columns) curLine.push_back(a1);

                        a0 = a1;
                        white = !white; // Color changes
                    }
                    break;

                    default:
                        break;
                    }
                }
            }

            // Finish line
            if (curLine.empty() || curLine.back() != columns)
                curLine.push_back(columns);

            refLine = curLine;
            row++;
        }

        output.resize((size_t)rowBytes * row);
        return true;
    }

    // ---------------------------------------------------------
    // ApplyPredictor - PNG/TIFF Predictor
    // ---------------------------------------------------------
    bool PdfFilters::ApplyPredictor(
        int predictor,
        int colors,
        int bitsPerComponent,
        int columns,
        std::vector<uint8_t>& data)
    {
        if (predictor <= 1)
            return true;

        int bytesPerPixel = (colors * bitsPerComponent + 7) / 8;
        int rowSize = bytesPerPixel * columns;

        // TIFF Predictor 2
        if (predictor == 2)
        {
            size_t numRows = data.size() / rowSize;
            for (size_t row = 0; row < numRows; row++)
            {
                uint8_t* rowPtr = &data[row * rowSize];
                for (int x = bytesPerPixel; x < rowSize; x++)
                {
                    rowPtr[x] = (rowPtr[x] + rowPtr[x - bytesPerPixel]) & 0xFF;
                }
            }
            return true;
        }

        // PNG Predictors (10-15)
        if (predictor < 10)
            return true;

        std::vector<uint8_t> out;
        out.reserve(data.size());

        std::vector<uint8_t> prevRow(rowSize, 0);

        size_t i = 0;

        while (i < data.size())
        {
            if (i >= data.size())
                break;

            uint8_t filterType = data[i++];

            if (i + rowSize > data.size())
            {
                while (i < data.size())
                    out.push_back(data[i++]);
                break;
            }

            const uint8_t* row = &data[i];
            i += rowSize;

            std::vector<uint8_t> decoded(rowSize);

            switch (filterType)
            {
            case 0: // None
                std::copy(row, row + rowSize, decoded.begin());
                break;

            case 1: // Sub
                for (int x = 0; x < rowSize; x++)
                {
                    uint8_t left = (x >= bytesPerPixel) ? decoded[x - bytesPerPixel] : 0;
                    decoded[x] = (row[x] + left) & 0xFF;
                }
                break;

            case 2: // Up
                for (int x = 0; x < rowSize; x++)
                {
                    decoded[x] = (row[x] + prevRow[x]) & 0xFF;
                }
                break;

            case 3: // Average
                for (int x = 0; x < rowSize; x++)
                {
                    uint8_t left = (x >= bytesPerPixel) ? decoded[x - bytesPerPixel] : 0;
                    uint8_t up = prevRow[x];
                    decoded[x] = (row[x] + ((left + up) >> 1)) & 0xFF;
                }
                break;

            case 4: // Paeth
                for (int x = 0; x < rowSize; x++)
                {
                    uint8_t left = (x >= bytesPerPixel) ? decoded[x - bytesPerPixel] : 0;
                    uint8_t up = prevRow[x];
                    uint8_t upLeft = (x >= bytesPerPixel) ? prevRow[x - bytesPerPixel] : 0;

                    int p = (int)left + (int)up - (int)upLeft;
                    int pa = std::abs(p - (int)left);
                    int pb = std::abs(p - (int)up);
                    int pc = std::abs(p - (int)upLeft);

                    uint8_t pr;
                    if (pa <= pb && pa <= pc)
                        pr = left;
                    else if (pb <= pc)
                        pr = up;
                    else
                        pr = upLeft;

                    decoded[x] = (row[x] + pr) & 0xFF;
                }
                break;

            default:
                std::copy(row, row + rowSize, decoded.begin());
                break;
            }

            out.insert(out.end(), decoded.begin(), decoded.end());
            prevRow = decoded;
        }

        data.swap(out);
        return true;
    }


    bool PdfFilters::Decode(
        const std::vector<uint8_t>& input,
        const std::vector<std::string>& filters,
        const std::vector<std::map<std::string, int>>& params,
        std::vector<uint8_t>& output)
    {
        std::vector<uint8_t> data = input;

        for (size_t i = 0; i < filters.size(); i++)
        {
            std::string f = NormalizeFilterName(filters[i]);
            const auto& p = (i < params.size()) ? params[i] : std::map<std::string, int>();

            std::vector<uint8_t> temp;

            if (f == "/FlateDecode")
            {
                if (!FlateDecode(data, temp))
                    return false;

                if (HasParam(p, "Predictor"))
                {
                    int predictor = GetParam(p, "Predictor", 1);
                    int colors = GetParam(p, "Colors", 1);
                    int bits = GetParam(p, "BitsPerComponent", 8);
                    int cols = GetParam(p, "Columns", 1);
                    ApplyPredictor(predictor, colors, bits, cols, temp);
                }
            }
            else if (f == "/LZWDecode")
            {
                LZWDecode(data, temp);

                if (HasParam(p, "Predictor"))
                {
                    int predictor = GetParam(p, "Predictor", 1);
                    int colors = GetParam(p, "Colors", 1);
                    int bits = GetParam(p, "BitsPerComponent", 8);
                    int cols = GetParam(p, "Columns", 1);
                    ApplyPredictor(predictor, colors, bits, cols, temp);
                }
            }
            else if (f == "/DCTDecode")
            {
                temp = data;
            }
            else if (f == "/JPXDecode")
            {
                temp = data;
            }
            else if (f == "/CCITTFaxDecode")
            {
                temp = data;
            }
            else if (f == "/ASCII85Decode")
            {
                ASCII85Decode(data, temp);
            }
            else if (f == "/RunLengthDecode")
            {
                RunLengthDecode(data, temp);
            }
            else if (f == "/ASCIIHexDecode")
            {
                temp.clear();
                for (size_t j = 0; j + 1 < data.size(); j += 2)
                {
                    char hex[3] = { (char)data[j], (char)data[j + 1], 0 };
                    temp.push_back((uint8_t)strtol(hex, nullptr, 16));
                }
            }
            else
            {
                temp = data;
            }

            data.swap(temp);
        }

        output = data;
        return true;
    }

}
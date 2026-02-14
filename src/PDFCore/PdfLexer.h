#pragma once

#include <vector>
#include <string>
#include <cstdint>

namespace pdf
{
    enum class TokenType
    {
        EndOfFile,
        Number,
        String,
        HexString,
        Name,
        Keyword,
        Delimiter
    };

    struct Token
    {
        TokenType type{ TokenType::EndOfFile };
        std::string text;
    };

    class PdfLexer
    {
    public:
        PdfLexer(const std::vector<uint8_t>& data);

        Token nextToken();
        Token peekToken();

        void setPosition(size_t pos);
        size_t getPosition() const { return _pos; }

    private:
        const std::vector<uint8_t>& _data;
        size_t _pos{ 0 };

        bool _hasPeek{ false };
        Token _peekToken;

        void skipWhitespace();
        Token readNumber();
        Token readName();
        Token readString();
        Token readHexString();
        Token readKeywordOrDelimiter();
    };
}

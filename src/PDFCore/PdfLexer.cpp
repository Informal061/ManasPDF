#include "pch.h"
#include "PdfLexer.h"
#include "PdfDebug.h"
#include <cctype>

namespace pdf
{
    PdfLexer::PdfLexer(const std::vector<uint8_t>& data)
        : _data(data)
        , _pos(0)
        , _hasPeek(false)
    {
        _peekToken = {}; // varsa
    }


    void PdfLexer::setPosition(size_t pos)
    {
        _pos = pos;
        _hasPeek = false;
    }

    void PdfLexer::skipWhitespace()
    {
        while (_pos < _data.size())
        {
            unsigned char c = _data[_pos];
            if (c == '%')
            {
                // Yorum satırı: satır sonuna kadar atla
                while (_pos < _data.size() && _data[_pos] != '\n' && _data[_pos] != '\r')
                    ++_pos;
            }
            else if (std::isspace(c))
            {
                ++_pos;
            }
            else
            {
                break;
            }
        }
    }

    Token PdfLexer::peekToken()
    {
        if (_hasPeek)
            return _peekToken;

        _peekToken = nextToken();
        _hasPeek = true;
        return _peekToken;
    }

    Token PdfLexer::nextToken()
    {
        if (_hasPeek)
        {
            _hasPeek = false;
            return _peekToken;
        }

        skipWhitespace();

        Token tok{};
        tok.type = TokenType::EndOfFile;
        tok.text.clear();

        if (_pos >= _data.size())
            return tok;

        unsigned char c = _data[_pos];

        if (std::isdigit(c) || c == '+' || c == '-' || c == '.')
            return readNumber();

        if (c == '/')
            return readName();

        if (c == '(')
            return readString();

        return readKeywordOrDelimiter();
    }


    Token PdfLexer::readNumber()
    {
        Token tok;
        tok.type = TokenType::Number;
        std::string s;

        // GÜVENLİK: Sayılar aşırı uzun olamaz (Max 255 karakter)
        size_t limit = 0;

        unsigned char c = _data[_pos];
        if (c == '+' || c == '-' || c == '.')
        {
            s.push_back(c);
            ++_pos;
        }

        while (_pos < _data.size() && limit++ < 255)
        {
            c = _data[_pos];
            if (std::isdigit(c) || c == '.')
            {
                s.push_back(c);
                ++_pos;
            }
            else
            {
                break;
            }
        }

        tok.text = s;
        return tok;
    }

    Token PdfLexer::readName()
    {
        Token tok;
        tok.type = TokenType::Name;
        std::string s;

        // Leading '/'
        s.push_back('/');
        ++_pos;

        // GÜVENLİK: Binary veri okuyorsak sonsuz döngüye girmemeli (Max 1024 karakter)
        size_t limit = 0;

        while (_pos < _data.size() && limit++ < 1024)
        {
            unsigned char c = _data[_pos];
            if (std::isspace(c) || c == '/' || c == '(' || c == ')' ||
                c == '<' || c == '>' || c == '[' || c == ']')
            {
                break;
            }
            s.push_back(static_cast<char>(c));
            ++_pos;
        }

        tok.text = s;
        return tok;
    }

    // -------------------------------------------------------------
    // DÜZELTİLEN FONKSİYON: STRING OKUMA GÜVENLİĞİ
    // OCTAL ESCAPE SEQUENCES (\ddd) DESTEĞİ EKLENDİ
    // -------------------------------------------------------------
    Token PdfLexer::readString()
    {
        Token tok;
        tok.type = TokenType::String;
        std::string s;

        ++_pos;
        int depth = 1;

        size_t limit = 0;
        const size_t MAX_STRING_LEN = 65535;
        size_t startPos = _pos;

        while (_pos < _data.size() && depth > 0)
        {
            if (++limit > MAX_STRING_LEN)
            {
                break;
            }

            char c = static_cast<char>(_data[_pos++]);

            if (c == '\\')
            {
                if (_pos < _data.size())
                {
                    char n = static_cast<char>(_data[_pos]);

                    // OCTAL ESCAPE: \ddd (1-3 oktal rakam: 0-7)
                    // PDF spec: backslash followed by 1-3 octal digits
                    if (n >= '0' && n <= '7')
                    {
                        int octalValue = 0;
                        int digitCount = 0;

                        // En fazla 3 oktal rakam oku
                        while (_pos < _data.size() && digitCount < 3)
                        {
                            char d = static_cast<char>(_data[_pos]);
                            if (d >= '0' && d <= '7')
                            {
                                octalValue = octalValue * 8 + (d - '0');
                                ++_pos;
                                ++digitCount;
                            }
                            else
                            {
                                break;
                            }
                        }

                        // Append as byte (0-255 range)
                        s.push_back(static_cast<char>(octalValue & 0xFF));
                    }
                    else
                    {
                        // Other escape characters
                        ++_pos;
                        switch (n)
                        {
                        case 'n': s.push_back('\n'); break;
                        case 'r': s.push_back('\r'); break;
                        case 't': s.push_back('\t'); break;
                        case 'b': s.push_back('\b'); break;
                        case 'f': s.push_back('\f'); break;
                        case '\\': s.push_back('\\'); break;
                        case '(': s.push_back('('); break;
                        case ')': s.push_back(')'); break;
                        case '\r':
                            // \<CR> veya \<CR><LF> → line continuation, append nothing
                            if (_pos < _data.size() && _data[_pos] == '\n')
                                ++_pos;
                            break;
                        case '\n':
                            // \<LF> → line continuation, append nothing
                            break;
                        default:
                            // Unknown escape: ignore backslash, append character
                            s.push_back(n);
                            break;
                        }
                    }
                }
            }
            else if (c == '(')
            {
                depth++;
                s.push_back(c);
            }
            else if (c == ')')
            {
                depth--;
                if (depth > 0)
                    s.push_back(c);
            }
            else
            {
                s.push_back(c);
            }
        }

        tok.text = s;
        return tok;
    }

    // -------------------------------------------------------------
    // HEX STRING OKUMA: <48656C6C6F> → "Hello"
    // PDF spec: hex digits between < and >, whitespace ignored
    // -------------------------------------------------------------
    Token PdfLexer::readHexString()
    {
        Token tok;
        tok.type = TokenType::HexString;
        std::string hexChars;

        ++_pos; // '<' karakterini atla

        size_t limit = 0;
        const size_t MAX_HEX_LEN = 131072; // 128KB hex = 64KB binary

        while (_pos < _data.size() && limit++ < MAX_HEX_LEN)
        {
            unsigned char c = _data[_pos];
            if (c == '>')
            {
                ++_pos;
                break;
            }
            if (std::isspace(c))
            {
                ++_pos;
                continue; // whitespace ignored in hex strings
            }
            if (std::isxdigit(c))
            {
                hexChars.push_back(static_cast<char>(c));
                ++_pos;
            }
            else
            {
                // Invalid hex char — skip
                ++_pos;
            }
        }

        // Tek sayıda hex digit varsa sonuna 0 ekle (PDF spec)
        if (hexChars.size() % 2 != 0)
            hexChars.push_back('0');

        // Hex → binary bytes
        std::string result;
        for (size_t i = 0; i + 1 < hexChars.size(); i += 2)
        {
            char hi = hexChars[i];
            char lo = hexChars[i + 1];
            auto hexVal = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
                if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
                return 0;
            };
            result.push_back(static_cast<char>((hexVal(hi) << 4) | hexVal(lo)));
        }

        tok.text = result;
        return tok;
    }

    Token PdfLexer::readKeywordOrDelimiter()
    {
        Token tok;
        unsigned char c = _data[_pos];

        // Çift karakterli delimiterler: <<, >>
        if (c == '<')
        {
            if (_pos + 1 < _data.size() && _data[_pos + 1] == '<')
            {
                tok.type = TokenType::Delimiter;
                tok.text = "<<";
                _pos += 2;
                return tok;
            }
            else
            {
                // Hex string: <ABCDEF0123...>
                return readHexString();
            }
        }

        if (c == '>')
        {
            if (_pos + 1 < _data.size() && _data[_pos + 1] == '>')
            {
                tok.type = TokenType::Delimiter;
                tok.text = ">>";
                _pos += 2;
                return tok;
            }
            else
            {
                tok.type = TokenType::Delimiter;
                tok.text = ">";
                ++_pos;
                return tok;
            }
        }

        // Tek karakterli delimiterler: [ ]
        if (c == '[' || c == ']')
        {
            tok.type = TokenType::Delimiter;
            tok.text = std::string(1, static_cast<char>(c));
            ++_pos;
            return tok;
        }

        // Diğer her şey: Keyword
        tok.type = TokenType::Keyword;
        std::string s;

        // Keyword'ler genelde kısadır
        size_t limit = 0;
        while (_pos < _data.size() && limit++ < 255)
        {
            c = _data[_pos];
            if (std::isspace(c) || c == '/' || c == '(' || c == ')' ||
                c == '<' || c == '>' || c == '[' || c == ']')
            {
                break;
            }
            s.push_back(static_cast<char>(c));
            ++_pos;
        }

        tok.text = s;
        return tok;
    }
}

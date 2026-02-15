#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "PdfObject.h"
#include "PdfParser.h"
#include "PdfGraphicsState.h"
#include <ft2build.h>
#include FT_FREETYPE_H

namespace pdf
{
    class IPdfPainter;
    class PdfPainter;
    class PdfPainterGPU;

    // Link annotation info
    struct PdfLinkInfo
    {
        double x1, y1, x2, y2;  // Bounding box in PDF points (user space)
        std::string uri;         // External URI (http://, mailto:, etc.)
        int destPage = -1;       // Internal destination page (-1 if external link)
    };

    struct PdfFontInfo
    {
        std::string resourceName;
        std::string subtype;
        std::string baseFont;
        std::string encoding;
        bool hasCidToGidMap = false;
        bool cidToGidIdentity = true;
        std::vector<uint16_t> cidToGid;

        bool isCidFont = false;

        uint32_t codeToUnicode[256];
        bool hasSimpleMap = false;

        uint16_t codeToGid[256];
        bool hasCodeToGid = false;

        std::string codeToGlyphName[256];

        std::vector<int> widths;
        int firstChar = 0;
        int missingWidth = 500;
        bool hasWidths = false;

        int cidDefaultWidth = 1000;
        std::map<uint16_t, int> cidWidths;

        std::map<uint16_t, uint32_t> cidToUnicode;

        std::vector<uint8_t> fontProgram;
        std::string fontProgramSubtype;

        FT_Face ftFace = nullptr;
        bool ftReady = false;
        size_t fontHash = 0;

        // ===== TYPE3 FONT SUPPORT =====
        bool isType3 = false;
        PdfMatrix type3FontMatrix;  // glyph space → user space (e.g. [0.001 0 0 0.001 0 0])
        std::map<std::string, std::vector<uint8_t>> type3CharProcs;  // glyphName → decoded stream
        std::shared_ptr<PdfDictionary> type3Resources;  // Resources for CharProc execution

        PdfFontInfo()
        {
            for (int i = 0; i < 256; ++i) {
                codeToUnicode[i] = i;
                codeToGid[i] = 0;
            }
        }
    };


    // =======================================================================
    // ASN.1 / DER Element
    // Full production-grade parser: all tag classes, multi-byte tags,
    // indefinite length, constructed types, nested structures
    // =======================================================================
    struct Asn1Element
    {
        uint8_t  tagClass;      // 0=UNIVERSAL, 1=APPLICATION, 2=CONTEXT-SPECIFIC, 3=PRIVATE
        bool     constructed;   // true = CONSTRUCTED (children), false = PRIMITIVE
        uint32_t tagNumber;     // tag number within class

        std::vector<uint8_t>     value;     // raw value bytes (primitive)
        std::vector<Asn1Element> children;  // child elements (constructed)

        size_t headerLength;
        size_t contentLength;
        size_t totalEncodedLength;
        std::vector<uint8_t> rawDer;        // raw DER of entire TLV

        Asn1Element()
            : tagClass(0), constructed(false), tagNumber(0),
            headerLength(0), contentLength(0), totalEncodedLength(0) {
        }

        // --- Type checks ---
        bool isSequence() const { return tagClass == 0 && tagNumber == 0x10 && constructed; }
        bool isSet() const { return tagClass == 0 && tagNumber == 0x11 && constructed; }
        bool isInteger() const { return tagClass == 0 && tagNumber == 0x02 && !constructed; }
        bool isOctetString() const { return tagClass == 0 && tagNumber == 0x04; }
        bool isBitString() const { return tagClass == 0 && tagNumber == 0x03; }
        bool isOid() const { return tagClass == 0 && tagNumber == 0x06 && !constructed; }
        bool isNull() const { return tagClass == 0 && tagNumber == 0x05; }
        bool isBoolean() const { return tagClass == 0 && tagNumber == 0x01 && !constructed; }
        bool isUtf8String() const { return tagClass == 0 && tagNumber == 0x0C; }
        bool isPrintableString() const { return tagClass == 0 && tagNumber == 0x13; }
        bool isIA5String() const { return tagClass == 0 && tagNumber == 0x16; }
        bool isContextTag(uint32_t n) const { return tagClass == 2 && tagNumber == n; }
        bool isExplicitTag(uint32_t n) const { return tagClass == 2 && tagNumber == n && constructed; }
        bool isImplicitTag(uint32_t n) const { return tagClass == 2 && tagNumber == n && !constructed; }

        // --- Value extraction ---
        std::string oidToString() const;
        std::vector<uint8_t> integerBytes() const;
        int integerToInt() const;
        std::string stringValue() const;
        bool booleanValue() const;

        // --- Child access ---
        const Asn1Element* childAt(size_t idx) const { return idx < children.size() ? &children[idx] : nullptr; }
        Asn1Element* childAt(size_t idx) { return idx < children.size() ? &children[idx] : nullptr; }
        const Asn1Element* findContextChild(uint32_t n) const;
        size_t childCount() const { return children.size(); }
        bool hasChildren() const { return !children.empty(); }
    };


    // =======================================================================
    // PKCS#7 / CMS Structures (Certificate Encryption)
    // Supports KeyTransRecipientInfo (RSA, version 0/2)
    // =======================================================================
    struct Pkcs7RecipientInfo
    {
        int version = 0;
        std::vector<uint8_t> issuerDer;         // raw DER of issuer Name
        std::vector<uint8_t> serialNumber;       // big-endian unsigned bytes
        std::string keyEncAlgorithmOid;          // e.g. "1.2.840.113549.1.1.1" (RSA)
        std::vector<uint8_t> keyEncAlgorithmParams;
        std::vector<uint8_t> encryptedKey;       // RSA-encrypted seed
        std::vector<uint8_t> subjectKeyId;       // for version 2 (SubjectKeyIdentifier)
    };

    struct Pkcs7EncryptedContentInfo
    {
        std::string contentTypeOid;
        std::string encAlgorithmOid;
        std::vector<uint8_t> encAlgorithmIv;
        std::vector<uint8_t> encryptedContent;
    };

    struct Pkcs7EnvelopedData
    {
        int version = 0;
        std::vector<Pkcs7RecipientInfo> recipients;
        Pkcs7EncryptedContentInfo encryptedContentInfo;
    };


    // =======================================================================
    // PdfDocument
    // =======================================================================
    class PdfDocument
    {
    public:
        PdfDocument();
        ~PdfDocument();

        bool loadFromBytes(const std::vector<uint8_t>& data);
        const std::map<int, PdfObjectPtr>& getObjects() const { return _objects; }
        std::shared_ptr<PdfDictionary> getPagesNode() const { return _pages; }

        std::shared_ptr<PdfObject> resolve(
            const std::shared_ptr<PdfObject>& obj,
            std::set<int>& visited
        ) const {
            return resolveIndirect(obj, visited);
        }

        bool isPage(const std::shared_ptr<PdfDictionary>& dict) const
        {
            return isPageObject(dict);
        }

        bool loadFallbackFont(PdfFontInfo& fi);
        int getPageCountFromPageTree() const;
        std::shared_ptr<PdfDictionary> getPageDictionary(int pageIndex) const;

        int getPageRotate(std::shared_ptr<PdfDictionary> pageDict) const;
        bool getPageSize(int pageIndex, double& wPt, double& hPt) const;
        bool getDisplayPageSize(int pageIndex, double& wPt, double& hPt) const;
        bool getRawPageSize(int pageIndex, double& wPt, double& hPt) const;
        int  getPageRotate(int pageIndex) const;

        bool renderPageToPainter(int pageIndex, IPdfPainter& painter);
        bool renderPageToPainter(int pageIndex, PdfPainter& painter);
        bool renderPageToPainter(int pageIndex, PdfPainterGPU& painter);

        bool getPageContentsBytes(int pageIndex, std::vector<uint8_t>& out) const;

        bool getPageXObjects(
            int pageIndex,
            std::map<std::string, std::shared_ptr<PdfStream>>& out) const;

        std::shared_ptr<PdfObject> loadObjectAtOffset(size_t offset);

        bool getPageResources(
            int pageIndex,
            std::vector<std::shared_ptr<PdfDictionary>>& outStack) const;

        bool decodeStream(
            const std::shared_ptr<PdfStream>& stream,
            std::vector<uint8_t>& outDecoded) const;

        bool decodeImageXObject(
            const std::shared_ptr<PdfStream>& st,
            std::vector<uint8_t>& argb, int& w, int& h);

        bool getPageFonts(
            int pageIndex,
            std::map<std::string, PdfFontInfo>& out) const;

        bool loadFontsFromResourceDict(
            const std::shared_ptr<PdfDictionary>& resDict,
            std::map<std::string, PdfFontInfo>& fonts) const;

        bool prepareFreeTypeFont(PdfFontInfo& fi);
        FT_Library getFreeTypeLibrary() const;

        // ==================== Link Extraction API ====================
        bool getPageLinks(int pageIndex, std::vector<PdfLinkInfo>& outLinks) const;

        // Named destination çözümleme (GoTo action'larda string /D değeri için)
        std::shared_ptr<PdfArray> resolveNamedDestination(const std::string& name) const;

        // Destination array'den ([pageRef /XYZ ...]) sayfa indeksini çöz
        int resolvePageFromDestArray(const std::shared_ptr<PdfArray>& destArr) const;

        // ==================== Encryption Public API ====================
        // Status: 0 = not encrypted, 1 = encrypted+ready, -1 = needs credentials
        int getEncryptionStatus() const;
        // Type: 0 = none, 1 = password (/Standard), 2 = certificate (/Adobe.PubSec)
        int getEncryptionType() const;
        // Try password for /Standard encryption
        bool tryPassword(const std::string& password);
        // Supply decrypted seed for /Adobe.PubSec
        bool supplySeed(const uint8_t* seed, size_t seedLen);
        // Get recipient list for certificate matching
        const std::vector<Pkcs7RecipientInfo>& getCertRecipients() const;


    private:
        std::vector<uint8_t> _data;
        std::map<int, PdfObjectPtr> _objects;

        std::map<int, size_t> _xrefTable;

        // Object Stream (ObjStm) desteği — XRef type 2 girdileri
        struct ObjStmEntry { int objStmNum; int indexInStream; };
        std::map<int, ObjStmEntry> _objStmEntries;
        std::shared_ptr<PdfObject> loadFromObjStm(int objNum, int objStmNum, int indexInStream);

        std::shared_ptr<PdfDictionary> _trailer;
        std::shared_ptr<PdfDictionary> _root;
        std::shared_ptr<PdfDictionary> _pages;

        // ---- Password encryption (/Standard) ----
        bool _isEncrypted = false;
        bool _encryptionReady = false;
        int  _encryptV = 0;
        int  _encryptR = 0;
        int  _encryptKeyLength = 5;
        std::vector<uint8_t> _encryptKey;
        std::vector<uint8_t> _encryptO;
        std::vector<uint8_t> _encryptU;
        std::vector<uint8_t> _encryptOE;
        std::vector<uint8_t> _encryptUE;
        std::vector<uint8_t> _encryptPerms;
        int32_t _encryptP = 0;
        bool _useAES = false;
        std::string _userPassword;
        std::vector<uint8_t> _fileId;

        // ---- Certificate encryption (/Adobe.PubSec) ----
        bool _isCertEncrypted = false;
        std::string _certSubFilter;
        bool _encryptMetadata = true;
        std::vector<std::vector<uint8_t>> _recipientBlobs;
        Pkcs7EnvelopedData _envelopedData;

        // ---- Encryption internal methods ----
        bool initEncryption();
        bool initCertEncryption(const std::shared_ptr<PdfDictionary>& encryptDict,
            const uint8_t* encData, size_t encLen);
        bool computeEncryptionKey();
        bool computeEncryptionKeyV5();
        bool verifyUserPassword();
        bool verifyUserPasswordV5();
        std::vector<uint8_t> computeObjectKey(int objNum, int genNum) const;
        static void rc4Crypt(const std::vector<uint8_t>& key,
            const uint8_t* input, size_t inputLen,
            std::vector<uint8_t>& output);
        static bool aesDecryptCBC(const std::vector<uint8_t>& key,
            const uint8_t* input, size_t inputLen,
            std::vector<uint8_t>& output);
        void decryptStream(const std::shared_ptr<PdfStream>& stream) const;
        void decryptString(std::shared_ptr<PdfObject>& obj, int objNum, int genNum) const;
        std::vector<uint8_t> parsePdfLiteralString(const uint8_t* data, size_t len,
            size_t startAfterParen, size_t& endPos) const;

        // ---- ASN.1 / PKCS#7 parsing ----
        static bool parseAsn1Element(const uint8_t* data, size_t dataLen,
            size_t& offset, Asn1Element& elem);
        static bool parseAsn1All(const uint8_t* data, size_t dataLen,
            std::vector<Asn1Element>& elements);
        static bool parsePkcs7EnvelopedData(const uint8_t* data, size_t dataLen,
            Pkcs7EnvelopedData& result);

        // ---- General internal methods ----
        std::shared_ptr<PdfObject> resolveIndirect(
            const std::shared_ptr<PdfObject>& obj,
            std::set<int>& visitedIds) const;

        bool isPageObject(const std::shared_ptr<PdfDictionary>& dict) const;
        bool loadXRefTable();
        bool loadTrailer();
        bool loadRootAndPages();
        bool parseXRefTableAt(size_t offset, std::map<int, size_t>& xrefEntries);
        bool parseXRefStreamAt(size_t offset, std::map<int, size_t>& xrefEntries);
        std::shared_ptr<PdfDictionary> parseTrailerAt(size_t xrefOffset);
        int  getPageCountByScan() const;
        int  countPagesRecursive(
            const std::shared_ptr<PdfDictionary>& node,
            std::set<const PdfDictionary*>& visited) const;

        bool getPageContentsBytesInternal(
            int pageIndex, std::vector<uint8_t>& out) const;

        void appendStreamData(
            const std::shared_ptr<PdfStream>& st,
            std::vector<uint8_t>& out) const;

        bool decompressFlate(
            const std::vector<uint8_t>& input,
            std::vector<uint8_t>& output) const;

        bool extractBox(
            const std::shared_ptr<PdfDictionary>& page,
            const std::string& key,
            double& x1, double& y1, double& x2, double& y2) const;

        int extractRotateFromDict(
            const std::shared_ptr<PdfDictionary>& dict) const;
    };

} // namespace pdf
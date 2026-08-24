#include "HwpxArchive.h"

#include "libmzip/crc32.h"
#include "libmzip/mzip_codec.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

constexpr std::uint32_t kLocalHeaderSignature = 0x04034B50u;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014B50u;
constexpr std::uint32_t kEndSignature = 0x06054B50u;
constexpr std::uint16_t kUtf8Flag = 1u << 11;
constexpr std::uint16_t kStoreMethod = 0;
constexpr std::uint16_t kDeflateMethod = 8;
constexpr std::size_t kMaximumArchiveBytes = 512ull * 1024 * 1024;
constexpr std::size_t kMaximumEntryBytes = 128ull * 1024 * 1024;
constexpr std::size_t kMaximumSectionBytes = 32ull * 1024 * 1024;
constexpr std::size_t kMaximumPreviewBytes = 8ull * 1024 * 1024;

// Raw-DEFLATE compressed header.xml from an empty HWPX document. The stock
// Hancom style table is retained for compatibility, then extended below with
// the small set of styles emitted by MdViewer.
constexpr std::string_view kBaseHeaderDeflateBase64 =
    "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";

struct CodecBuffer {
    CodecBuffer() { mzip_codec_buffer_init(&value); }
    ~CodecBuffer() { mzip_codec_buffer_free(&value); }
    CodecBuffer(const CodecBuffer&) = delete;
    CodecBuffer& operator=(const CodecBuffer&) = delete;
    MzipCodecBuffer value{};
};

struct ZipEntry {
    std::string name;
    std::vector<std::uint8_t> bytes;
    bool compress = true;
};

struct CentralRecord {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t expandedSize = 0;
    std::uint32_t offset = 0;
    std::uint16_t method = 0;
};

void Append16(std::string* bytes, std::uint16_t value) {
    bytes->push_back(static_cast<char>(value & 0xFF));
    bytes->push_back(static_cast<char>((value >> 8) & 0xFF));
}

void Append32(std::string* bytes, std::uint32_t value) {
    bytes->push_back(static_cast<char>(value & 0xFF));
    bytes->push_back(static_cast<char>((value >> 8) & 0xFF));
    bytes->push_back(static_cast<char>((value >> 16) & 0xFF));
    bytes->push_back(static_cast<char>((value >> 24) & 0xFF));
}

std::wstring WideError(const std::exception& error) {
    const std::string message = error.what();
    return std::wstring(message.begin(), message.end());
}

std::pair<std::uint16_t, std::uint16_t> CurrentDosTime() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    const int year = (std::max)(1980, local.tm_year + 1900);
    const std::uint16_t date = static_cast<std::uint16_t>(
        ((year - 1980) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
    const std::uint16_t time = static_cast<std::uint16_t>(
        (local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
    return {time, date};
}

bool IsSafeZipName(std::string_view name) {
    if (name.empty() || name.size() > 1024 || name.front() == '/' ||
        name.find('\\') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos) return false;
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t slash = name.find('/', start);
        const std::size_t end = slash == std::string_view::npos
            ? name.size() : slash;
        const std::string_view segment = name.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

std::vector<std::uint8_t> Deflate(const std::vector<std::uint8_t>& input) {
    MzipCodecOptions options{};
    mzip_codec_options_init(&options);
    options.level = 6;
    options.threads = input.size() >= 4ull * 1024 * 1024 ? 0u : 1u;
    CodecBuffer compressed;
    const MzipCodecStatus status = mzip_deflate_alloc(
        input.data(), input.size(), &options, &compressed.value);
    if (status != MZIP_CODEC_OK) {
        throw std::runtime_error(std::string("DEFLATE compression failed: ") +
                                 mzip_codec_status_string(status));
    }
    return std::vector<std::uint8_t>(
        compressed.value.data, compressed.value.data + compressed.value.size);
}

std::string BuildZip(const std::vector<ZipEntry>& entries) {
    if (entries.empty() || entries.size() > 4096 || entries.size() > 0xFFFF) {
        throw std::runtime_error("HWPX archive has an invalid entry count");
    }
    std::size_t total = 0;
    for (const auto& entry : entries) {
        if (!IsSafeZipName(entry.name) || entry.bytes.size() > kMaximumEntryBytes ||
            total > kMaximumArchiveBytes - entry.bytes.size()) {
            throw std::runtime_error("HWPX archive entry exceeds a safety limit");
        }
        total += entry.bytes.size();
    }

    std::string result;
    result.reserve(total + entries.size() * 128 + 22);
    std::vector<CentralRecord> central;
    central.reserve(entries.size());
    const auto [dosTime, dosDate] = CurrentDosTime();

    for (const auto& entry : entries) {
        if (result.size() > 0xFFFFFFFFu || entry.bytes.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("HWPX archive requires ZIP64");
        }
        const std::vector<std::uint8_t> encoded = entry.compress
            ? Deflate(entry.bytes) : entry.bytes;
        if (encoded.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("HWPX archive requires ZIP64");
        }

        CentralRecord record;
        record.name = entry.name;
        record.crc = fz::Crc32::compute(entry.bytes.data(), entry.bytes.size());
        record.compressedSize = static_cast<std::uint32_t>(encoded.size());
        record.expandedSize = static_cast<std::uint32_t>(entry.bytes.size());
        record.offset = static_cast<std::uint32_t>(result.size());
        record.method = entry.compress ? kDeflateMethod : kStoreMethod;
        central.push_back(record);

        Append32(&result, kLocalHeaderSignature);
        Append16(&result, 20);
        Append16(&result, kUtf8Flag);
        Append16(&result, record.method);
        Append16(&result, dosTime);
        Append16(&result, dosDate);
        Append32(&result, record.crc);
        Append32(&result, record.compressedSize);
        Append32(&result, record.expandedSize);
        Append16(&result, static_cast<std::uint16_t>(entry.name.size()));
        Append16(&result, 0);
        result.append(entry.name);
        result.append(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    }

    if (result.size() > 0xFFFFFFFFu) {
        throw std::runtime_error("HWPX archive requires ZIP64");
    }
    const auto centralOffset = static_cast<std::uint32_t>(result.size());
    for (const auto& record : central) {
        Append32(&result, kCentralHeaderSignature);
        Append16(&result, 20);
        Append16(&result, 20);
        Append16(&result, kUtf8Flag);
        Append16(&result, record.method);
        Append16(&result, dosTime);
        Append16(&result, dosDate);
        Append32(&result, record.crc);
        Append32(&result, record.compressedSize);
        Append32(&result, record.expandedSize);
        Append16(&result, static_cast<std::uint16_t>(record.name.size()));
        Append16(&result, 0);
        Append16(&result, 0);
        Append16(&result, 0);
        Append16(&result, 0);
        Append32(&result, 0);
        Append32(&result, record.offset);
        result.append(record.name);
    }
    const auto centralSize = static_cast<std::uint32_t>(
        result.size() - centralOffset);
    Append32(&result, kEndSignature);
    Append16(&result, 0);
    Append16(&result, 0);
    Append16(&result, static_cast<std::uint16_t>(central.size()));
    Append16(&result, static_cast<std::uint16_t>(central.size()));
    Append32(&result, centralSize);
    Append32(&result, centralOffset);
    Append16(&result, 0);
    if (result.size() > kMaximumArchiveBytes) {
        throw std::runtime_error("HWPX archive exceeds the 512 MB safety limit");
    }
    return result;
}

std::vector<std::uint8_t> DecodeBase64(std::string_view encoded) {
    static constexpr std::array<std::int8_t, 256> table = [] {
        std::array<std::int8_t, 256> values{};
        values.fill(-1);
        for (int index = 0; index < 26; ++index) {
            values[static_cast<unsigned char>('A' + index)] =
                static_cast<std::int8_t>(index);
            values[static_cast<unsigned char>('a' + index)] =
                static_cast<std::int8_t>(index + 26);
        }
        for (int index = 0; index < 10; ++index) {
            values[static_cast<unsigned char>('0' + index)] =
                static_cast<std::int8_t>(index + 52);
        }
        values[static_cast<unsigned char>('+')] = 62;
        values[static_cast<unsigned char>('/')] = 63;
        return values;
    }();

    std::vector<std::uint8_t> output;
    output.reserve(encoded.size() * 3 / 4);
    unsigned int accumulator = 0;
    int bits = 0;
    for (const unsigned char character : encoded) {
        if (character == '=') break;
        const int value = table[character];
        if (value < 0) throw std::runtime_error("Invalid embedded HWPX template");
        accumulator = (accumulator << 6) | static_cast<unsigned int>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<std::uint8_t>(accumulator >> bits));
            accumulator &= (1u << bits) - 1u;
        }
    }
    return output;
}

std::string InflateBaseHeader() {
    const auto compressed = DecodeBase64(kBaseHeaderDeflateBase64);
    CodecBuffer expanded;
    const MzipCodecStatus status = mzip_inflate_alloc(
        compressed.data(), compressed.size(), 32731, &expanded.value);
    if (status != MZIP_CODEC_OK) {
        throw std::runtime_error("Embedded HWPX header template is corrupt");
    }
    return std::string(reinterpret_cast<const char*>(expanded.value.data),
                       expanded.value.size);
}

void ReplaceOnce(std::string* value, std::string_view before,
                 std::string_view after) {
    const std::size_t position = value->find(before);
    if (position == std::string::npos) {
        throw std::runtime_error("Embedded HWPX header template is incompatible");
    }
    value->replace(position, before.size(), after);
}

void InsertBefore(std::string* value, std::string_view marker,
                  std::string_view insertion) {
    const std::size_t position = value->find(marker);
    if (position == std::string::npos) {
        throw std::runtime_error("Embedded HWPX header template is incompatible");
    }
    value->insert(position, insertion);
}

std::string XmlEscape(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 16);
    for (const char character : value) {
        switch (character) {
        case '&': output += "&amp;"; break;
        case '<': output += "&lt;"; break;
        case '>': output += "&gt;"; break;
        case '"': output += "&quot;"; break;
        case '\'': output += "&apos;"; break;
        default: output.push_back(character); break;
        }
    }
    return output;
}

std::string CharProperty(int id, int height, int font, bool bold = false,
                         bool italic = false, bool strike = false,
                         bool underline = false,
                         std::string_view color = "#000000",
                         std::string_view shade = "none") {
    const std::string fontText = std::to_string(font);
    std::string xml = "<hh:charPr id=\"" + std::to_string(id) +
        "\" height=\"" + std::to_string(height) +
        "\" textColor=\"" + std::string(color) +
        "\" shadeColor=\"" + std::string(shade) +
        "\" useFontSpace=\"0\" useKerning=\"0\" symMark=\"NONE\" borderFillIDRef=\"2\">";
    xml += "<hh:fontRef hangul=\"" + fontText + "\" latin=\"" + fontText +
        "\" hanja=\"" + fontText + "\" japanese=\"" + fontText +
        "\" other=\"" + fontText + "\" symbol=\"" + fontText +
        "\" user=\"" + fontText + "\"/>";
    xml += "<hh:ratio hangul=\"100\" latin=\"100\" hanja=\"100\" japanese=\"100\" other=\"100\" symbol=\"100\" user=\"100\"/>";
    xml += "<hh:spacing hangul=\"0\" latin=\"0\" hanja=\"0\" japanese=\"0\" other=\"0\" symbol=\"0\" user=\"0\"/>";
    xml += "<hh:relSz hangul=\"100\" latin=\"100\" hanja=\"100\" japanese=\"100\" other=\"100\" symbol=\"100\" user=\"100\"/>";
    xml += "<hh:offset hangul=\"0\" latin=\"0\" hanja=\"0\" japanese=\"0\" other=\"0\" symbol=\"0\" user=\"0\"/>";
    if (italic) xml += "<hh:italic/>";
    if (bold) xml += "<hh:bold/>";
    if (underline) {
        xml += "<hh:underline type=\"BOTTOM\" shape=\"SOLID\" color=\"#0563C1\"/>";
    }
    if (strike) xml += "<hh:strikeout shape=\"SOLID\" color=\"#000000\"/>";
    xml += "</hh:charPr>";
    return xml;
}

std::string ParagraphProperty(int id, int left, int intent, int previous,
                              int next, int borderFill, int lineSpacing,
                              bool keepWithNext = false) {
    std::string xml = "<hh:paraPr id=\"" + std::to_string(id) +
        "\" tabPrIDRef=\"0\" condense=\"0\" fontLineHeight=\"0\" snapToGrid=\"1\" suppressLineNumbers=\"0\" checked=\"0\">";
    xml += "<hh:align horizontal=\"LEFT\" vertical=\"BASELINE\"/>";
    xml += "<hh:heading type=\"NONE\" idRef=\"0\" level=\"0\"/>";
    xml += "<hh:breakSetting breakLatinWord=\"KEEP_WORD\" breakNonLatinWord=\"BREAK_WORD\" widowOrphan=\"1\" keepWithNext=\"" +
        std::string(keepWithNext ? "1" : "0") +
        "\" keepLines=\"0\" pageBreakBefore=\"0\" lineWrap=\"BREAK\"/>";
    xml += "<hh:autoSpacing eAsianEng=\"0\" eAsianNum=\"0\"/>";
    xml += "<hh:margin><hc:intent value=\"" + std::to_string(intent) +
        "\" unit=\"HWPUNIT\"/><hc:left value=\"" + std::to_string(left) +
        "\" unit=\"HWPUNIT\"/><hc:right value=\"0\" unit=\"HWPUNIT\"/>";
    xml += "<hc:prev value=\"" + std::to_string(previous) +
        "\" unit=\"HWPUNIT\"/><hc:next value=\"" + std::to_string(next) +
        "\" unit=\"HWPUNIT\"/></hh:margin>";
    xml += "<hh:lineSpacing type=\"PERCENT\" value=\"" +
        std::to_string(lineSpacing) + "\" unit=\"HWPUNIT\"/>";
    xml += "<hh:border borderFillIDRef=\"" + std::to_string(borderFill) +
        "\" offsetLeft=\"180\" offsetRight=\"180\" offsetTop=\"120\" offsetBottom=\"120\" connect=\"0\" ignoreMargin=\"0\"/>";
    xml += "</hh:paraPr>";
    return xml;
}

std::string BuildHeader(bool sansSerif) {
    std::string header = InflateBaseHeader();
    ReplaceOnce(&header, "<hh:borderFills itemCnt=\"2\">",
                "<hh:borderFills itemCnt=\"6\">");
    const std::string borderFills =
        "<hh:borderFill id=\"3\" threeD=\"0\" shadow=\"0\" centerLine=\"NONE\" breakCellSeparateLine=\"0\"><hh:slash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/><hh:backSlash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/><hh:leftBorder type=\"SOLID\" width=\"0.12 mm\" color=\"#B8BEC7\"/><hh:rightBorder type=\"SOLID\" width=\"0.12 mm\" color=\"#B8BEC7\"/><hh:topBorder type=\"SOLID\" width=\"0.12 mm\" color=\"#B8BEC7\"/><hh:bottomBorder type=\"SOLID\" width=\"0.12 mm\" color=\"#B8BEC7\"/><hh:diagonal type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/></hh:borderFill>"
        "<hh:borderFill id=\"4\" threeD=\"0\" shadow=\"0\" centerLine=\"NONE\" breakCellSeparateLine=\"0\"><hh:slash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/><hh:backSlash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/><hh:leftBorder type=\"SOLID\" width=\"0.5 mm\" color=\"#8A929E\"/><hh:rightBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/><hh:topBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/><hh:bottomBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/><hh:diagonal type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/></hh:borderFill>"
        "<hh:borderFill id=\"5\" threeD=\"0\" shadow=\"0\" centerLine=\"NONE\" breakCellSeparateLine=\"0\"><hh:slash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/><hh:backSlash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/><hh:leftBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/><hh:rightBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/><hh:topBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/><hh:bottomBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/><hh:diagonal type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/><hc:fillBrush><hc:winBrush faceColor=\"#F1F3F5\" hatchColor=\"#F1F3F5\" alpha=\"0\"/></hc:fillBrush></hh:borderFill>"
        "<hh:borderFill id=\"6\" threeD=\"0\" shadow=\"0\" centerLine=\"NONE\" breakCellSeparateLine=\"0\"><hh:slash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/><hh:backSlash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/><hh:leftBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/><hh:rightBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/><hh:topBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/><hh:bottomBorder type=\"SOLID\" width=\"0.25 mm\" color=\"#8A929E\"/><hh:diagonal type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/></hh:borderFill>";
    InsertBefore(&header, "</hh:borderFills>", borderFills);

    ReplaceOnce(&header, "<hh:charProperties itemCnt=\"7\">",
                "<hh:charProperties itemCnt=\"21\">");
    const int bodyFont = sansSerif ? 0 : 1;
    std::string chars;
    chars += CharProperty(7, 1000, bodyFont);
    chars += CharProperty(8, 1000, bodyFont, true);
    chars += CharProperty(9, 1000, bodyFont, false, true);
    chars += CharProperty(10, 1000, bodyFont, true, true);
    chars += CharProperty(11, 1000, bodyFont, false, false, true);
    chars += CharProperty(12, 900, 0, false, false, false, false,
                          "#20242A", "#F1F3F5");
    chars += CharProperty(13, 1000, bodyFont, false, false, false, true,
                          "#0563C1");
    chars += CharProperty(14, 2400, 0, true, false, false, false, "#1F2933");
    chars += CharProperty(15, 1900, 0, true, false, false, false, "#1F2933");
    chars += CharProperty(16, 1550, 0, true, false, false, false, "#27313C");
    chars += CharProperty(17, 1300, 0, true, false, false, false, "#27313C");
    chars += CharProperty(18, 1150, 0, true, false, false, false, "#34404C");
    chars += CharProperty(19, 1050, 0, true, false, false, false, "#34404C");
    chars += CharProperty(20, 1000, bodyFont, true, false, false, false,
                          "#20242A", "#E9EDF2");
    InsertBefore(&header, "</hh:charProperties>", chars);

    ReplaceOnce(&header, "<hh:paraProperties itemCnt=\"20\">",
                "<hh:paraProperties itemCnt=\"25\">");
    std::string paragraphs;
    paragraphs += ParagraphProperty(20, 1600, 0, 300, 300, 4, 150);
    paragraphs += ParagraphProperty(21, 500, 0, 300, 300, 5, 135);
    paragraphs += ParagraphProperty(22, 3000, -1600, 0, 150, 2, 150);
    paragraphs += ParagraphProperty(23, 0, 0, 0, 0, 2, 140);
    paragraphs += ParagraphProperty(24, 0, 0, 250, 450, 6, 100);
    InsertBefore(&header, "</hh:paraProperties>", paragraphs);
    return header;
}

std::string IsoDateUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

std::string BuildContent(const hwpx::Document& document) {
    const std::string title = XmlEscape(document.title);
    const std::string author = XmlEscape(document.author);
    const std::string date = IsoDateUtc();
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
        "<opf:package xmlns:ha=\"http://www.hancom.co.kr/hwpml/2011/app\" xmlns:hp=\"http://www.hancom.co.kr/hwpml/2011/paragraph\" xmlns:hp10=\"http://www.hancom.co.kr/hwpml/2016/paragraph\" xmlns:hs=\"http://www.hancom.co.kr/hwpml/2011/section\" xmlns:hc=\"http://www.hancom.co.kr/hwpml/2011/core\" xmlns:hh=\"http://www.hancom.co.kr/hwpml/2011/head\" xmlns:hpf=\"http://www.hancom.co.kr/schema/2011/hpf\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf/\" xmlns:config=\"urn:oasis:names:tc:opendocument:xmlns:config:1.0\" version=\"\" unique-identifier=\"\" id=\"\"><opf:metadata>";
    xml += "<opf:title>" + title + "</opf:title><opf:language>ko</opf:language>";
    xml += "<opf:meta name=\"creator\" content=\"text\">" + author + "</opf:meta>";
    xml += "<opf:meta name=\"CreatedDate\" content=\"text\">" + date + "</opf:meta>";
    xml += "<opf:meta name=\"ModifiedDate\" content=\"text\">" + date + "</opf:meta>";
    xml += "</opf:metadata><opf:manifest>";
    xml += "<opf:item id=\"header\" href=\"Contents/header.xml\" media-type=\"application/xml\"/>";
    for (const auto& image : document.images) {
        xml += "<opf:item id=\"" + XmlEscape(image.id) + "\" href=\"BinData/" +
            XmlEscape(image.id + image.extension) + "\" media-type=\"" +
            XmlEscape(image.mediaType) + "\" isEmbeded=\"1\"/>";
    }
    xml += "<opf:item id=\"section0\" href=\"Contents/section0.xml\" media-type=\"application/xml\"/>";
    xml += "<opf:item id=\"settings\" href=\"settings.xml\" media-type=\"application/xml\"/>";
    xml += "</opf:manifest><opf:spine><opf:itemref idref=\"header\" linear=\"yes\"/><opf:itemref idref=\"section0\" linear=\"yes\"/></opf:spine></opf:package>";
    return xml;
}

std::vector<std::uint8_t> ToBytes(std::string_view value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

bool IsValidImage(const hwpx::Image& image) {
    if (image.id.size() < 6 || image.id.rfind("image", 0) != 0 ||
        image.bytes.empty() || image.bytes.size() > 64ull * 1024 * 1024) {
        return false;
    }
    for (const char character : std::string_view(image.id).substr(5)) {
        if (!std::isdigit(static_cast<unsigned char>(character))) return false;
    }
    const bool extensionOkay = image.extension == ".png" ||
        image.extension == ".jpg" || image.extension == ".gif" ||
        image.extension == ".bmp" || image.extension == ".webp";
    return extensionOkay && image.mediaType.rfind("image/", 0) == 0;
}

void ValidateDocument(const hwpx::Document& document) {
    if (document.sectionXml.empty() ||
        document.sectionXml.size() > kMaximumSectionBytes ||
        document.previewText.size() > kMaximumPreviewBytes) {
        throw std::runtime_error("HWPX document content exceeds a safety limit");
    }
    if (document.sectionXml.find("<hs:sec ") == std::string::npos ||
        document.sectionXml.find("<hp:secPr ") == std::string::npos ||
        document.sectionXml.find("http://www.hancom.co.kr/hwpml/2011/section") ==
            std::string::npos ||
        document.sectionXml.find("<!DOCTYPE") != std::string::npos ||
        document.sectionXml.find("<!ENTITY") != std::string::npos ||
        document.sectionXml.find("<script") != std::string::npos) {
        throw std::runtime_error("HWPX section XML is invalid or unsafe");
    }
    if (document.images.size() > 128) {
        throw std::runtime_error("HWPX document contains too many images");
    }
    for (const auto& image : document.images) {
        if (!IsValidImage(image) ||
            document.sectionXml.find("binaryItemIDRef=\"" + image.id + "\"") ==
                std::string::npos) {
            throw std::runtime_error("HWPX image data is invalid or unreferenced");
        }
    }
}

}  // namespace

namespace hwpx {

bool BuildBytes(const Document& document, std::string* bytes,
                std::wstring* errorMessage) {
    if (!bytes) return false;
    try {
        ValidateDocument(document);
        std::vector<ZipEntry> entries;
        entries.push_back({"mimetype", ToBytes(kMimeType), false});
        entries.push_back({
            "version.xml",
            ToBytes("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
                  "<hv:HCFVersion xmlns:hv=\"http://www.hancom.co.kr/hwpml/2011/version\" tagetApplication=\"WORDPROCESSOR\" major=\"5\" minor=\"1\" micro=\"0\" buildNumber=\"1\" os=\"1\" xmlVersion=\"1.2\" application=\"MdViewer\" appVersion=\"0.2.1 Windows\"/>"),
            true});
        entries.push_back({"Contents/header.xml", ToBytes(BuildHeader(document.sansSerif)), true});
        for (const auto& image : document.images) {
            entries.push_back({"BinData/" + image.id + image.extension,
                               image.bytes, true});
        }
        entries.push_back({"Contents/section0.xml", ToBytes(document.sectionXml), true});
        entries.push_back({"Preview/PrvText.txt", ToBytes(document.previewText), true});
        entries.push_back({
            "settings.xml",
            ToBytes("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
                  "<ha:HWPApplicationSetting xmlns:ha=\"http://www.hancom.co.kr/hwpml/2011/app\" xmlns:config=\"urn:oasis:names:tc:opendocument:xmlns:config:1.0\"><ha:CaretPosition listIDRef=\"0\" paraIDRef=\"0\" pos=\"0\"/></ha:HWPApplicationSetting>"),
            true});
        entries.push_back({
            "META-INF/container.rdf",
            ToBytes("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
                  "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description rdf:about=\"\"><ns0:hasPart xmlns:ns0=\"http://www.hancom.co.kr/hwpml/2016/meta/pkg#\" rdf:resource=\"Contents/header.xml\"/></rdf:Description><rdf:Description rdf:about=\"Contents/header.xml\"><rdf:type rdf:resource=\"http://www.hancom.co.kr/hwpml/2016/meta/pkg#HeaderFile\"/></rdf:Description><rdf:Description rdf:about=\"\"><ns0:hasPart xmlns:ns0=\"http://www.hancom.co.kr/hwpml/2016/meta/pkg#\" rdf:resource=\"Contents/section0.xml\"/></rdf:Description><rdf:Description rdf:about=\"Contents/section0.xml\"><rdf:type rdf:resource=\"http://www.hancom.co.kr/hwpml/2016/meta/pkg#SectionFile\"/></rdf:Description><rdf:Description rdf:about=\"\"><rdf:type rdf:resource=\"http://www.hancom.co.kr/hwpml/2016/meta/pkg#Document\"/></rdf:Description></rdf:RDF>"),
            true});
        entries.push_back({"Contents/content.hpf", ToBytes(BuildContent(document)), true});
        entries.push_back({
            "META-INF/container.xml",
            ToBytes("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
                  "<ocf:container xmlns:ocf=\"urn:oasis:names:tc:opendocument:xmlns:container\" xmlns:hpf=\"http://www.hancom.co.kr/schema/2011/hpf\"><ocf:rootfiles><ocf:rootfile full-path=\"Contents/content.hpf\" media-type=\"application/hwpml-package+xml\"/><ocf:rootfile full-path=\"Preview/PrvText.txt\" media-type=\"text/plain\"/><ocf:rootfile full-path=\"META-INF/container.rdf\" media-type=\"application/rdf+xml\"/></ocf:rootfiles></ocf:container>"),
            true});
        entries.push_back({
            "META-INF/manifest.xml",
            ToBytes("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
                  "<odf:manifest xmlns:odf=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\"/>"),
            true});

        *bytes = BuildZip(entries);
        return true;
    } catch (const std::exception& error) {
        if (errorMessage) *errorMessage = WideError(error);
        return false;
    }
}

}  // namespace hwpx

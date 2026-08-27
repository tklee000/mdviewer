#include "DocxArchive.h"

#include "libmzip/crc32.h"
#include "libmzip/mzip_codec.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

constexpr std::uint32_t kLocalHeaderSignature = 0x04034B50u;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014B50u;
constexpr std::uint32_t kEndSignature = 0x06054B50u;
constexpr std::uint16_t kUtf8Flag = 1u << 11;
constexpr std::uint16_t kDeflateMethod = 8;
constexpr std::size_t kMaximumArchiveBytes = 512ull * 1024 * 1024;
constexpr std::size_t kMaximumEntryBytes = 128ull * 1024 * 1024;
constexpr std::size_t kMaximumDocumentBytes = 32ull * 1024 * 1024;

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
};

struct CentralRecord {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t expandedSize = 0;
    std::uint32_t offset = 0;
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
        throw std::runtime_error("DOCX archive has an invalid entry count");
    }
    std::size_t total = 0;
    for (const auto& entry : entries) {
        if (!IsSafeZipName(entry.name) || entry.name.size() > 0xFFFF ||
            entry.bytes.size() > kMaximumEntryBytes ||
            total > kMaximumArchiveBytes - entry.bytes.size()) {
            throw std::runtime_error("DOCX archive entry exceeds a safety limit");
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
            throw std::runtime_error("DOCX archive requires ZIP64");
        }
        const std::vector<std::uint8_t> encoded = Deflate(entry.bytes);
        if (encoded.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("DOCX archive requires ZIP64");
        }
        CentralRecord record;
        record.name = entry.name;
        record.crc = fz::Crc32::compute(entry.bytes.data(), entry.bytes.size());
        record.compressedSize = static_cast<std::uint32_t>(encoded.size());
        record.expandedSize = static_cast<std::uint32_t>(entry.bytes.size());
        record.offset = static_cast<std::uint32_t>(result.size());
        central.push_back(record);

        Append32(&result, kLocalHeaderSignature);
        Append16(&result, 20);
        Append16(&result, kUtf8Flag);
        Append16(&result, kDeflateMethod);
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
        throw std::runtime_error("DOCX archive requires ZIP64");
    }
    const auto centralOffset = static_cast<std::uint32_t>(result.size());
    for (const auto& record : central) {
        Append32(&result, kCentralHeaderSignature);
        Append16(&result, 20);
        Append16(&result, 20);
        Append16(&result, kUtf8Flag);
        Append16(&result, kDeflateMethod);
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
    const auto centralSize = static_cast<std::uint32_t>(result.size() - centralOffset);
    Append32(&result, kEndSignature);
    Append16(&result, 0);
    Append16(&result, 0);
    Append16(&result, static_cast<std::uint16_t>(central.size()));
    Append16(&result, static_cast<std::uint16_t>(central.size()));
    Append32(&result, centralSize);
    Append32(&result, centralOffset);
    Append16(&result, 0);
    if (result.size() > kMaximumArchiveBytes) {
        throw std::runtime_error("DOCX archive exceeds the 512 MB safety limit");
    }
    return result;
}

std::vector<std::uint8_t> ToBytes(std::string_view value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

std::string XmlEscape(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 16);
    for (const unsigned char character : value) {
        if (character < 0x20 && character != '\t' && character != '\n' &&
            character != '\r') continue;
        switch (character) {
        case '&': output += "&amp;"; break;
        case '<': output += "&lt;"; break;
        case '>': output += "&gt;"; break;
        case '"': output += "&quot;"; break;
        case '\'': output += "&apos;"; break;
        default: output.push_back(static_cast<char>(character)); break;
        }
    }
    return output;
}

std::string IsoDateUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

bool IsNumberedId(std::string_view value, std::string_view prefix) {
    if (value.size() <= prefix.size() || value.rfind(prefix, 0) != 0) return false;
    for (const char character : value.substr(prefix.size())) {
        if (!std::isdigit(static_cast<unsigned char>(character))) return false;
    }
    return true;
}

bool IsValidImage(const docx::Image& image) {
    if (!IsNumberedId(image.id, "image") || image.bytes.empty() ||
        image.bytes.size() > 64ull * 1024 * 1024) return false;
    if (image.extension == ".png") return image.mediaType == "image/png";
    if (image.extension == ".jpg" || image.extension == ".jpeg") {
        return image.mediaType == "image/jpeg";
    }
    if (image.extension == ".gif") return image.mediaType == "image/gif";
    if (image.extension == ".bmp") return image.mediaType == "image/bmp";
    if (image.extension == ".webp") return image.mediaType == "image/webp";
    return false;
}

bool IsSafeHyperlinkTarget(std::string_view target) {
    if (target.empty() || target.size() > 8192) return false;
    for (const unsigned char character : target) {
        if (character < 0x20 || character == 0x7F) return false;
    }
    std::string scheme(target.substr(0, (std::min)(target.size(), std::size_t{8})));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return scheme.rfind("https://", 0) == 0 || scheme.rfind("http://", 0) == 0 ||
        scheme.rfind("mailto:", 0) == 0;
}

void ValidateDocument(const docx::Document& document) {
    if (document.documentXml.empty() ||
        document.documentXml.size() > kMaximumDocumentBytes) {
        throw std::runtime_error("DOCX document content exceeds a safety limit");
    }
    if (document.documentXml.find("<w:document") == std::string::npos ||
        document.documentXml.find("<w:body") == std::string::npos ||
        document.documentXml.find("<w:sectPr") == std::string::npos ||
        document.documentXml.find("http://schemas.openxmlformats.org/wordprocessingml/2006/main") ==
            std::string::npos ||
        document.documentXml.find("<!DOCTYPE") != std::string::npos ||
        document.documentXml.find("<!ENTITY") != std::string::npos ||
        document.documentXml.find("<script") != std::string::npos) {
        throw std::runtime_error("DOCX document XML is invalid or unsafe");
    }
    if (document.images.size() > 128 || document.hyperlinks.size() > 512 ||
        document.lists.size() > 256) {
        throw std::runtime_error("DOCX document contains too many related items");
    }
    std::set<std::string> ids;
    for (const auto& image : document.images) {
        const std::string relationId = "rIdImage" + image.id.substr(5);
        if (!IsValidImage(image) || !ids.insert(relationId).second ||
            document.documentXml.find("r:embed=\"" + relationId + "\"") ==
                std::string::npos) {
            throw std::runtime_error("DOCX image data is invalid or unreferenced");
        }
    }
    for (const auto& hyperlink : document.hyperlinks) {
        const std::string relationId = "rIdLink" + hyperlink.id.substr(4);
        if (!IsNumberedId(hyperlink.id, "link") ||
            !IsSafeHyperlinkTarget(hyperlink.target) ||
            !ids.insert(relationId).second ||
            document.documentXml.find("r:id=\"" + relationId + "\"") ==
                std::string::npos) {
            throw std::runtime_error("DOCX hyperlink data is invalid or unreferenced");
        }
    }
    for (std::size_t index = 0; index < document.lists.size(); ++index) {
        const auto& list = document.lists[index];
        if (list.start == 0 || list.start > 1000000 ||
            document.documentXml.find("<w:numId w:val=\"" +
                std::to_string(index + 1) + "\"") == std::string::npos) {
            throw std::runtime_error("DOCX list data is invalid or unreferenced");
        }
    }
}

std::string BuildContentTypes(const docx::Document& document) {
    std::set<std::pair<std::string, std::string>> imageTypes;
    for (const auto& image : document.images) {
        std::string extension = image.extension.substr(1);
        imageTypes.emplace(extension, image.mediaType);
    }
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
    for (const auto& [extension, mediaType] : imageTypes) {
        xml += "<Default Extension=\"" + XmlEscape(extension) +
            "\" ContentType=\"" + XmlEscape(mediaType) + "\"/>";
    }
    xml +=
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
        "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
        "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>"
        "<Override PartName=\"/word/fontTable.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.fontTable+xml\"/>"
        "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
        "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
        "</Types>";
    return xml;
}

std::string BuildCoreProperties(const docx::Document& document) {
    const std::string date = IsoDateUtc();
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
        "<dc:title>" + XmlEscape(document.title) + "</dc:title><dc:creator>" +
        XmlEscape(document.author) + "</dc:creator><cp:lastModifiedBy>" +
        XmlEscape(document.author) + "</cp:lastModifiedBy><dcterms:created xsi:type=\"dcterms:W3CDTF\">" +
        date + "</dcterms:created><dcterms:modified xsi:type=\"dcterms:W3CDTF\">" +
        date + "</dcterms:modified></cp:coreProperties>";
}

std::string BuildDocumentRelationships(const docx::Document& document) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rIdStyles\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "<Relationship Id=\"rIdNumbering\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>"
        "<Relationship Id=\"rIdSettings\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings\" Target=\"settings.xml\"/>"
        "<Relationship Id=\"rIdFontTable\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/fontTable\" Target=\"fontTable.xml\"/>";
    for (const auto& image : document.images) {
        xml += "<Relationship Id=\"rIdImage" + image.id.substr(5) +
            "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/" +
            XmlEscape(image.id + image.extension) + "\"/>";
    }
    for (const auto& hyperlink : document.hyperlinks) {
        xml += "<Relationship Id=\"rIdLink" + hyperlink.id.substr(4) +
            "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" Target=\"" +
            XmlEscape(hyperlink.target) + "\" TargetMode=\"External\"/>";
    }
    xml += "</Relationships>";
    return xml;
}

std::string BuildStyles(bool sansSerif) {
    const std::string body = sansSerif ? "Malgun Gothic" : "Batang";
    const std::string bodyEscaped = XmlEscape(body);
    const std::string fonts = "<w:rFonts w:ascii=\"" + bodyEscaped +
        "\" w:hAnsi=\"" + bodyEscaped + "\" w:eastAsia=\"" +
        bodyEscaped + "\" w:cs=\"" + bodyEscaped + "\"/>";
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr>" + fonts +
        "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/></w:rPr></w:rPrDefault>"
        "<w:pPrDefault><w:pPr><w:spacing w:after=\"120\" w:line=\"276\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/><w:pPr><w:widowControl/></w:pPr><w:rPr>" +
        fonts + "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/></w:rPr></w:style>";
    constexpr int sizes[] = {48, 40, 32, 28, 24, 22};
    constexpr int before[] = {360, 320, 280, 240, 200, 160};
    for (int level = 1; level <= 6; ++level) {
        xml += "<w:style w:type=\"paragraph\" w:styleId=\"Heading" +
            std::to_string(level) + "\"><w:name w:val=\"heading " +
            std::to_string(level) + "\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:uiPriority w:val=\"" +
            std::to_string(9 + level) + "\"/><w:pPr><w:keepNext/><w:keepLines/><w:spacing w:before=\"" +
            std::to_string(before[level - 1]) + "\" w:after=\"120\"/><w:outlineLvl w:val=\"" +
            std::to_string(level - 1) + "\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Malgun Gothic\" w:hAnsi=\"Malgun Gothic\" w:eastAsia=\"Malgun Gothic\"/><w:b/><w:bCs/><w:color w:val=\"27313C\"/><w:sz w:val=\"" +
            std::to_string(sizes[level - 1]) + "\"/><w:szCs w:val=\"" +
            std::to_string(sizes[level - 1]) + "\"/></w:rPr></w:style>";
    }
    xml +=
        "<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/><w:qFormat/><w:pPr><w:ind w:left=\"720\" w:right=\"240\"/><w:spacing w:before=\"120\" w:after=\"120\"/><w:pBdr><w:left w:val=\"single\" w:sz=\"18\" w:space=\"12\" w:color=\"8A929E\"/></w:pBdr></w:pPr><w:rPr><w:i/><w:iCs/><w:color w:val=\"4F5965\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Code\"><w:name w:val=\"Code\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:ind w:left=\"240\" w:right=\"240\"/><w:spacing w:before=\"120\" w:after=\"120\" w:line=\"240\" w:lineRule=\"auto\"/><w:shd w:val=\"clear\" w:fill=\"F1F3F5\"/><w:pBdr><w:top w:val=\"single\" w:sz=\"4\" w:color=\"D8DDE3\"/><w:left w:val=\"single\" w:sz=\"4\" w:color=\"D8DDE3\"/><w:bottom w:val=\"single\" w:sz=\"4\" w:color=\"D8DDE3\"/><w:right w:val=\"single\" w:sz=\"4\" w:color=\"D8DDE3\"/></w:pBdr></w:pPr><w:rPr><w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:eastAsia=\"Malgun Gothic\"/><w:sz w:val=\"19\"/><w:szCs w:val=\"19\"/></w:rPr></w:style>"
        "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/><w:unhideWhenUsed/><w:rPr><w:color w:val=\"0563C1\"/><w:u w:val=\"single\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"TableText\"><w:name w:val=\"Table Text\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr><w:rPr><w:sz w:val=\"20\"/><w:szCs w:val=\"20\"/></w:rPr></w:style>"
        "</w:styles>";
    return xml;
}

std::string BuildAbstractNumbering(int id, bool ordered) {
    std::string xml = "<w:abstractNum w:abstractNumId=\"" + std::to_string(id) +
        "\"><w:multiLevelType w:val=\"multilevel\"/>";
    for (int level = 0; level < 9; ++level) {
        const int left = 720 + level * 360;
        const int hanging = 360;
        xml += "<w:lvl w:ilvl=\"" + std::to_string(level) +
            "\"><w:start w:val=\"1\"/><w:numFmt w:val=\"" +
            std::string(ordered ? "decimal" : "bullet") + "\"/><w:lvlText w:val=\"";
        if (ordered) {
            xml += "%" + std::to_string(level + 1) + ".";
        } else {
            xml += level % 3 == 0 ? "•" : (level % 3 == 1 ? "○" : "▪");
        }
        xml += "\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:tabs><w:tab w:val=\"num\" w:pos=\"" +
            std::to_string(left) + "\"/></w:tabs><w:ind w:left=\"" +
            std::to_string(left) + "\" w:hanging=\"" + std::to_string(hanging) +
            "\"/></w:pPr>";
        if (!ordered) {
            xml += "<w:rPr><w:rFonts w:ascii=\"Segoe UI Symbol\" w:hAnsi=\"Segoe UI Symbol\" w:eastAsia=\"Malgun Gothic\"/></w:rPr>";
        }
        xml += "</w:lvl>";
    }
    xml += "</w:abstractNum>";
    return xml;
}

std::string BuildNumbering(const docx::Document& document) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">" +
        BuildAbstractNumbering(0, false) + BuildAbstractNumbering(1, true);
    for (std::size_t index = 0; index < document.lists.size(); ++index) {
        const auto& list = document.lists[index];
        xml += "<w:num w:numId=\"" + std::to_string(index + 1) +
            "\"><w:abstractNumId w:val=\"" + std::string(list.ordered ? "1" : "0") + "\"/>";
        if (list.start != 1) {
            xml += "<w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"" +
                std::to_string(list.start) + "\"/></w:lvlOverride>";
        }
        xml += "</w:num>";
    }
    xml += "</w:numbering>";
    return xml;
}

}  // namespace

namespace docx {

bool BuildBytes(const Document& document, std::string* bytes,
                std::wstring* errorMessage) {
    if (!bytes) return false;
    try {
        ValidateDocument(document);
        std::vector<ZipEntry> entries;
        entries.push_back({"[Content_Types].xml", ToBytes(BuildContentTypes(document))});
        entries.push_back({"_rels/.rels", ToBytes(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
            "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
            "</Relationships>")});
        entries.push_back({"docProps/core.xml", ToBytes(BuildCoreProperties(document))});
        entries.push_back({"docProps/app.xml", ToBytes(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\">"
            "<Application>MdViewer</Application><AppVersion>0.3.0</AppVersion><Company></Company><DocSecurity>0</DocSecurity><ScaleCrop>false</ScaleCrop><LinksUpToDate>false</LinksUpToDate><SharedDoc>false</SharedDoc><HyperlinksChanged>false</HyperlinksChanged></Properties>")});
        entries.push_back({"word/document.xml", ToBytes(document.documentXml)});
        entries.push_back({"word/styles.xml", ToBytes(BuildStyles(document.sansSerif))});
        entries.push_back({"word/numbering.xml", ToBytes(BuildNumbering(document))});
        entries.push_back({"word/settings.xml", ToBytes(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<w:settings xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:zoom w:percent=\"100\"/><w:defaultTabStop w:val=\"720\"/><w:characterSpacingControl w:val=\"doNotCompress\"/><w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"15\"/></w:compat></w:settings>")});
        entries.push_back({"word/fontTable.xml", ToBytes(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<w:fonts xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:font w:name=\"Malgun Gothic\"><w:family w:val=\"swiss\"/><w:charset w:val=\"81\"/></w:font><w:font w:name=\"Batang\"><w:family w:val=\"roman\"/><w:charset w:val=\"81\"/></w:font><w:font w:name=\"Consolas\"><w:family w:val=\"modern\"/><w:pitch w:val=\"fixed\"/></w:font></w:fonts>")});
        entries.push_back({"word/_rels/document.xml.rels",
                           ToBytes(BuildDocumentRelationships(document))});
        for (const auto& image : document.images) {
            entries.push_back({"word/media/" + image.id + image.extension,
                               image.bytes});
        }
        *bytes = BuildZip(entries);
        return true;
    } catch (const std::exception& error) {
        if (errorMessage) *errorMessage = WideError(error);
        return false;
    }
}

}  // namespace docx

#include "image/EmbeddedImageScanner.h"

#include <QBuffer>
#include <QByteArray>
#include <QImageReader>
#include <QMetaObject>
#include <QSet>
#include <QSize>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cmath>
#include <future>
#include <limits>
#include <memory>
#include <vector>

namespace breco {

namespace {

constexpr quint64 kDefaultMaxDecodeBytes = 64ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaxPatternBytes = 16;

unsigned char byteAt(const QByteArray& bytes, int index) {
    return static_cast<unsigned char>(bytes.at(index));
}

bool hasBytes(const QByteArray& bytes, int start, int count) {
    return start >= 0 && count >= 0 && start + count <= bytes.size();
}

quint16 readLe16(const QByteArray& bytes, int start, bool* ok = nullptr) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (!hasBytes(bytes, start, 2)) {
        return 0;
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return static_cast<quint16>(byteAt(bytes, start) |
                                (static_cast<quint16>(byteAt(bytes, start + 1)) << 8U));
}

quint16 readBe16(const QByteArray& bytes, int start, bool* ok = nullptr) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (!hasBytes(bytes, start, 2)) {
        return 0;
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return static_cast<quint16>((static_cast<quint16>(byteAt(bytes, start)) << 8U) |
                                byteAt(bytes, start + 1));
}

quint32 readLe32(const QByteArray& bytes, int start, bool* ok = nullptr) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (!hasBytes(bytes, start, 4)) {
        return 0;
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return static_cast<quint32>(byteAt(bytes, start)) |
           (static_cast<quint32>(byteAt(bytes, start + 1)) << 8U) |
           (static_cast<quint32>(byteAt(bytes, start + 2)) << 16U) |
           (static_cast<quint32>(byteAt(bytes, start + 3)) << 24U);
}

quint32 readBe32(const QByteArray& bytes, int start, bool* ok = nullptr) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (!hasBytes(bytes, start, 4)) {
        return 0;
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return (static_cast<quint32>(byteAt(bytes, start)) << 24U) |
           (static_cast<quint32>(byteAt(bytes, start + 1)) << 16U) |
           (static_cast<quint32>(byteAt(bytes, start + 2)) << 8U) |
           static_cast<quint32>(byteAt(bytes, start + 3));
}

quint64 readLe64(const QByteArray& bytes, int start, bool* ok = nullptr) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (!hasBytes(bytes, start, 8)) {
        return 0;
    }
    quint64 value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<quint64>(byteAt(bytes, start + i)) << (8U * i);
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return value;
}

quint64 readBe64(const QByteArray& bytes, int start, bool* ok = nullptr) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (!hasBytes(bytes, start, 8)) {
        return 0;
    }
    quint64 value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | static_cast<quint64>(byteAt(bytes, start + i));
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return value;
}

qint32 readLeSigned32(const QByteArray& bytes, int start, bool* ok = nullptr) {
    return static_cast<qint32>(readLe32(bytes, start, ok));
}

bool pixelsWithinLimit(quint64 width, quint64 height, quint64 maxPixels) {
    if (width == 0 || height == 0 || maxPixels == 0) {
        return false;
    }
    return width <= maxPixels / height;
}

bool sizeWithinLimit(const QSize& size, quint64 maxPixels) {
    return size.isValid() &&
           pixelsWithinLimit(static_cast<quint64>(size.width()),
                             static_cast<quint64>(size.height()), maxPixels);
}

EmbeddedImageFormats formatFlag(EmbeddedImageFormat format) {
    return EmbeddedImageFormats(format);
}

bool enabled(const EmbeddedImageScanOptions& options, EmbeddedImageFormat format) {
    return options.formats.testFlag(format);
}

bool startsWithAt(const QByteArray& bytes, int offset, const QByteArray& pattern) {
    return hasBytes(bytes, offset, pattern.size()) &&
           std::memcmp(bytes.constData() + offset, pattern.constData(),
                       static_cast<size_t>(pattern.size())) == 0;
}

int indexOfInsensitive(const QByteArray& bytes, const QByteArray& pattern, int from) {
    if (pattern.isEmpty() || from < 0 || from >= bytes.size()) {
        return -1;
    }
    const int maxStart = bytes.size() - pattern.size();
    for (int i = from; i <= maxStart; ++i) {
        bool matched = true;
        for (int j = 0; j < pattern.size(); ++j) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(bytes.at(i + j))));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(pattern.at(j))));
            if (a != b) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return i;
        }
    }
    return -1;
}

bool parsePngSize(const QByteArray& bytes, QSize* size, quint64 maxPixels) {
    static const QByteArray sig = QByteArray::fromHex("89504e470d0a1a0a");
    if (!startsWithAt(bytes, 0, sig) || !startsWithAt(bytes, 12, "IHDR")) {
        return false;
    }
    bool okW = false;
    bool okH = false;
    const quint32 width = readBe32(bytes, 16, &okW);
    const quint32 height = readBe32(bytes, 20, &okH);
    if (!okW || !okH || !pixelsWithinLimit(width, height, maxPixels)) {
        return false;
    }
    if (size != nullptr) {
        *size = QSize(static_cast<int>(width), static_cast<int>(height));
    }
    return true;
}

bool parseGifSize(const QByteArray& bytes, QSize* size, quint64 maxPixels) {
    if (!(startsWithAt(bytes, 0, "GIF87a") || startsWithAt(bytes, 0, "GIF89a"))) {
        return false;
    }
    bool okW = false;
    bool okH = false;
    const quint16 width = readLe16(bytes, 6, &okW);
    const quint16 height = readLe16(bytes, 8, &okH);
    if (!okW || !okH || !pixelsWithinLimit(width, height, maxPixels)) {
        return false;
    }
    if (size != nullptr) {
        *size = QSize(width, height);
    }
    return true;
}

bool parseBmpSize(const QByteArray& bytes, QSize* size, quint64 maxPixels) {
    if (!startsWithAt(bytes, 0, "BM")) {
        return false;
    }
    bool okHeader = false;
    const quint32 dibHeaderSize = readLe32(bytes, 14, &okHeader);
    if (!okHeader) {
        return false;
    }
    quint64 width = 0;
    quint64 height = 0;
    quint16 bitDepth = 0;
    if (dibHeaderSize == 12 && hasBytes(bytes, 18, 8)) {
        width = readLe16(bytes, 18);
        height = readLe16(bytes, 20);
        bitDepth = readLe16(bytes, 24);
    } else if (dibHeaderSize >= 40 && hasBytes(bytes, 18, 12)) {
        bool okW = false;
        bool okH = false;
        const qint32 signedWidth = readLeSigned32(bytes, 18, &okW);
        const qint32 signedHeight = readLeSigned32(bytes, 22, &okH);
        bitDepth = readLe16(bytes, 28);
        if (!okW || !okH || signedWidth <= 0 || signedHeight == 0) {
            return false;
        }
        width = static_cast<quint64>(signedWidth);
        height = static_cast<quint64>(std::abs(signedHeight));
    } else {
        return false;
    }
    static const QSet<quint16> kDepths = {1, 4, 8, 16, 24, 32};
    if (!kDepths.contains(bitDepth) || !pixelsWithinLimit(width, height, maxPixels)) {
        return false;
    }
    if (size != nullptr) {
        *size = QSize(static_cast<int>(width), static_cast<int>(height));
    }
    return true;
}

bool parseIcoSize(const QByteArray& bytes, QSize* size, quint64 maxPixels) {
    if (!hasBytes(bytes, 0, 16) || readLe16(bytes, 0) != 0 || readLe16(bytes, 2) != 1) {
        return false;
    }
    const quint16 count = readLe16(bytes, 4);
    if (count == 0 || count > 256) {
        return false;
    }
    const quint64 dirSize = 6ULL + static_cast<quint64>(count) * 16ULL;
    if (dirSize > static_cast<quint64>(bytes.size())) {
        return false;
    }
    const quint64 width = byteAt(bytes, 6) == 0 ? 256 : byteAt(bytes, 6);
    const quint64 height = byteAt(bytes, 7) == 0 ? 256 : byteAt(bytes, 7);
    if (!pixelsWithinLimit(width, height, maxPixels)) {
        return false;
    }
    if (size != nullptr) {
        *size = QSize(static_cast<int>(width), static_cast<int>(height));
    }
    return true;
}

bool parseJpegSize(const QByteArray& bytes, QSize* size, quint64 maxPixels) {
    if (!hasBytes(bytes, 0, 4) || byteAt(bytes, 0) != 0xFF || byteAt(bytes, 1) != 0xD8 ||
        byteAt(bytes, 2) != 0xFF) {
        return false;
    }
    int pos = 2;
    while (pos + 4 < bytes.size()) {
        while (pos < bytes.size() && byteAt(bytes, pos) == 0xFF) {
            ++pos;
        }
        if (pos >= bytes.size()) {
            break;
        }
        const unsigned char marker = byteAt(bytes, pos++);
        if (marker == 0xD9 || marker == 0xDA) {
            break;
        }
        if (marker >= 0xD0 && marker <= 0xD7) {
            continue;
        }
        if (pos + 2 > bytes.size()) {
            break;
        }
        const quint16 segmentLength = readBe16(bytes, pos);
        if (segmentLength < 2 || pos + segmentLength > bytes.size()) {
            break;
        }
        const bool isSof = (marker >= 0xC0 && marker <= 0xC3) ||
                           (marker >= 0xC5 && marker <= 0xC7) ||
                           (marker >= 0xC9 && marker <= 0xCB) ||
                           (marker >= 0xCD && marker <= 0xCF);
        if (isSof && segmentLength >= 7) {
            const quint16 height = readBe16(bytes, pos + 3);
            const quint16 width = readBe16(bytes, pos + 5);
            if (!pixelsWithinLimit(width, height, maxPixels)) {
                return false;
            }
            if (size != nullptr) {
                *size = QSize(width, height);
            }
            return true;
        }
        pos += segmentLength;
    }
    return false;
}

bool readTiffUnsignedValue(const QByteArray& bytes, bool littleEndian, bool bigTiff, quint64 entryOffset,
                           quint16 type, quint64 count, quint64 valueOrOffset, quint64* value) {
    if (value == nullptr || count == 0) {
        return false;
    }
    const quint64 typeSize = (type == 3) ? 2ULL : ((type == 4) ? 4ULL : ((type == 16) ? 8ULL : 0ULL));
    if (typeSize == 0 || count != 1) {
        return false;
    }
    const quint64 inlineCapacity = bigTiff ? 8ULL : 4ULL;
    quint64 valueOffset = entryOffset + (bigTiff ? 12ULL : 8ULL);
    if (typeSize > inlineCapacity) {
        valueOffset = valueOrOffset;
    }
    if (valueOffset > static_cast<quint64>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int pos = static_cast<int>(valueOffset);
    bool ok = false;
    if (type == 3) {
        *value = littleEndian ? readLe16(bytes, pos, &ok) : readBe16(bytes, pos, &ok);
    } else if (type == 4) {
        *value = littleEndian ? readLe32(bytes, pos, &ok) : readBe32(bytes, pos, &ok);
    } else {
        *value = littleEndian ? readLe64(bytes, pos, &ok) : readBe64(bytes, pos, &ok);
    }
    return ok;
}

bool parseTiffSize(const QByteArray& bytes, QSize* size, quint64 maxPixels) {
    if (!hasBytes(bytes, 0, 8)) {
        return false;
    }
    const bool littleEndian = startsWithAt(bytes, 0, "II");
    const bool bigEndian = startsWithAt(bytes, 0, "MM");
    if (!littleEndian && !bigEndian) {
        return false;
    }
    const quint16 magic = littleEndian ? readLe16(bytes, 2) : readBe16(bytes, 2);
    const bool bigTiff = magic == 43;
    if (magic != 42 && magic != 43) {
        return false;
    }
    quint64 ifdOffset = 0;
    if (bigTiff) {
        if (!hasBytes(bytes, 0, 16)) {
            return false;
        }
        const quint16 bytesize = littleEndian ? readLe16(bytes, 4) : readBe16(bytes, 4);
        const quint16 zero = littleEndian ? readLe16(bytes, 6) : readBe16(bytes, 6);
        if (bytesize != 8 || zero != 0) {
            return false;
        }
        ifdOffset = littleEndian ? readLe64(bytes, 8) : readBe64(bytes, 8);
    } else {
        ifdOffset = littleEndian ? readLe32(bytes, 4) : readBe32(bytes, 4);
    }
    if (ifdOffset > static_cast<quint64>(bytes.size() - 2)) {
        return true;
    }
    const int countPos = static_cast<int>(ifdOffset);
    bool okCount = false;
    const quint64 entryCount = bigTiff ? (littleEndian ? readLe64(bytes, countPos, &okCount)
                                                       : readBe64(bytes, countPos, &okCount))
                                      : (littleEndian ? readLe16(bytes, countPos, &okCount)
                                                       : readBe16(bytes, countPos, &okCount));
    if (!okCount || entryCount > 4096) {
        return false;
    }
    quint64 width = 0;
    quint64 height = 0;
    const quint64 entriesStart = ifdOffset + (bigTiff ? 8ULL : 2ULL);
    const quint64 entrySize = bigTiff ? 20ULL : 12ULL;
    for (quint64 i = 0; i < entryCount; ++i) {
        const quint64 entryOffset = entriesStart + i * entrySize;
        if (entryOffset + entrySize > static_cast<quint64>(bytes.size()) ||
            entryOffset > static_cast<quint64>(std::numeric_limits<int>::max())) {
            break;
        }
        const int pos = static_cast<int>(entryOffset);
        const quint16 tag = littleEndian ? readLe16(bytes, pos) : readBe16(bytes, pos);
        const quint16 type = littleEndian ? readLe16(bytes, pos + 2) : readBe16(bytes, pos + 2);
        const quint64 count = bigTiff ? (littleEndian ? readLe64(bytes, pos + 4)
                                                      : readBe64(bytes, pos + 4))
                                      : (littleEndian ? readLe32(bytes, pos + 4)
                                                      : readBe32(bytes, pos + 4));
        const quint64 valueOrOffset = bigTiff ? (littleEndian ? readLe64(bytes, pos + 12)
                                                             : readBe64(bytes, pos + 12))
                                             : (littleEndian ? readLe32(bytes, pos + 8)
                                                             : readBe32(bytes, pos + 8));
        quint64 value = 0;
        if ((tag == 256 || tag == 257) &&
            readTiffUnsignedValue(bytes, littleEndian, bigTiff, entryOffset, type, count,
                                  valueOrOffset, &value)) {
            if (tag == 256) {
                width = value;
            } else {
                height = value;
            }
        }
        if (width > 0 && height > 0) {
            if (!pixelsWithinLimit(width, height, maxPixels)) {
                return false;
            }
            if (size != nullptr && width <= static_cast<quint64>(std::numeric_limits<int>::max()) &&
                height <= static_cast<quint64>(std::numeric_limits<int>::max())) {
                *size = QSize(static_cast<int>(width), static_cast<int>(height));
            }
            return true;
        }
    }
    return true;
}

bool parseTgaSize(const QByteArray& bytes, QSize* size, quint64 maxPixels) {
    if (!hasBytes(bytes, 0, 18)) {
        return false;
    }
    const unsigned char colorMapType = byteAt(bytes, 1);
    const unsigned char imageType = byteAt(bytes, 2);
    static const QSet<unsigned char> kTypes = {1, 2, 3, 9, 10, 11};
    static const QSet<unsigned char> kDepths = {8, 15, 16, 24, 32};
    if (colorMapType > 1 || !kTypes.contains(imageType) || !kDepths.contains(byteAt(bytes, 16))) {
        return false;
    }
    const quint16 width = readLe16(bytes, 12);
    const quint16 height = readLe16(bytes, 14);
    if (!pixelsWithinLimit(width, height, maxPixels)) {
        return false;
    }
    if (colorMapType == 0 && (imageType == 1 || imageType == 9)) {
        return false;
    }
    if (size != nullptr) {
        *size = QSize(width, height);
    }
    return true;
}

bool validateHeader(EmbeddedImageFormat format, const QByteArray& data, quint64 maxPixels,
                    QSize* parsedSize) {
    switch (format) {
        case EmbeddedImageFormat::Png:
            return parsePngSize(data, parsedSize, maxPixels);
        case EmbeddedImageFormat::Tiff:
            return parseTiffSize(data, parsedSize, maxPixels);
        case EmbeddedImageFormat::Jpeg:
            return parseJpegSize(data, parsedSize, maxPixels);
        case EmbeddedImageFormat::Bmp:
            return parseBmpSize(data, parsedSize, maxPixels);
        case EmbeddedImageFormat::Ico:
            return parseIcoSize(data, parsedSize, maxPixels);
        case EmbeddedImageFormat::Gif:
            return parseGifSize(data, parsedSize, maxPixels);
        case EmbeddedImageFormat::Tga:
            return parseTgaSize(data, parsedSize, maxPixels);
        case EmbeddedImageFormat::Xbm:
            return data.startsWith("#define");
        case EmbeddedImageFormat::Xpm:
            return data.startsWith("/* XPM */");
        case EmbeddedImageFormat::Svg:
            return data.startsWith("<svg") || data.startsWith("<SVG");
    }
    return false;
}

std::optional<quint64> pngPayloadSize(const QByteArray& bytes) {
    static const QByteArray signature = QByteArray::fromHex("89504e470d0a1a0a");
    if (!startsWithAt(bytes, 0, signature)) {
        return std::nullopt;
    }
    quint64 pos = 8;
    while (pos + 12 <= static_cast<quint64>(bytes.size())) {
        const quint32 dataSize = readBe32(bytes, static_cast<int>(pos));
        const quint64 chunkEnd = pos + 12ULL + static_cast<quint64>(dataSize);
        if (chunkEnd > static_cast<quint64>(bytes.size())) {
            return std::nullopt;
        }
        if (startsWithAt(bytes, static_cast<int>(pos + 4), "IEND")) {
            return chunkEnd;
        }
        pos = chunkEnd;
    }
    return std::nullopt;
}

std::optional<quint64> gifSubBlocksEnd(const QByteArray& bytes, quint64 pos) {
    while (pos < static_cast<quint64>(bytes.size())) {
        const quint64 blockSize = byteAt(bytes, static_cast<int>(pos++));
        if (blockSize == 0) {
            return pos;
        }
        if (pos + blockSize > static_cast<quint64>(bytes.size())) {
            return std::nullopt;
        }
        pos += blockSize;
    }
    return std::nullopt;
}

std::optional<quint64> gifPayloadSize(const QByteArray& bytes) {
    if (!hasBytes(bytes, 0, 13) ||
        !(startsWithAt(bytes, 0, "GIF87a") || startsWithAt(bytes, 0, "GIF89a"))) {
        return std::nullopt;
    }
    quint64 pos = 13;
    const unsigned char logicalPacked = byteAt(bytes, 10);
    if ((logicalPacked & 0x80U) != 0) {
        pos += 3ULL * (1ULL << ((logicalPacked & 0x07U) + 1U));
    }
    while (pos < static_cast<quint64>(bytes.size())) {
        const unsigned char marker = byteAt(bytes, static_cast<int>(pos++));
        if (marker == 0x3B) {
            return pos;
        }
        if (marker == 0x21) {
            if (pos >= static_cast<quint64>(bytes.size())) {
                return std::nullopt;
            }
            ++pos;
            const std::optional<quint64> end = gifSubBlocksEnd(bytes, pos);
            if (!end.has_value()) {
                return std::nullopt;
            }
            pos = *end;
            continue;
        }
        if (marker != 0x2C || pos + 9 > static_cast<quint64>(bytes.size())) {
            return std::nullopt;
        }
        const unsigned char imagePacked = byteAt(bytes, static_cast<int>(pos + 8));
        pos += 9;
        if ((imagePacked & 0x80U) != 0) {
            pos += 3ULL * (1ULL << ((imagePacked & 0x07U) + 1U));
        }
        if (pos >= static_cast<quint64>(bytes.size())) {
            return std::nullopt;
        }
        ++pos;
        const std::optional<quint64> end = gifSubBlocksEnd(bytes, pos);
        if (!end.has_value()) {
            return std::nullopt;
        }
        pos = *end;
    }
    return std::nullopt;
}

QVector<int> gifFrameDelays(const QByteArray& bytes) {
    QVector<int> delays;
    if (!hasBytes(bytes, 0, 13) ||
        !(startsWithAt(bytes, 0, "GIF87a") || startsWithAt(bytes, 0, "GIF89a"))) {
        return delays;
    }
    quint64 pos = 13;
    const unsigned char logicalPacked = byteAt(bytes, 10);
    if ((logicalPacked & 0x80U) != 0) {
        pos += 3ULL * (1ULL << ((logicalPacked & 0x07U) + 1U));
    }
    int pendingDelayMs = 100;
    while (pos < static_cast<quint64>(bytes.size())) {
        const unsigned char marker = byteAt(bytes, static_cast<int>(pos++));
        if (marker == 0x3B) {
            break;
        }
        if (marker == 0x21) {
            if (pos >= static_cast<quint64>(bytes.size())) {
                break;
            }
            const unsigned char label = byteAt(bytes, static_cast<int>(pos++));
            if (label == 0xF9 && hasBytes(bytes, static_cast<int>(pos), 6) &&
                byteAt(bytes, static_cast<int>(pos)) == 4) {
                pendingDelayMs =
                    qMax(16, static_cast<int>(readLe16(bytes, static_cast<int>(pos + 2))) * 10);
            }
            const std::optional<quint64> end = gifSubBlocksEnd(bytes, pos);
            if (!end.has_value()) {
                break;
            }
            pos = *end;
            continue;
        }
        if (marker != 0x2C || pos + 9 > static_cast<quint64>(bytes.size())) {
            break;
        }
        delays.push_back(pendingDelayMs);
        pendingDelayMs = 100;
        const unsigned char imagePacked = byteAt(bytes, static_cast<int>(pos + 8));
        pos += 9;
        if ((imagePacked & 0x80U) != 0) {
            pos += 3ULL * (1ULL << ((imagePacked & 0x07U) + 1U));
        }
        if (pos >= static_cast<quint64>(bytes.size())) {
            break;
        }
        ++pos;
        const std::optional<quint64> end = gifSubBlocksEnd(bytes, pos);
        if (!end.has_value()) {
            break;
        }
        pos = *end;
    }
    return delays;
}

std::optional<quint64> tgaPayloadSize(const QByteArray& bytes) {
    if (!hasBytes(bytes, 0, 18)) {
        return std::nullopt;
    }
    const unsigned char imageType = byteAt(bytes, 2);
    const quint64 colorMapBytes =
        (static_cast<quint64>(readLe16(bytes, 5)) * byteAt(bytes, 7) + 7ULL) / 8ULL;
    quint64 pos = 18ULL + byteAt(bytes, 0) + colorMapBytes;
    const quint64 pixelCount =
        static_cast<quint64>(readLe16(bytes, 12)) * readLe16(bytes, 14);
    const quint64 pixelBytes = (byteAt(bytes, 16) + 7ULL) / 8ULL;
    if (pixelBytes == 0 || pos > static_cast<quint64>(bytes.size())) {
        return std::nullopt;
    }
    if (imageType == 1 || imageType == 2 || imageType == 3) {
        const quint64 dataBytes = pixelCount * pixelBytes;
        if (dataBytes > static_cast<quint64>(bytes.size()) - pos) {
            return std::nullopt;
        }
        pos += dataBytes;
    } else if (imageType == 9 || imageType == 10 || imageType == 11) {
        quint64 decodedPixels = 0;
        while (decodedPixels < pixelCount && pos < static_cast<quint64>(bytes.size())) {
            const unsigned char packet = byteAt(bytes, static_cast<int>(pos++));
            const quint64 packetPixels = (packet & 0x7FU) + 1ULL;
            const quint64 packetBytes =
                ((packet & 0x80U) != 0 ? 1ULL : packetPixels) * pixelBytes;
            if (packetPixels > pixelCount - decodedPixels ||
                packetBytes > static_cast<quint64>(bytes.size()) - pos) {
                return std::nullopt;
            }
            pos += packetBytes;
            decodedPixels += packetPixels;
        }
        if (decodedPixels != pixelCount) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    static const QByteArray footerSignature("TRUEVISION-XFILE.", 17);
    const int signaturePos =
        bytes.indexOf(footerSignature, static_cast<qsizetype>(qMin<quint64>(
                                           pos + 8ULL, static_cast<quint64>(bytes.size()))));
    if (signaturePos >= 8 && hasBytes(bytes, signaturePos - 8, 26) &&
        byteAt(bytes, signaturePos + 17) == 0) {
        return static_cast<quint64>(signaturePos - 8 + 26);
    }
    return pos;
}

QByteArray encodedPayload(EmbeddedImageFormat format, const QByteArray& bytes) {
    std::optional<quint64> size;
    switch (format) {
        case EmbeddedImageFormat::Png:
            size = pngPayloadSize(bytes);
            break;
        case EmbeddedImageFormat::Gif:
            size = gifPayloadSize(bytes);
            break;
        case EmbeddedImageFormat::Bmp: {
            const quint64 declaredSize = readLe32(bytes, 2);
            if (declaredSize > 0 && declaredSize <= static_cast<quint64>(bytes.size())) {
                size = declaredSize;
            }
            break;
        }
        case EmbeddedImageFormat::Ico: {
            const quint16 count = readLe16(bytes, 4);
            quint64 end = 6ULL + static_cast<quint64>(count) * 16ULL;
            bool valid = count > 0 && end <= static_cast<quint64>(bytes.size());
            for (quint16 i = 0; valid && i < count; ++i) {
                const int entry = 6 + static_cast<int>(i) * 16;
                const quint64 imageSize = readLe32(bytes, entry + 8);
                const quint64 imageOffset = readLe32(bytes, entry + 12);
                valid = imageOffset <= static_cast<quint64>(bytes.size()) &&
                        imageSize <= static_cast<quint64>(bytes.size()) - imageOffset;
                if (valid) {
                    end = qMax(end, imageOffset + imageSize);
                }
            }
            if (valid) {
                size = end;
            }
            break;
        }
        case EmbeddedImageFormat::Jpeg: {
            for (int i = 2; i + 1 < bytes.size(); ++i) {
                if (byteAt(bytes, i) == 0xFF && byteAt(bytes, i + 1) == 0xD9) {
                    size = static_cast<quint64>(i + 2);
                    break;
                }
            }
            break;
        }
        case EmbeddedImageFormat::Tga:
            size = tgaPayloadSize(bytes);
            break;
        case EmbeddedImageFormat::Xbm:
        case EmbeddedImageFormat::Xpm: {
            const int end = bytes.indexOf("};");
            if (end >= 0) {
                size = static_cast<quint64>(end + 2);
            }
            break;
        }
        case EmbeddedImageFormat::Svg: {
            const int end = indexOfInsensitive(bytes, "</svg>", 0);
            if (end >= 0) {
                size = static_cast<quint64>(end + 6);
            }
            break;
        }
        case EmbeddedImageFormat::Tiff:
            break;
    }
    if (!size.has_value() || *size == 0 || *size > static_cast<quint64>(bytes.size())) {
        return bytes;
    }
    return bytes.left(static_cast<qsizetype>(*size));
}

std::optional<EmbeddedImageResult> decodeCandidate(const EmbeddedImageScanSource& source,
                                                   const EmbeddedImageScanOptions& options,
                                                   quint64 offset,
                                                   EmbeddedImageFormat format) {
    if (offset >= options.endOffsetExclusive || !embeddedImageFormatHasReader(format)) {
        return std::nullopt;
    }
    const quint64 remaining = options.endOffsetExclusive - offset;
    const quint64 decodeLimit = qMin(qMax<quint64>(options.maxDecodeBytes, 4096ULL), kDefaultMaxDecodeBytes);
    const quint64 readSize = qMin(remaining, decodeLimit);
    if (readSize == 0 || !source.read) {
        return std::nullopt;
    }
    const std::optional<QByteArray> bytes = source.read(offset, readSize);
    if (!bytes.has_value() || bytes->isEmpty()) {
        return std::nullopt;
    }

    const quint64 maxPixels = qMax<quint64>(1, static_cast<quint64>(options.maxPixelsK) * 1000ULL);
    QSize parsedSize;
    if (!validateHeader(format, *bytes, maxPixels, &parsedSize)) {
        return std::nullopt;
    }
    const QByteArray payload = encodedPayload(format, *bytes);

    QBuffer sizeBuffer;
    sizeBuffer.setData(payload);
    if (!sizeBuffer.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QImageReader sizeReader(&sizeBuffer, embeddedImageQtFormatName(format));
    sizeReader.setAutoTransform(false);
    const QSize readerSize = sizeReader.size();
    if (readerSize.isValid() && !sizeWithinLimit(readerSize, maxPixels)) {
        return std::nullopt;
    }

    EmbeddedImageResult result;
    result.offset = offset;
    result.format = format;
    result.formatName = embeddedImageFormatName(format);
    result.encodedData = payload;

    QBuffer imageBuffer;
    imageBuffer.setData(payload);
    if (!imageBuffer.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QImageReader imageReader(&imageBuffer, embeddedImageQtFormatName(format));
    imageReader.setAutoTransform(false);
    if (format == EmbeddedImageFormat::Gif) {
        const QVector<int> encodedFrameDelays = gifFrameDelays(payload);
        const int reportedFrameCount = imageReader.imageCount();
        int frameIndex = 0;
        while ((reportedFrameCount > 0 && frameIndex < reportedFrameCount) ||
               (reportedFrameCount <= 0 && imageReader.canRead())) {
            const int frameDelayMs =
                frameIndex < encodedFrameDelays.size()
                    ? encodedFrameDelays.at(frameIndex)
                    : qMax(16, imageReader.nextImageDelay());
            QImage frame = imageReader.read();
            if (frame.isNull() || !sizeWithinLimit(frame.size(), maxPixels)) {
                return std::nullopt;
            }
            result.animationFrames.push_back(std::move(frame));
            result.frameDelaysMs.push_back(frameDelayMs);
            ++frameIndex;
        }
        if (result.animationFrames.isEmpty()) {
            return std::nullopt;
        }
        result.image = result.animationFrames.first();
    } else {
        result.image = imageReader.read();
        if (result.image.isNull() || !sizeWithinLimit(result.image.size(), maxPixels)) {
            return std::nullopt;
        }
    }
    result.size = result.image.size();
    return result;
}

struct MagicPattern {
    EmbeddedImageFormat format;
    QByteArray pattern;
    bool caseInsensitive = false;
};

QVector<MagicPattern> patternsForOptions(const EmbeddedImageScanOptions& options) {
    QVector<MagicPattern> patterns;
    if (enabled(options, EmbeddedImageFormat::Png)) {
        patterns.push_back({EmbeddedImageFormat::Png, QByteArray::fromHex("89504e470d0a1a0a")});
    }
    if (enabled(options, EmbeddedImageFormat::Tiff)) {
        patterns.push_back({EmbeddedImageFormat::Tiff, QByteArray("II*\0", 4)});
        patterns.push_back({EmbeddedImageFormat::Tiff, QByteArray("MM\0*", 4)});
        patterns.push_back({EmbeddedImageFormat::Tiff, QByteArray("II+\0", 4)});
        patterns.push_back({EmbeddedImageFormat::Tiff, QByteArray("MM\0+", 4)});
    }
    if (enabled(options, EmbeddedImageFormat::Jpeg)) {
        patterns.push_back({EmbeddedImageFormat::Jpeg, QByteArray::fromHex("ffd8ff")});
    }
    if (enabled(options, EmbeddedImageFormat::Bmp)) {
        patterns.push_back({EmbeddedImageFormat::Bmp, "BM"});
    }
    if (enabled(options, EmbeddedImageFormat::Ico)) {
        patterns.push_back({EmbeddedImageFormat::Ico, QByteArray::fromHex("00000100")});
    }
    if (enabled(options, EmbeddedImageFormat::Gif)) {
        patterns.push_back({EmbeddedImageFormat::Gif, "GIF87a"});
        patterns.push_back({EmbeddedImageFormat::Gif, "GIF89a"});
    }
    if (enabled(options, EmbeddedImageFormat::Xpm)) {
        patterns.push_back({EmbeddedImageFormat::Xpm, "/* XPM */"});
    }
    if (enabled(options, EmbeddedImageFormat::Svg)) {
        patterns.push_back({EmbeddedImageFormat::Svg, "<svg", true});
    }
    return patterns;
}

bool shouldStop(const EmbeddedImageCancelCallback& shouldCancel) {
    return shouldCancel && shouldCancel();
}

bool acceptedLimitReached(const QVector<EmbeddedImageResult>& results,
                          const EmbeddedImageScanOptions& options) {
    return options.maxResults > 0 && results.size() >= options.maxResults;
}

struct Candidate {
    quint64 offset = 0;
    EmbeddedImageFormat format = EmbeddedImageFormat::Png;
};

QString candidateKey(const Candidate& candidate) {
    return QStringLiteral("%1:%2")
        .arg(static_cast<quint32>(candidate.format))
        .arg(candidate.offset);
}

bool appendDecodedCandidate(QVector<EmbeddedImageResult>* results, QSet<QString>* seen,
                            const EmbeddedImageScanSource& source,
                            const EmbeddedImageScanOptions& options, const Candidate& candidate,
                            const EmbeddedImageResultCallback& resultCallback) {
    if (results == nullptr || seen == nullptr || acceptedLimitReached(*results, options)) {
        return false;
    }
    const QString key = candidateKey(candidate);
    if (seen->contains(key)) {
        return false;
    }
    seen->insert(key);
    const std::optional<EmbeddedImageResult> decoded =
        decodeCandidate(source, options, candidate.offset, candidate.format);
    if (!decoded.has_value()) {
        return false;
    }
    results->push_back(*decoded);
    if (resultCallback) {
        resultCallback(*decoded);
    }
    return true;
}

QVector<Candidate> scanCandidatesInRange(const QByteArray& chunk, quint64 chunkStart,
                                         quint64 rangeStart, quint64 rangeEnd,
                                         const QVector<MagicPattern>& patterns) {
    QVector<Candidate> candidates;
    if (rangeEnd <= rangeStart || rangeStart < chunkStart) {
        return candidates;
    }
    const int localRangeStart = static_cast<int>(rangeStart - chunkStart);
    for (const MagicPattern& pattern : patterns) {
        int local = pattern.caseInsensitive
                        ? indexOfInsensitive(chunk, pattern.pattern, localRangeStart)
                        : chunk.indexOf(pattern.pattern, localRangeStart);
        while (local >= 0) {
            const quint64 candidateOffset = chunkStart + static_cast<quint64>(local);
            if (candidateOffset >= rangeEnd) {
                break;
            }
            candidates.push_back({candidateOffset, pattern.format});
            const int nextFrom = local + 1;
            local = pattern.caseInsensitive ? indexOfInsensitive(chunk, pattern.pattern, nextFrom)
                                            : chunk.indexOf(pattern.pattern, nextFrom);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.offset != b.offset) {
            return a.offset < b.offset;
        }
        return static_cast<quint32>(a.format) < static_cast<quint32>(b.format);
    });
    return candidates;
}

void emitProgress(const EmbeddedImageProgressCallback& progressCallback,
                  quint64 bytesScanned, quint64 bytesTotal, quint64 rawBytesRead,
                  const QVector<EmbeddedImageResult>& results,
                  const EmbeddedImageScanOptions& options) {
    if (!progressCallback) {
        return;
    }
    progressCallback(bytesScanned, bytesTotal, rawBytesRead, results.size(), options.maxResults);
}

}  // namespace

EmbeddedImageFormats allEmbeddedImageFormats() {
    return formatFlag(EmbeddedImageFormat::Tga) | formatFlag(EmbeddedImageFormat::Tiff) |
           formatFlag(EmbeddedImageFormat::Png) | formatFlag(EmbeddedImageFormat::Jpeg) |
           formatFlag(EmbeddedImageFormat::Bmp) | formatFlag(EmbeddedImageFormat::Ico) |
           formatFlag(EmbeddedImageFormat::Gif) | formatFlag(EmbeddedImageFormat::Xbm) |
           formatFlag(EmbeddedImageFormat::Xpm) | formatFlag(EmbeddedImageFormat::Svg);
}

QString embeddedImageFormatName(EmbeddedImageFormat format) {
    switch (format) {
        case EmbeddedImageFormat::Tga:
            return QStringLiteral("TGA");
        case EmbeddedImageFormat::Tiff:
            return QStringLiteral("TIFF");
        case EmbeddedImageFormat::Png:
            return QStringLiteral("PNG");
        case EmbeddedImageFormat::Jpeg:
            return QStringLiteral("JPEG");
        case EmbeddedImageFormat::Bmp:
            return QStringLiteral("BMP");
        case EmbeddedImageFormat::Ico:
            return QStringLiteral("ICO");
        case EmbeddedImageFormat::Gif:
            return QStringLiteral("GIF");
        case EmbeddedImageFormat::Xbm:
            return QStringLiteral("XBM");
        case EmbeddedImageFormat::Xpm:
            return QStringLiteral("XPM");
        case EmbeddedImageFormat::Svg:
            return QStringLiteral("SVG");
    }
    return QStringLiteral("Image");
}

QString embeddedImageFileExtension(EmbeddedImageFormat format) {
    switch (format) {
        case EmbeddedImageFormat::Tga:
            return QStringLiteral("tga");
        case EmbeddedImageFormat::Tiff:
            return QStringLiteral("tiff");
        case EmbeddedImageFormat::Png:
            return QStringLiteral("png");
        case EmbeddedImageFormat::Jpeg:
            return QStringLiteral("jpg");
        case EmbeddedImageFormat::Bmp:
            return QStringLiteral("bmp");
        case EmbeddedImageFormat::Ico:
            return QStringLiteral("ico");
        case EmbeddedImageFormat::Gif:
            return QStringLiteral("gif");
        case EmbeddedImageFormat::Xbm:
            return QStringLiteral("xbm");
        case EmbeddedImageFormat::Xpm:
            return QStringLiteral("xpm");
        case EmbeddedImageFormat::Svg:
            return QStringLiteral("svg");
    }
    return QStringLiteral("img");
}

QByteArray embeddedImageQtFormatName(EmbeddedImageFormat format) {
    switch (format) {
        case EmbeddedImageFormat::Tga:
            return "tga";
        case EmbeddedImageFormat::Tiff:
            return "tiff";
        case EmbeddedImageFormat::Png:
            return "png";
        case EmbeddedImageFormat::Jpeg:
            return "jpeg";
        case EmbeddedImageFormat::Bmp:
            return "bmp";
        case EmbeddedImageFormat::Ico:
            return "ico";
        case EmbeddedImageFormat::Gif:
            return "gif";
        case EmbeddedImageFormat::Xbm:
            return "xbm";
        case EmbeddedImageFormat::Xpm:
            return "xpm";
        case EmbeddedImageFormat::Svg:
            return "svg";
    }
    return {};
}

bool embeddedImageFormatHasReader(EmbeddedImageFormat format) {
    const QList<QByteArray> supported = QImageReader::supportedImageFormats();
    const QByteArray qtFormat = embeddedImageQtFormatName(format);
    if (supported.contains(qtFormat)) {
        return true;
    }
    if (format == EmbeddedImageFormat::Jpeg) {
        return supported.contains("jpg");
    }
    if (format == EmbeddedImageFormat::Tiff) {
        return supported.contains("tif");
    }
    return false;
}

EmbeddedImageFormats supportedEmbeddedImageFormats() {
    EmbeddedImageFormats formats;
    for (const EmbeddedImageFormat format :
         {EmbeddedImageFormat::Tga, EmbeddedImageFormat::Tiff, EmbeddedImageFormat::Png,
          EmbeddedImageFormat::Jpeg, EmbeddedImageFormat::Bmp, EmbeddedImageFormat::Ico,
          EmbeddedImageFormat::Gif, EmbeddedImageFormat::Xbm, EmbeddedImageFormat::Xpm,
          EmbeddedImageFormat::Svg}) {
        if (embeddedImageFormatHasReader(format)) {
            formats |= formatFlag(format);
        }
    }
    return formats;
}

QVector<EmbeddedImageResult> scanEmbeddedImages(
    const EmbeddedImageScanSource& source, EmbeddedImageScanOptions options,
    EmbeddedImageScanSummary* summary, EmbeddedImageCancelCallback shouldCancel,
    EmbeddedImageProgressCallback progressCallback,
    EmbeddedImageResultCallback resultCallback) {
    EmbeddedImageScanSummary localSummary;
    QVector<EmbeddedImageResult> results;
    QSet<QString> seen;

    EmbeddedImageScanSource countedSource = source;
    countedSource.read = [&source, &localSummary](quint64 offset,
                                                  quint64 size) -> std::optional<QByteArray> {
        const std::optional<QByteArray> bytes = source.read(offset, size);
        if (bytes.has_value()) {
            localSummary.rawBytesRead += static_cast<quint64>(bytes->size());
        }
        return bytes;
    };

    if (!source.read || source.fileSize == 0) {
        if (summary != nullptr) {
            *summary = localSummary;
        }
        return results;
    }

    options.formats &= supportedEmbeddedImageFormats();
    if (!options.formats) {
        localSummary.message = QStringLiteral("No enabled image formats have Qt readers available.");
        if (summary != nullptr) {
            *summary = localSummary;
        }
        return results;
    }

    options.endOffsetExclusive =
        qMin(options.endOffsetExclusive == 0 ? source.fileSize : options.endOffsetExclusive,
             source.fileSize);
    options.startOffset = qMin(options.startOffset, options.endOffsetExclusive);
    options.chunkSize = qBound<quint64>(4096ULL, options.chunkSize, 16ULL * 1024ULL * 1024ULL);
    options.maxDecodeBytes = qBound<quint64>(4096ULL, options.maxDecodeBytes, kDefaultMaxDecodeBytes);
    options.workerCount = qBound(1, options.workerCount, 256);
    const quint64 bytesTotal =
        options.endOffsetExclusive >= options.startOffset
            ? options.endOffsetExclusive - options.startOffset
            : 0ULL;

    if (enabled(options, EmbeddedImageFormat::Tga)) {
        appendDecodedCandidate(&results, &seen, countedSource, options,
                               Candidate{options.startOffset, EmbeddedImageFormat::Tga},
                               resultCallback);
    }
    if (enabled(options, EmbeddedImageFormat::Xbm)) {
        appendDecodedCandidate(&results, &seen, countedSource, options,
                               Candidate{options.startOffset, EmbeddedImageFormat::Xbm},
                               resultCallback);
    }
    emitProgress(progressCallback, 0, bytesTotal, localSummary.rawBytesRead, results, options);
    if (acceptedLimitReached(results, options) || shouldStop(shouldCancel)) {
        localSummary.cancelled = shouldStop(shouldCancel);
        localSummary.message =
            localSummary.cancelled
                ? QStringLiteral("Image scan cancelled.")
                : QStringLiteral("Found %1 image%2.")
                      .arg(results.size())
                      .arg(results.size() == 1 ? QString() : QStringLiteral("s"));
        if (summary != nullptr) {
            *summary = localSummary;
        }
        return results;
    }

    const QVector<MagicPattern> patterns = patternsForOptions(options);
    quint64 chunkStart = options.startOffset;
    while (chunkStart < options.endOffsetExclusive && !acceptedLimitReached(results, options)) {
        if (shouldStop(shouldCancel)) {
            localSummary.cancelled = true;
            break;
        }
        const quint64 primarySize =
            qMin(options.chunkSize, options.endOffsetExclusive - chunkStart);
        const quint64 readSize =
            qMin(primarySize + kMaxPatternBytes - 1ULL, options.endOffsetExclusive - chunkStart);
        const std::optional<QByteArray> chunk = countedSource.read(chunkStart, readSize);
        if (!chunk.has_value() || chunk->isEmpty()) {
            break;
        }
        const quint64 actualPrimarySize =
            qMin<quint64>(primarySize, static_cast<quint64>(chunk->size()));
        const quint64 primaryEnd = chunkStart + primarySize;

        const int workerCount =
            static_cast<int>(qMin<quint64>(static_cast<quint64>(options.workerCount),
                                           qMax<quint64>(1, actualPrimarySize)));
        const auto sharedChunk = std::make_shared<const QByteArray>(*chunk);
        std::vector<std::future<QVector<Candidate>>> futures;
        futures.reserve(workerCount);
        for (int worker = 0; worker < workerCount; ++worker) {
            const quint64 rangeStart =
                chunkStart + (actualPrimarySize * static_cast<quint64>(worker)) /
                                 static_cast<quint64>(workerCount);
            const quint64 rangeEnd =
                chunkStart + (actualPrimarySize * static_cast<quint64>(worker + 1)) /
                                 static_cast<quint64>(workerCount);
            futures.push_back(std::async(std::launch::async,
                                         [sharedChunk, chunkStart, rangeStart, rangeEnd, patterns]() {
                                             return scanCandidatesInRange(*sharedChunk, chunkStart,
                                                                          rangeStart, rangeEnd,
                                                                          patterns);
                                         }));
        }

        QVector<Candidate> candidates;
        for (std::future<QVector<Candidate>>& future : futures) {
            QVector<Candidate> workerCandidates = future.get();
            candidates += workerCandidates;
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.offset != b.offset) {
                return a.offset < b.offset;
            }
            return static_cast<quint32>(a.format) < static_cast<quint32>(b.format);
        });

        for (const Candidate& candidate : candidates) {
            if (shouldStop(shouldCancel)) {
                localSummary.cancelled = true;
                break;
            }
            if (candidate.offset >= primaryEnd) {
                continue;
            }
            appendDecodedCandidate(&results, &seen, countedSource, options, candidate, resultCallback);
            if (acceptedLimitReached(results, options)) {
                break;
            }
        }
        localSummary.bytesScanned += actualPrimarySize;
        emitProgress(progressCallback, localSummary.bytesScanned, bytesTotal,
                     localSummary.rawBytesRead, results, options);
        if (localSummary.cancelled) {
            break;
        }
        chunkStart += actualPrimarySize;
        if (actualPrimarySize == 0) {
            break;
        }
    }

    if (localSummary.cancelled) {
        localSummary.message = QStringLiteral("Image scan cancelled.");
    } else {
        localSummary.message =
            QStringLiteral("Found %1 image%2.")
                .arg(results.size())
                .arg(results.size() == 1 ? QString() : QStringLiteral("s"));
    }
    if (summary != nullptr) {
        *summary = localSummary;
    }
    return results;
}

EmbeddedImageScanController::EmbeddedImageScanController(QObject* parent) : QObject(parent) {
    qRegisterMetaType<breco::EmbeddedImageResult>("breco::EmbeddedImageResult");
    qRegisterMetaType<breco::EmbeddedImageScanSummary>("breco::EmbeddedImageScanSummary");
    qRegisterMetaType<QVector<breco::EmbeddedImageResult>>("QVector<breco::EmbeddedImageResult>");
}

EmbeddedImageScanController::~EmbeddedImageScanController() {
    requestStop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

quint64 EmbeddedImageScanController::startScan(const EmbeddedImageScanRequest& request) {
    requestStop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    const quint64 scanId = m_nextScanId.fetch_add(1, std::memory_order_acq_rel);
    m_running.store(true, std::memory_order_release);
    emit scanStarted(scanId);
    m_worker = std::jthread([this, request, scanId](std::stop_token token) {
        EmbeddedImageScanSummary summary;
        ScanProgressTracker progressTracker;
        progressTracker.reset();
        auto lastProgress = ScanProgressTracker::Clock::time_point{};
        quint64 latestTotal = 0;
        int latestResultsFound = 0;
        int latestResultsLimit = request.options.maxResults;
        QVector<EmbeddedImageResult> results =
            scanEmbeddedImages(request.source, request.options, &summary,
                               [&token]() { return token.stop_requested(); },
                               [this, scanId, &progressTracker, &lastProgress, &latestTotal,
                                &latestResultsFound,
                                &latestResultsLimit](quint64 bytesScanned, quint64 bytesTotal,
                                                     quint64 rawBytesRead, int resultsFound,
                                                     int resultsLimit) mutable {
                                   const auto now = ScanProgressTracker::Clock::now();
                                   latestTotal = bytesTotal;
                                   latestResultsFound = resultsFound;
                                   latestResultsLimit = resultsLimit;
                                   if (lastProgress == ScanProgressTracker::Clock::time_point{}) {
                                       progressTracker.reset(bytesScanned, rawBytesRead, now);
                                       lastProgress = now;
                                       const ScanProgressSnapshot progress{
                                           bytesScanned, bytesTotal, rawBytesRead, 0.0, 0.0};
                                       QMetaObject::invokeMethod(
                                           this,
                                           [this, scanId, progress, resultsFound, resultsLimit]() {
                                               emit progressUpdated(scanId, progress, resultsFound,
                                                                    resultsLimit);
                                           },
                                           Qt::QueuedConnection);
                                       return;
                                   }
                                   if (bytesScanned != 0 && bytesScanned < bytesTotal &&
                                       now - lastProgress < std::chrono::seconds(2)) {
                                       return;
                                   }
                                   const ScanProgressSnapshot progress = progressTracker.sample(
                                       bytesScanned, bytesTotal, rawBytesRead, now);
                                   lastProgress = now;
                                   QMetaObject::invokeMethod(
                                       this,
                                       [this, scanId, progress,
                                        resultsFound, resultsLimit]() {
                                           emit progressUpdated(scanId, progress,
                                                                resultsFound, resultsLimit);
                                       },
                                       Qt::QueuedConnection);
                               },
                               [this, scanId](const EmbeddedImageResult& result) {
                                   QMetaObject::invokeMethod(
                                       this,
                                       [this, scanId, result]() { emit resultReady(scanId, result); },
                                       Qt::QueuedConnection);
                               });
        if (token.stop_requested()) {
            summary.cancelled = true;
        }
        const ScanProgressSnapshot finalProgress = progressTracker.sample(
            summary.bytesScanned, latestTotal, summary.rawBytesRead);
        QMetaObject::invokeMethod(
            this,
            [this, scanId, finalProgress, latestResultsFound, latestResultsLimit]() {
                emit progressUpdated(scanId, finalProgress, latestResultsFound,
                                     latestResultsLimit);
            },
            Qt::QueuedConnection);
        m_running.store(false, std::memory_order_release);
        QMetaObject::invokeMethod(this, [this, scanId, summary, results]() {
            emit scanFinished(scanId, summary, results);
        }, Qt::QueuedConnection);
    });
    return scanId;
}

void EmbeddedImageScanController::requestStop() {
    if (m_worker.joinable()) {
        m_worker.request_stop();
    }
}

bool EmbeddedImageScanController::isRunning() const {
    return m_running.load(std::memory_order_acquire);
}

void EmbeddedImageScanController::joinFinishedWorker() {
    if (!isRunning() && m_worker.joinable()) {
        m_worker.join();
    }
}

}  // namespace breco

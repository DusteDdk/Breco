#include "text/TextSequenceAnalyzer.h"

#include <algorithm>
#include <array>

#include <QChar>
#include <QStringDecoder>

namespace breco {

namespace {

constexpr quint32 kWordJoiner = 0x2060U;

bool isAsciiPrintable(unsigned char byte) { return byte >= 0x20 && byte <= 0x7E; }

TextByteClass classifyWhitespaceCodepoint(quint32 cp) {
    switch (cp) {
        case 0x0A:
            return TextByteClass::Newline;
        case 0x0D:
            return TextByteClass::CarriageReturn;
        case 0x09:
            return TextByteClass::Tab;
        case 0x20:
            return TextByteClass::Space;
        case 0x00A0:
            return TextByteClass::NonBreakingSpace;
        case 0x00AD:
            return TextByteClass::SoftHyphen;
        case kWordJoiner:
            return TextByteClass::OtherWhitespace;
        default:
            break;
    }

    if (QChar::isSpace(cp)) {
        return TextByteClass::OtherWhitespace;
    }
    return TextByteClass::Invalid;
}

bool isCodepointPrintable(quint32 cp) {
    if (cp > 0x10FFFFU) {
        return false;
    }
    if (cp >= 0xD800U && cp <= 0xDFFFU) {
        return false;
    }
    if (cp < 0x20U || (cp >= 0x7FU && cp <= 0x9FU)) {
        return false;
    }
    return true;
}

bool decodeUtf8At(const QByteArray& bytes, int index, quint32* codepointOut, int* lengthOut) {
    if (index < 0 || index >= bytes.size()) {
        return false;
    }

    const unsigned char b0 = static_cast<unsigned char>(bytes.at(index));
    if ((b0 & 0x80U) == 0U) {
        if (codepointOut != nullptr) {
            *codepointOut = b0;
        }
        if (lengthOut != nullptr) {
            *lengthOut = 1;
        }
        return true;
    }

    int length = 0;
    quint32 cp = 0;
    quint32 minCp = 0;
    if ((b0 & 0xE0U) == 0xC0U) {
        length = 2;
        cp = static_cast<quint32>(b0 & 0x1FU);
        minCp = 0x80U;
    } else if ((b0 & 0xF0U) == 0xE0U) {
        length = 3;
        cp = static_cast<quint32>(b0 & 0x0FU);
        minCp = 0x800U;
    } else if ((b0 & 0xF8U) == 0xF0U) {
        length = 4;
        cp = static_cast<quint32>(b0 & 0x07U);
        minCp = 0x10000U;
    } else {
        return false;
    }

    if (index + length > bytes.size()) {
        return false;
    }

    for (int i = 1; i < length; ++i) {
        const unsigned char bx = static_cast<unsigned char>(bytes.at(index + i));
        if ((bx & 0xC0U) != 0x80U) {
            return false;
        }
        cp = (cp << 6U) | static_cast<quint32>(bx & 0x3FU);
    }

    if (cp < minCp || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
        return false;
    }

    if (codepointOut != nullptr) {
        *codepointOut = cp;
    }
    if (lengthOut != nullptr) {
        *lengthOut = length;
    }
    return true;
}

quint16 readUtf16Unit(const QByteArray& bytes, int index, bool littleEndian) {
    if (index + 1 >= bytes.size()) {
        return 0;
    }
    const unsigned char b0 = static_cast<unsigned char>(bytes.at(index));
    const unsigned char b1 = static_cast<unsigned char>(bytes.at(index + 1));
    if (littleEndian) {
        return static_cast<quint16>(b0 | (static_cast<quint16>(b1) << 8U));
    }
    return static_cast<quint16>((static_cast<quint16>(b0) << 8U) | b1);
}

bool decodeUtf16At(const QByteArray& bytes, int index, bool littleEndian, quint32* codepointOut,
                  int* lengthOut) {
    if (index < 0 || index + 1 >= bytes.size()) {
        return false;
    }

    const quint16 u0 = readUtf16Unit(bytes, index, littleEndian);
    if (u0 >= 0xD800U && u0 <= 0xDBFFU) {
        if (index + 3 >= bytes.size()) {
            return false;
        }
        const quint16 u1 = readUtf16Unit(bytes, index + 2, littleEndian);
        if (u1 < 0xDC00U || u1 > 0xDFFFU) {
            return false;
        }
        const quint32 cp = 0x10000U +
                           ((static_cast<quint32>(u0 - 0xD800U) << 10U) |
                            static_cast<quint32>(u1 - 0xDC00U));
        if (codepointOut != nullptr) {
            *codepointOut = cp;
        }
        if (lengthOut != nullptr) {
            *lengthOut = 4;
        }
        return true;
    }

    if (u0 >= 0xDC00U && u0 <= 0xDFFFU) {
        return false;
    }

    if (codepointOut != nullptr) {
        *codepointOut = static_cast<quint32>(u0);
    }
    if (lengthOut != nullptr) {
        *lengthOut = 2;
    }
    return true;
}

void markBytes(QVector<TextByteClass>* classes, int start, int length, TextByteClass cls) {
    if (classes == nullptr || length <= 0) {
        return;
    }
    const int end = qMin(classes->size(), start + length);
    for (int i = qMax(0, start); i < end; ++i) {
        classes->operator[](i) = cls;
    }
}

void finalizeSequences(const QByteArray& bytes, TextAnalysisResult* out) {
    if (out == nullptr) {
        return;
    }

    out->sequenceIndexByByte.fill(-1, bytes.size());

    int i = 0;
    while (i < out->classes.size()) {
        if (out->classes.at(i) == TextByteClass::Invalid) {
            ++i;
            continue;
        }

        const int start = i;
        while (i < out->classes.size() && out->classes.at(i) != TextByteClass::Invalid) {
            ++i;
        }
        const int end = i;
        const int len = end - start;
        const bool followedByNull = (end < bytes.size() && static_cast<unsigned char>(bytes.at(end)) == 0x00U);
        if (len >= 5 || (len >= 2 && followedByNull)) {
            const int idx = out->sequences.size();
            out->sequences.push_back({start, end});
            for (int j = start; j < end; ++j) {
                out->sequenceIndexByByte[j] = idx;
            }
        }
    }
}

QString decodeUtf16Range(const QByteArray& bytes, bool littleEndian) {
    QStringDecoder decoder(littleEndian ? QStringDecoder::Utf16LE : QStringDecoder::Utf16BE);
    return decoder.decode(bytes);
}

}  // namespace

TextAnalysisResult TextSequenceAnalyzer::analyze(const QByteArray& bytes, TextInterpretationMode mode) {
    TextAnalysisResult result;
    result.classes.fill(TextByteClass::Invalid, bytes.size());
    result.sequenceIndexByByte.fill(-1, bytes.size());
    result.utf16LittleEndian = true;

    if (bytes.isEmpty()) {
        return result;
    }

    if (mode == TextInterpretationMode::Ascii) {
        for (int i = 0; i < bytes.size(); ++i) {
            const unsigned char byte = static_cast<unsigned char>(bytes.at(i));
            const TextByteClass ws = classifyWhitespaceCodepoint(byte);
            if (ws != TextByteClass::Invalid) {
                result.classes[i] = ws;
            } else if (isAsciiPrintable(byte)) {
                result.classes[i] = TextByteClass::Printable;
            }
        }
        finalizeSequences(bytes, &result);
        return result;
    }

    if (mode == TextInterpretationMode::Utf8) {
        int i = 0;
        while (i < bytes.size()) {
            quint32 cp = 0;
            int len = 0;
            if (!decodeUtf8At(bytes, i, &cp, &len)) {
                ++i;
                continue;
            }

            const TextByteClass ws = classifyWhitespaceCodepoint(cp);
            if (ws != TextByteClass::Invalid) {
                markBytes(&result.classes, i, len, ws);
            } else if (isCodepointPrintable(cp)) {
                markBytes(&result.classes, i, len, TextByteClass::Printable);
            }
            i += qMax(1, len);
        }
        finalizeSequences(bytes, &result);
        return result;
    }

    if (bytes.size() >= 2) {
        const unsigned char b0 = static_cast<unsigned char>(bytes.at(0));
        const unsigned char b1 = static_cast<unsigned char>(bytes.at(1));
        if (b0 == 0xFEU && b1 == 0xFFU) {
            result.utf16LittleEndian = false;
            result.classes[0] = TextByteClass::OtherWhitespace;
            result.classes[1] = TextByteClass::OtherWhitespace;
        } else if (b0 == 0xFFU && b1 == 0xFEU) {
            result.utf16LittleEndian = true;
            result.classes[0] = TextByteClass::OtherWhitespace;
            result.classes[1] = TextByteClass::OtherWhitespace;
        }
    }

    for (int i = 0; i + 1 < bytes.size(); i += 2) {
        quint32 cp = 0;
        int len = 0;
        if (!decodeUtf16At(bytes, i, result.utf16LittleEndian, &cp, &len)) {
            continue;
        }

        const TextByteClass ws = classifyWhitespaceCodepoint(cp);
        if (ws != TextByteClass::Invalid) {
            markBytes(&result.classes, i, len, ws);
        } else if (isCodepointPrintable(cp)) {
            markBytes(&result.classes, i, len, TextByteClass::Printable);
        }
    }

    finalizeSequences(bytes, &result);
    return result;
}

QString TextSequenceAnalyzer::decodeRange(const QByteArray& bytes, int startIndex, int length,
                                          TextInterpretationMode mode, bool utf16LittleEndian) {
    if (length <= 0 || bytes.isEmpty()) {
        return {};
    }

    const int start = qBound(0, startIndex, bytes.size());
    const int end = qBound(start, start + length, bytes.size());
    const QByteArray slice = bytes.mid(start, end - start);
    if (slice.isEmpty()) {
        return {};
    }

    if (mode == TextInterpretationMode::Ascii) {
        QString out;
        out.reserve(slice.size());
        for (const char ch : slice) {
            const unsigned char byte = static_cast<unsigned char>(ch);
            if (isAsciiPrintable(byte) || byte == '\n' || byte == '\r' || byte == '\t') {
                out.append(QChar::fromLatin1(static_cast<char>(byte)));
            } else {
                out.append(QChar(0xFFFD));
            }
        }
        return out;
    }

    if (mode == TextInterpretationMode::Utf8) {
        return QString::fromUtf8(slice);
    }

    return decodeUtf16Range(slice, utf16LittleEndian);
}

}  // namespace breco

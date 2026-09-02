#include "edit/EditQueue.h"

#include <QSet>
#include <algorithm>
#include <functional>
#include <limits>

namespace breco {

int EditQueue::detectBase(QString* text, int defaultBase) {
    QString trimmed = text->trimmed();
    int base = defaultBase;
    if (trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        base = 16;
        trimmed.remove(0, 2);
    } else if (trimmed.startsWith(QStringLiteral("0o"), Qt::CaseInsensitive)) {
        base = 8;
        trimmed.remove(0, 2);
    } else if (trimmed.startsWith(QStringLiteral("0b"), Qt::CaseInsensitive)) {
        base = 2;
        trimmed.remove(0, 2);
    } else if (trimmed.startsWith(QStringLiteral("0d"), Qt::CaseInsensitive)) {
        base = 10;
        trimmed.remove(0, 2);
    }
    *text = trimmed;
    return base;
}

std::optional<quint64> EditQueue::parseUnsigned(const QString& text, int defaultBase) {
    QString body = text;
    const int base = detectBase(&body, defaultBase);
    if (body.isEmpty()) {
        return std::nullopt;
    }
    bool ok = false;
    const quint64 value = body.toULongLong(&ok, base);
    if (!ok) {
        return std::nullopt;
    }
    return value;
}

std::optional<qint64> EditQueue::parseSigned(const QString& text, int defaultBase) {
    QString trimmed = text.trimmed();
    bool negative = false;
    if (trimmed.startsWith(QLatin1Char('-'))) {
        negative = true;
        trimmed.remove(0, 1);
    } else if (trimmed.startsWith(QLatin1Char('+'))) {
        trimmed.remove(0, 1);
    }
    const std::optional<quint64> magnitude = parseUnsigned(trimmed, defaultBase);
    if (!magnitude.has_value()) {
        return std::nullopt;
    }
    if (!negative) {
        if (magnitude.value() > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
            return std::nullopt;
        }
        return static_cast<qint64>(magnitude.value());
    }
    if (magnitude.value() > static_cast<quint64>(std::numeric_limits<qint64>::max()) + 1ULL) {
        return std::nullopt;
    }
    if (magnitude.value() == static_cast<quint64>(std::numeric_limits<qint64>::max()) + 1ULL) {
        return std::numeric_limits<qint64>::min();
    }
    return -static_cast<qint64>(magnitude.value());
}

QByteArray EditQueue::packUnsigned(quint64 value, int byteWidth, bool littleEndian) {
    QByteArray bytes;
    bytes.resize(byteWidth);
    for (int i = 0; i < byteWidth; ++i) {
        const int shift = littleEndian ? (i * 8) : ((byteWidth - 1 - i) * 8);
        bytes[i] = static_cast<char>((value >> shift) & 0xFFULL);
    }
    return bytes;
}

std::optional<QByteArray> EditQueue::packInteger(const QString& text, int byteWidth,
                                                 bool signedValue, bool littleEndian,
                                                 int defaultBase) {
    if (byteWidth <= 0 || byteWidth > 8) {
        return std::nullopt;
    }
    const quint64 maxUnsigned =
        (byteWidth == 8) ? std::numeric_limits<quint64>::max()
                         : ((1ULL << (byteWidth * 8)) - 1ULL);
    if (signedValue) {
        const std::optional<qint64> parsed = parseSigned(text, defaultBase);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        const qint64 minSigned = -static_cast<qint64>(1ULL << (byteWidth * 8 - 1));
        const qint64 maxSigned = static_cast<qint64>((1ULL << (byteWidth * 8 - 1)) - 1ULL);
        if (byteWidth == 8) {
            // qint64 already covers the full range.
        } else if (parsed.value() < minSigned || parsed.value() > maxSigned) {
            return std::nullopt;
        }
        quint64 raw = static_cast<quint64>(parsed.value());
        if (byteWidth < 8) {
            raw &= maxUnsigned;
        }
        return packUnsigned(raw, byteWidth, littleEndian);
    }
    const std::optional<quint64> parsed = parseUnsigned(text, defaultBase);
    if (!parsed.has_value() || parsed.value() > maxUnsigned) {
        return std::nullopt;
    }
    return packUnsigned(parsed.value(), byteWidth, littleEndian);
}

QString EditQueue::bytesToHex(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toHex());
}

QByteArray EditQueue::hexToBytes(const QString& hex) {
    return QByteArray::fromHex(hex.toLatin1());
}

int EditQueue::add(QueuedEdit edit) {
    m_edits.push_back(std::move(edit));
    return m_edits.size() - 1;
}

void EditQueue::removeAt(int index) {
    if (index < 0 || index >= m_edits.size()) {
        return;
    }
    m_edits.removeAt(index);
}

void EditQueue::removeIndices(QVector<int> indices) {
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    for (int index : indices) {
        removeAt(index);
    }
}

void EditQueue::setNewBytes(int index, QByteArray newBytes) {
    if (index < 0 || index >= m_edits.size()) {
        return;
    }
    m_edits[index].newBytes = std::move(newBytes);
}

void EditQueue::remapFilePath(const QString& from, const QString& to) {
    for (QueuedEdit& edit : m_edits) {
        if (edit.filePath == from) {
            edit.filePath = to;
        }
    }
}

QVector<QString> EditQueue::implicatedFiles() const {
    QVector<QString> files;
    QSet<QString> seen;
    for (const QueuedEdit& edit : m_edits) {
        if (seen.contains(edit.filePath)) {
            continue;
        }
        seen.insert(edit.filePath);
        files.push_back(edit.filePath);
    }
    return files;
}

QVector<QueuedEdit> EditQueue::mergedForApply() const {
    // Later queue entries win on overlapping bytes: apply in order.
    return m_edits;
}

std::optional<QByteArray> packTypedInteger(const QString& text, int byteWidth, bool signedValue,
                                           bool littleEndian) {
    return EditQueue::packInteger(text, byteWidth, signedValue, littleEndian, 10);
}

}  // namespace breco

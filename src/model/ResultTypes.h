#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace breco {

enum class TextInterpretationMode {
    Ascii = 0,
    Utf8,
    Utf16
};

enum class BitmapMode {
    Rgb24 = 0,
    Grey8,
    Grey24,
    Rgbi256,
    Binary,
    Text
};

enum class ShiftUnit {
    Bytes = 0,
    Bits
};

struct ShiftSettings {
    int amount = 0;
    ShiftUnit unit = ShiftUnit::Bytes;
};

struct ScanTarget {
    QString filePath;
    quint64 fileSize = 0;
};

struct MatchRecord {
    int scanTargetIdx = -1;
    int threadId = 0;
    quint64 offset = 0;
    quint64 searchTimeNs = 0;
};

struct ResultBuffer {
    int scanTargetIdx = -1;
    quint64 fileOffset = 0;
    QByteArray bytes;
};

}  // namespace breco

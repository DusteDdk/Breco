#pragma once

#include <QByteArray>
#include <QVector>
#include <QString>

#include "model/ResultTypes.h"

namespace breco {

enum class TextByteClass {
    Invalid = 0,
    Printable,
    Newline,
    CarriageReturn,
    Space,
    Tab,
    NonBreakingSpace,
    SoftHyphen,
    OtherWhitespace,
};

struct ValidTextSequence {
    int startIndex = 0;
    int endIndex = 0;  // exclusive
};

struct TextAnalysisResult {
    QVector<TextByteClass> classes;
    QVector<int> sequenceIndexByByte;
    QVector<ValidTextSequence> sequences;
    bool utf16LittleEndian = true;
};

class TextSequenceAnalyzer {
public:
    static TextAnalysisResult analyze(const QByteArray& bytes, TextInterpretationMode mode);
    static QString decodeRange(const QByteArray& bytes, int startIndex, int length,
                               TextInterpretationMode mode, bool utf16LittleEndian = true);
};

}  // namespace breco

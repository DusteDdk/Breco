#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <optional>
#include <variant>

namespace breco {

enum class Endianness {
    Native = 0,
    Little,
    Big,
};

enum class PodKind {
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Int8,
    Int16,
    Int32,
    Int64,
};

struct PodType {
    PodKind kind = PodKind::UInt8;
    Endianness endianness = Endianness::Native;
    QString decoration;
};

enum class StringEncoding {
    Ascii,
    Utf8,
    Utf16,
};

struct StringType {
    StringEncoding encoding = StringEncoding::Ascii;
    Endianness endianness = Endianness::Native;
    QString decoration;
};

struct ByteType {};

struct StructRefType {
    QString structName;
};

using ResolvedType = std::variant<PodType, StringType, ByteType, StructRefType>;

struct TextRange {
    int start = 0;
    int end = 0;
};

enum class ExpressionKind {
    Integer,
    String,
    Boolean,
    Variable,
};

struct ValueExpression {
    ExpressionKind kind = ExpressionKind::Integer;
    qint64 integerValue = 0;
    quint64 unsignedIntegerValue = 0;
    bool integerIsUnsigned = false;
    bool booleanValue = false;
    QString stringValue;
    QStringList variablePath;
    TextRange range;
};

enum class IntExpressionKind {
    Integer,
    Variable,
    UnaryMinus,
    Binary,
};

enum class IntBinaryOp {
    Add,
    Subtract,
    Multiply,
    Divide,
};

struct IntExpression {
    IntExpressionKind kind = IntExpressionKind::Integer;
    qint64 integerValue = 0;
    quint64 unsignedIntegerValue = 0;
    bool integerIsUnsigned = false;
    QStringList variablePath;
    IntBinaryOp binaryOp = IntBinaryOp::Add;
    std::shared_ptr<IntExpression> left;
    std::shared_ptr<IntExpression> right;
    TextRange range;
};

enum class ComparisonOperator {
    Equal,
    Less,
    Greater,
};

struct ComparisonExpression {
    ComparisonOperator op = ComparisonOperator::Equal;
    ValueExpression right;
    std::optional<IntExpression> leftIntegerExpression;
    std::optional<IntExpression> rightIntegerExpression;
    TextRange range;
    QString sourceText;
};

enum class LengthMode {
    None,
    Fixed,
    Maximum,
    Until,
};

struct BitfieldMember {
    int highBit = 0;
    int lowBit = 0;
    QString name;
    TextRange nameRange;
};

struct FieldAttributes {
    LengthMode lengthMode = LengthMode::None;
    std::optional<IntExpression> lengthExpression;
    std::optional<ComparisonExpression> untilExpression;
    QString variableName;
    std::optional<IntExpression> repeatExpression;
    std::optional<ComparisonExpression> conditionExpression;
    std::optional<ComparisonExpression> whenExpression;
    QString decoration;
    QString sourceRole;
    QVector<BitfieldMember> bitfields;

    bool hasDynamicExtent() const {
        return lengthMode != LengthMode::None || repeatExpression.has_value() ||
               whenExpression.has_value();
    }
};

inline int podKindWidthBytes(PodKind kind) {
    switch (kind) {
        case PodKind::UInt8:
        case PodKind::Int8:
            return 1;
        case PodKind::UInt16:
        case PodKind::Int16:
            return 2;
        case PodKind::UInt32:
        case PodKind::Int32:
            return 4;
        case PodKind::UInt64:
        case PodKind::Int64:
            return 8;
    }
    return 0;
}

inline bool podKindIsSigned(PodKind kind) {
    switch (kind) {
        case PodKind::Int8:
        case PodKind::Int16:
        case PodKind::Int32:
        case PodKind::Int64:
            return true;
        default:
            return false;
    }
}

inline QString podKindName(PodKind kind) {
    switch (kind) {
        case PodKind::UInt8:
            return QStringLiteral("uint8_t");
        case PodKind::UInt16:
            return QStringLiteral("uint16_t");
        case PodKind::UInt32:
            return QStringLiteral("uint32_t");
        case PodKind::UInt64:
            return QStringLiteral("uint64_t");
        case PodKind::Int8:
            return QStringLiteral("int8_t");
        case PodKind::Int16:
            return QStringLiteral("int16_t");
        case PodKind::Int32:
            return QStringLiteral("int32_t");
        case PodKind::Int64:
            return QStringLiteral("int64_t");
    }
    return QString();
}

inline QString endiannessLabel(Endianness endianness) {
    switch (endianness) {
        case Endianness::Native:
            return QStringLiteral("native");
        case Endianness::Little:
            return QStringLiteral("little");
        case Endianness::Big:
            return QStringLiteral("big");
    }
    return QStringLiteral("native");
}

inline QString stringEncodingName(StringEncoding encoding) {
    switch (encoding) {
        case StringEncoding::Ascii:
            return QStringLiteral("asciistr");
        case StringEncoding::Utf8:
            return QStringLiteral("utf8str");
        case StringEncoding::Utf16:
            return QStringLiteral("utf16str");
    }
    return QString();
}

}  // namespace breco

#include "struct/StructVisualizer.h"

#include <QHash>
#include <QStringList>

#include <limits>
#include <optional>
#include <variant>

namespace breco {

namespace {

constexpr qint64 kMaxDynamicElements = 1000000;

Endianness effectiveDecodeEndianness(Endianness declared, Endianness defaultEndianness) {
    return declared == Endianness::Native ? defaultEndianness : declared;
}

quint64 readUnsignedAt(const QByteArray& data, size_t offset, int widthBytes,
                       Endianness endianness, Endianness defaultEndianness, bool* ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (offset >= static_cast<size_t>(data.size()) || widthBytes <= 0 ||
        offset + static_cast<size_t>(widthBytes) > static_cast<size_t>(data.size())) {
        return 0;
    }
    const Endianness decode = effectiveDecodeEndianness(endianness, defaultEndianness);
    quint64 value = 0;
    if (decode == Endianness::Little) {
        for (int i = 0; i < widthBytes; ++i) {
            value |= static_cast<quint64>(
                         static_cast<unsigned char>(data.at(static_cast<int>(offset) + i)))
                     << (8 * i);
        }
    } else {
        for (int i = 0; i < widthBytes; ++i) {
            value = (value << 8U) |
                    static_cast<quint64>(
                        static_cast<unsigned char>(data.at(static_cast<int>(offset) + i)));
        }
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return value;
}

qint64 integerValue(quint64 unsignedValue, PodKind kind) {
    switch (kind) {
        case PodKind::Int8:
            return static_cast<qint8>(unsignedValue);
        case PodKind::Int16:
            return static_cast<qint16>(unsignedValue);
        case PodKind::Int32:
            return static_cast<qint32>(unsignedValue);
        case PodKind::Int64:
            return static_cast<qint64>(unsignedValue);
        default:
            return static_cast<qint64>(unsignedValue);
    }
}

QString formatIntegerValue(quint64 unsignedValue, PodKind kind) {
    const QString hex =
        QStringLiteral("0x%1").arg(unsignedValue, 0, 16, QChar('0')).toUpper();
    if (podKindIsSigned(kind)) {
        return QStringLiteral("%1 (%2)").arg(integerValue(unsignedValue, kind)).arg(hex);
    }
    return QStringLiteral("%1 (%2)").arg(static_cast<qulonglong>(unsignedValue)).arg(hex);
}

QString combineDecoration(const QString& typeDecoration, const QString& fieldDecoration) {
    if (typeDecoration.isEmpty()) {
        return fieldDecoration;
    }
    if (fieldDecoration.isEmpty()) {
        return typeDecoration;
    }
    return typeDecoration + QLatin1Char(' ') + fieldDecoration;
}

struct ReadCursor {
    const QByteArray* data = nullptr;
    size_t offset = 0;
    quint64 baseOffset = 0;
    QString filePath;

    size_t remaining() const {
        if (data == nullptr) {
            return 0;
        }
        return offset < static_cast<size_t>(data->size())
                   ? static_cast<size_t>(data->size()) - offset
                   : 0;
    }

    QByteArray bytesFrom(size_t start) const {
        if (data == nullptr || offset <= start) {
            return {};
        }
        return data->mid(static_cast<int>(start), static_cast<int>(offset - start));
    }
};

void setSourceOffset(VisualizedNode& node, const ReadCursor& cursor) {
    node.sourceOffset = cursor.baseOffset + static_cast<quint64>(cursor.offset);
    node.hasSourceOffset = true;
    node.sourceFilePath = cursor.filePath;
}

void updateSourceLengths(VisualizedNode& node) {
    node.sourceLength = static_cast<quint64>(qMax(0, node.rawBytes.size()));
    quint64 firstChildOffset = std::numeric_limits<quint64>::max();
    quint64 lastChildEnd = 0;
    bool hasChildSpan = false;
    for (VisualizedNode& child : node.children) {
        updateSourceLengths(child);
        if (!child.hasSourceOffset ||
            child.sourceLength >
                std::numeric_limits<quint64>::max() - child.sourceOffset) {
            continue;
        }
        const quint64 childEnd = child.sourceOffset + child.sourceLength;
        firstChildOffset = qMin(firstChildOffset, child.sourceOffset);
        lastChildEnd = qMax(lastChildEnd, childEnd);
        hasChildSpan = true;
        if (node.hasSourceOffset && childEnd >= node.sourceOffset) {
            node.sourceLength =
                qMax(node.sourceLength, childEnd - node.sourceOffset);
        }
    }
    if (!node.hasSourceOffset && hasChildSpan) {
        node.sourceLength = lastChildEnd - firstChildOffset;
    }
}

using EvaluatedValue = std::variant<qint64, quint64, QString, QByteArray, bool>;

struct DecodeContext {
    Endianness defaultEndianness = Endianness::Little;
    QVector<QHash<QString, EvaluatedValue>> scopes;
    QHash<QString, ReadCursor> externalCursors;

    void enterScope() { scopes.push_back({}); }

    QHash<QString, EvaluatedValue> leaveScope() {
        if (scopes.isEmpty()) {
            return {};
        }
        const QHash<QString, EvaluatedValue> values = scopes.back();
        scopes.pop_back();
        return values;
    }

    void bind(const QString& name, const EvaluatedValue& value) {
        if (!name.isEmpty() && !scopes.isEmpty()) {
            scopes.back().insert(name, value);
        }
    }

    std::optional<EvaluatedValue> evaluateVariable(const QStringList& path) const {
        const QString key = path.join(QLatin1Char('.'));
        for (int i = scopes.size() - 1; i >= 0; --i) {
            const auto found = scopes.at(i).constFind(key);
            if (found != scopes.at(i).constEnd()) {
                return *found;
            }
        }
        return std::nullopt;
    }

    std::optional<EvaluatedValue> evaluate(const ValueExpression& expression) const {
        if (expression.kind == ExpressionKind::Integer) {
            return expression.integerIsUnsigned
                       ? EvaluatedValue{expression.unsignedIntegerValue}
                       : EvaluatedValue{expression.integerValue};
        }
        if (expression.kind == ExpressionKind::String) {
            return EvaluatedValue{expression.stringValue};
        }
        if (expression.kind == ExpressionKind::Boolean) {
            return EvaluatedValue{expression.booleanValue};
        }
        return evaluateVariable(expression.variablePath);
    }
};

struct DecodeResult {
    VisualizedNode node;
    std::optional<EvaluatedValue> scalarValue;
    QHash<QString, EvaluatedValue> exports;
    bool valid = true;
    bool invalidatesContainer = false;
    bool hasNode = true;
};

void invalidate(DecodeResult& result, const QString& message) {
    result.valid = false;
    result.node.valid = false;
    result.node.errorMessage = message;
    if (result.node.valueText.isEmpty()) {
        result.node.valueText = QStringLiteral("invalid: %1").arg(message);
    } else {
        result.node.valueText += QStringLiteral(" [invalid: %1]").arg(message);
    }
}

// Overflow-checked qint64 arithmetic. These avoid __int128 (unsupported by
// MSVC) and avoid ever letting a signed operation actually overflow (which is
// undefined behavior in C++); every branch below is a pre-check on the
// inputs, followed by an operation that is guaranteed to be in-range.
bool addWithOverflowCheck(qint64 left, qint64 right, qint64* result) {
    constexpr qint64 kMax = std::numeric_limits<qint64>::max();
    constexpr qint64 kMin = std::numeric_limits<qint64>::min();
    if (right >= 0) {
        if (left > kMax - right) {
            return false;
        }
    } else {
        if (left < kMin - right) {
            return false;
        }
    }
    *result = left + right;
    return true;
}

bool subtractWithOverflowCheck(qint64 left, qint64 right, qint64* result) {
    constexpr qint64 kMax = std::numeric_limits<qint64>::max();
    constexpr qint64 kMin = std::numeric_limits<qint64>::min();
    if (right >= 0) {
        if (left < kMin + right) {
            return false;
        }
    } else {
        if (left > kMax + right) {
            return false;
        }
    }
    *result = left - right;
    return true;
}

bool multiplyWithOverflowCheck(qint64 left, qint64 right, qint64* result) {
    constexpr qint64 kMax = std::numeric_limits<qint64>::max();
    constexpr qint64 kMin = std::numeric_limits<qint64>::min();
    if (left == 0 || right == 0) {
        *result = 0;
        return true;
    }
    // Handle -1 specially: it is the only factor for which negating the
    // other operand could itself overflow (kMin * -1), and it also makes the
    // divisor used below potentially equal to -1, which is unsafe to divide
    // kMin by.
    if (left == -1) {
        if (right == kMin) {
            return false;
        }
        *result = -right;
        return true;
    }
    if (right == -1) {
        if (left == kMin) {
            return false;
        }
        *result = -left;
        return true;
    }
    // Neither operand is 0 or -1 here, so dividing by either is always
    // well-defined (never 0/x and never kMin/-1).
    if (left > 0) {
        if (right > 0) {
            if (left > kMax / right) {
                return false;
            }
        } else {
            if (right < kMin / left) {
                return false;
            }
        }
    } else {
        if (right > 0) {
            if (left < kMin / right) {
                return false;
            }
        } else {
            if (left < kMax / right) {
                return false;
            }
        }
    }
    *result = left * right;
    return true;
}

bool integerValueToSigned(const EvaluatedValue& value, qint64* out, QString* error) {
    if (const auto* integer = std::get_if<qint64>(&value)) {
        *out = *integer;
        return true;
    }
    if (const auto* integer = std::get_if<quint64>(&value)) {
        if (*integer > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
            if (error != nullptr) {
                *error = QStringLiteral("Integer expression value is too large for arithmetic");
            }
            return false;
        }
        *out = static_cast<qint64>(*integer);
        return true;
    }
    if (error != nullptr) {
        *error = QStringLiteral("Integer expression must evaluate to an integer");
    }
    return false;
}

std::optional<EvaluatedValue> evaluateIntExpression(const IntExpression& expression,
                                                    const DecodeContext& context,
                                                    QString* error) {
    switch (expression.kind) {
        case IntExpressionKind::Integer:
            return expression.integerIsUnsigned
                       ? EvaluatedValue{expression.unsignedIntegerValue}
                       : EvaluatedValue{expression.integerValue};
        case IntExpressionKind::Variable: {
            const std::optional<EvaluatedValue> value =
                context.evaluateVariable(expression.variablePath);
            if (!value.has_value() && error != nullptr) {
                *error = QStringLiteral("Unknown variable '$%1'")
                             .arg(expression.variablePath.join(QLatin1Char('.')));
            }
            return value;
        }
        case IntExpressionKind::UnaryMinus: {
            if (expression.left == nullptr) {
                if (error != nullptr) {
                    *error = QStringLiteral("Invalid unary integer expression");
                }
                return std::nullopt;
            }
            const std::optional<EvaluatedValue> value =
                evaluateIntExpression(*expression.left, context, error);
            if (!value.has_value()) {
                return std::nullopt;
            }
            if (const auto* signedValue = std::get_if<qint64>(&*value)) {
                if (*signedValue == std::numeric_limits<qint64>::min()) {
                    if (error != nullptr) {
                        *error = QStringLiteral("Integer expression overflow");
                    }
                    return std::nullopt;
                }
                return EvaluatedValue{-*signedValue};
            }
            const auto* maybeUnsigned = std::get_if<quint64>(&*value);
            if (maybeUnsigned == nullptr) {
                if (error != nullptr) {
                    *error = QStringLiteral("Integer expression must evaluate to an integer");
                }
                return std::nullopt;
            }
            const quint64 unsignedValue = *maybeUnsigned;
            const quint64 signedLimit =
                static_cast<quint64>(std::numeric_limits<qint64>::max()) + 1U;
            if (unsignedValue > signedLimit) {
                if (error != nullptr) {
                    *error = QStringLiteral("Integer expression overflow");
                }
                return std::nullopt;
            }
            return EvaluatedValue{unsignedValue == signedLimit
                                      ? std::numeric_limits<qint64>::min()
                                      : -static_cast<qint64>(unsignedValue)};
        }
        case IntExpressionKind::Binary: {
            if (expression.left == nullptr || expression.right == nullptr) {
                if (error != nullptr) {
                    *error = QStringLiteral("Invalid binary integer expression");
                }
                return std::nullopt;
            }
            const std::optional<EvaluatedValue> left =
                evaluateIntExpression(*expression.left, context, error);
            const std::optional<EvaluatedValue> right =
                evaluateIntExpression(*expression.right, context, error);
            if (!left.has_value() || !right.has_value()) {
                return std::nullopt;
            }
            qint64 leftValue = 0;
            qint64 rightValue = 0;
            if (!integerValueToSigned(*left, &leftValue, error) ||
                !integerValueToSigned(*right, &rightValue, error)) {
                return std::nullopt;
            }
            if (expression.binaryOp == IntBinaryOp::Divide && rightValue == 0) {
                if (error != nullptr) {
                    *error = QStringLiteral("Division by zero in integer expression");
                }
                return std::nullopt;
            }
            qint64 result = 0;
            bool ok = true;
            switch (expression.binaryOp) {
                case IntBinaryOp::Add:
                    ok = addWithOverflowCheck(leftValue, rightValue, &result);
                    break;
                case IntBinaryOp::Subtract:
                    ok = subtractWithOverflowCheck(leftValue, rightValue, &result);
                    break;
                case IntBinaryOp::Multiply:
                    ok = multiplyWithOverflowCheck(leftValue, rightValue, &result);
                    break;
                case IntBinaryOp::Divide:
                    // leftValue / rightValue is itself undefined behavior when
                    // leftValue is INT64_MIN and rightValue is -1 (the only
                    // signed-division case that overflows), so it must be
                    // checked before the division executes.
                    if (leftValue == std::numeric_limits<qint64>::min() && rightValue == -1) {
                        ok = false;
                    } else {
                        result = leftValue / rightValue;
                    }
                    break;
            }
            if (!ok) {
                if (error != nullptr) {
                    *error = QStringLiteral("Integer expression overflow");
                }
                return std::nullopt;
            }
            return EvaluatedValue{result};
        }
    }
    return std::nullopt;
}

bool evaluateCount(const IntExpression& expression, const DecodeContext& context,
                   qint64* count, QString* error) {
    const std::optional<EvaluatedValue> evaluated =
        evaluateIntExpression(expression, context, error);
    if (!evaluated.has_value()) {
        return false;
    }
    qint64 signedCount = 0;
    if (const auto* integer = std::get_if<qint64>(&*evaluated)) {
        if (*integer < 0) {
            if (error != nullptr) {
                *error = QStringLiteral("Length and repeat expressions cannot be negative");
            }
            return false;
        }
        signedCount = *integer;
    } else if (const auto* integer = std::get_if<quint64>(&*evaluated)) {
        if (*integer > static_cast<quint64>(kMaxDynamicElements)) {
            if (error != nullptr) {
                *error = QStringLiteral("Dynamic element count exceeds %1")
                             .arg(kMaxDynamicElements);
            }
            return false;
        }
        signedCount = static_cast<qint64>(*integer);
    } else {
        if (error != nullptr) {
            *error = QStringLiteral("Length and repeat expressions must evaluate to integers");
        }
        return false;
    }
    if (signedCount > kMaxDynamicElements) {
        if (error != nullptr) {
            *error = QStringLiteral("Dynamic element count exceeds %1")
                         .arg(kMaxDynamicElements);
        }
        return false;
    }
    *count = signedCount;
    return true;
}

bool compareValues(const EvaluatedValue& left, const ComparisonExpression& comparison,
                   const DecodeContext& context, bool* comparable) {
    if (comparable != nullptr) {
        *comparable = false;
    }
    QString expressionError;
    const std::optional<EvaluatedValue> right =
        comparison.rightIntegerExpression.has_value()
            ? evaluateIntExpression(*comparison.rightIntegerExpression, context,
                                    &expressionError)
            : context.evaluate(comparison.right);
    if (!right.has_value()) {
        return false;
    }
    const auto compareOrdered = [&](int ordering) {
        if (comparable != nullptr) {
            *comparable = true;
        }
        switch (comparison.op) {
            case ComparisonOperator::Equal:
                return ordering == 0;
            case ComparisonOperator::Less:
                return ordering < 0;
            case ComparisonOperator::Greater:
                return ordering > 0;
        }
        return false;
    };
    const auto* leftSigned = std::get_if<qint64>(&left);
    const auto* leftUnsigned = std::get_if<quint64>(&left);
    const auto* rightSigned = std::get_if<qint64>(&*right);
    const auto* rightUnsigned = std::get_if<quint64>(&*right);
    if (const auto* rightBoolean = std::get_if<bool>(&*right)) {
        bool leftBoolean = false;
        if (const auto* boolean = std::get_if<bool>(&left)) {
            leftBoolean = *boolean;
        } else if (leftSigned != nullptr) {
            leftBoolean = *leftSigned != 0;
        } else if (leftUnsigned != nullptr) {
            leftBoolean = *leftUnsigned != 0;
        } else {
            return false;
        }
        if (comparable != nullptr) {
            *comparable = true;
        }
        return comparison.op == ComparisonOperator::Equal &&
               leftBoolean == *rightBoolean;
    }
    if (leftSigned != nullptr && rightSigned != nullptr) {
        return compareOrdered(*leftSigned < *rightSigned
                                  ? -1
                                  : (*leftSigned > *rightSigned ? 1 : 0));
    }
    if (leftUnsigned != nullptr && rightUnsigned != nullptr) {
        return compareOrdered(*leftUnsigned < *rightUnsigned
                                  ? -1
                                  : (*leftUnsigned > *rightUnsigned ? 1 : 0));
    }
    if (leftSigned != nullptr && rightUnsigned != nullptr) {
        if (*leftSigned < 0) {
            return compareOrdered(-1);
        }
        const quint64 converted = static_cast<quint64>(*leftSigned);
        return compareOrdered(converted < *rightUnsigned
                                  ? -1
                                  : (converted > *rightUnsigned ? 1 : 0));
    }
    if (leftUnsigned != nullptr && rightSigned != nullptr) {
        if (*rightSigned < 0) {
            return compareOrdered(1);
        }
        const quint64 converted = static_cast<quint64>(*rightSigned);
        return compareOrdered(*leftUnsigned < converted
                                  ? -1
                                  : (*leftUnsigned > converted ? 1 : 0));
    }
    if (const auto* leftString = std::get_if<QString>(&left)) {
        const auto* rightString = std::get_if<QString>(&*right);
        if (rightString == nullptr || comparison.op != ComparisonOperator::Equal) {
            return false;
        }
        if (comparable != nullptr) {
            *comparable = true;
        }
        return *leftString == *rightString;
    }
    if (const auto* leftBytes = std::get_if<QByteArray>(&left)) {
        const auto* rightString = std::get_if<QString>(&*right);
        if (rightString == nullptr || comparison.op != ComparisonOperator::Equal) {
            return false;
        }
        if (comparable != nullptr) {
            *comparable = true;
        }
        return *leftBytes == rightString->toUtf8();
    }
    return false;
}

bool compareIntegerExpressions(const ComparisonExpression& comparison,
                               const DecodeContext& context, bool* comparable,
                               QString* error) {
    if (comparable != nullptr) {
        *comparable = false;
    }
    if (!comparison.leftIntegerExpression.has_value() ||
        !comparison.rightIntegerExpression.has_value()) {
        if (error != nullptr) {
            *error = QStringLiteral("Expected integer comparison expression");
        }
        return false;
    }
    const std::optional<EvaluatedValue> left =
        evaluateIntExpression(*comparison.leftIntegerExpression, context, error);
    const std::optional<EvaluatedValue> right =
        evaluateIntExpression(*comparison.rightIntegerExpression, context, error);
    if (!left.has_value() || !right.has_value()) {
        return false;
    }

    ComparisonExpression evaluated;
    evaluated.op = comparison.op;
    if (const auto* signedRight = std::get_if<qint64>(&*right)) {
        evaluated.right.kind = ExpressionKind::Integer;
        evaluated.right.integerValue = *signedRight;
        evaluated.right.integerIsUnsigned = false;
    } else if (const auto* unsignedRight = std::get_if<quint64>(&*right)) {
        evaluated.right.kind = ExpressionKind::Integer;
        evaluated.right.unsignedIntegerValue = *unsignedRight;
        evaluated.right.integerIsUnsigned = true;
    } else {
        if (error != nullptr) {
            *error = QStringLiteral("Integer comparison expression must evaluate to integers");
        }
        return false;
    }
    return compareValues(*left, evaluated, context, comparable);
}

void appendBitfieldChildren(VisualizedNode& node, quint64 value,
                            const QVector<BitfieldMember>& bitfields) {
    for (const BitfieldMember& member : bitfields) {
        const int width = member.highBit - member.lowBit + 1;
        const quint64 mask = width >= 64
                                 ? std::numeric_limits<quint64>::max()
                                 : ((quint64{1} << width) - 1U);
        const quint64 extracted = (value >> member.lowBit) & mask;

        VisualizedNode child;
        child.name = member.name;
        child.typeName = member.highBit == member.lowBit
                             ? QStringLiteral("bit %1").arg(member.lowBit)
                             : QStringLiteral("bits %1:%2")
                                   .arg(member.highBit)
                                   .arg(member.lowBit);
        child.valueText = QStringLiteral("%1 (0X%2)")
                              .arg(static_cast<qulonglong>(extracted))
                              .arg(extracted, 0, 16, QChar('0'))
                              .toUpper();
        child.scalarKind = VisualizedScalarKind::UnsignedInteger;
        child.unsignedValue = extracted;
        child.declarationRange = member.nameRange;
        child.hasSourceOffset = node.hasSourceOffset;
        child.sourceOffset = node.sourceOffset;
        child.sourceFilePath = node.sourceFilePath;
        node.children.push_back(child);
    }
}

DecodeResult decodePodScalar(const QString& name, const QString& typeName,
                             const PodType& pod, const QString& fieldDecoration,
                             const QVector<BitfieldMember>& bitfields,
                             ReadCursor& cursor, const DecodeContext& context) {
    DecodeResult result;
    setSourceOffset(result.node, cursor);
    result.node.name = name;
    result.node.typeName = typeName;
    result.node.decoration = combineDecoration(pod.decoration, fieldDecoration);
    result.node.endianness = endiannessLabel(pod.endianness);
    const int width = podKindWidthBytes(pod.kind);
    result.node.scalarKind = podKindIsSigned(pod.kind)
                                 ? VisualizedScalarKind::SignedInteger
                                 : VisualizedScalarKind::UnsignedInteger;
    result.node.declaredEndianness = pod.endianness;
    result.node.effectiveEndianness =
        effectiveDecodeEndianness(pod.endianness, context.defaultEndianness);
    result.node.byteOrderUnitWidth =
        pod.endianness == Endianness::Native ? 0 : width;
    const int available =
        static_cast<int>(qMin(static_cast<size_t>(width), cursor.remaining()));
    if (available == 0) {
        result.node.valueText = QStringLiteral("n/a");
        result.node.bytesMissing = width;
        return result;
    }
    const size_t start = cursor.offset;
    result.node.rawBytes = cursor.data->mid(static_cast<int>(start), available);
    result.node.bytesMissing = width - available;
    cursor.offset += static_cast<size_t>(available);
    if (available != width) {
        result.node.valueText = QStringLiteral("partial");
        return result;
    }
    bool ok = false;
    const quint64 value = readUnsignedAt(*cursor.data, start, width, pod.endianness,
                                         context.defaultEndianness, &ok);
    if (!ok) {
        result.node.valueText = QStringLiteral("n/a");
        return result;
    }
    result.node.valueText = formatIntegerValue(value, pod.kind);
    if (podKindIsSigned(pod.kind)) {
        const qint64 signedValue = integerValue(value, pod.kind);
        result.node.signedValue = signedValue;
        result.scalarValue = EvaluatedValue{signedValue};
    } else {
        result.node.unsignedValue = value;
        result.scalarValue = EvaluatedValue{value};
    }
    appendBitfieldChildren(result.node, value, bitfields);
    return result;
}

DecodeResult decodeByteScalar(const QString& name, const QString& fieldDecoration,
                              ReadCursor& cursor, const DecodeContext& context) {
    PodType bytePod{PodKind::UInt8, Endianness::Native, {}};
    DecodeResult result =
        decodePodScalar(name, QStringLiteral("byte"), bytePod, fieldDecoration,
                        {}, cursor, context);
    result.node.endianness = QStringLiteral("n/a");
    result.node.scalarKind = VisualizedScalarKind::Bytes;
    result.node.byteOrderUnitWidth = 0;
    return result;
}

QString decodeString(const QByteArray& bytes, const StringType& type,
                     Endianness defaultEndianness) {
    if (type.encoding == StringEncoding::Ascii) {
        return QString::fromLatin1(bytes);
    }
    if (type.encoding == StringEncoding::Utf8) {
        return QString::fromUtf8(bytes);
    }
    QString decoded;
    const Endianness endian =
        effectiveDecodeEndianness(type.endianness, defaultEndianness);
    for (int i = 0; i + 1 < bytes.size(); i += 2) {
        const quint16 first = static_cast<unsigned char>(bytes.at(i));
        const quint16 second = static_cast<unsigned char>(bytes.at(i + 1));
        const quint16 codeUnit =
            endian == Endianness::Little ? static_cast<quint16>(first | (second << 8U))
                                         : static_cast<quint16>((first << 8U) | second);
        decoded += QChar(codeUnit);
    }
    return decoded;
}

QByteArray encodeString(const QString& text, const StringType& type,
                        Endianness defaultEndianness) {
    if (type.encoding == StringEncoding::Ascii) {
        return text.toLatin1();
    }
    if (type.encoding == StringEncoding::Utf8) {
        return text.toUtf8();
    }
    QByteArray encoded;
    const Endianness endian =
        effectiveDecodeEndianness(type.endianness, defaultEndianness);
    for (QChar ch : text) {
        const quint16 value = ch.unicode();
        if (endian == Endianness::Little) {
            encoded.push_back(static_cast<char>(value & 0xffU));
            encoded.push_back(static_cast<char>((value >> 8U) & 0xffU));
        } else {
            encoded.push_back(static_cast<char>((value >> 8U) & 0xffU));
            encoded.push_back(static_cast<char>(value & 0xffU));
        }
    }
    return encoded;
}

struct UntilScan {
    size_t byteCount = 0;
    bool found = false;
    QString error;
};

UntilScan scanByteUntil(const ComparisonExpression& comparison, ReadCursor& cursor,
                        const DecodeContext& context) {
    UntilScan scan;
    QString expressionError;
    const std::optional<EvaluatedValue> right =
        comparison.rightIntegerExpression.has_value()
            ? evaluateIntExpression(*comparison.rightIntegerExpression, context,
                                    &expressionError)
            : context.evaluate(comparison.right);
    if (!right.has_value()) {
        scan.error = expressionError.isEmpty()
                         ? QStringLiteral("Unable to evaluate <until> expression")
                         : expressionError;
        return scan;
    }
    if (const auto* string = std::get_if<QString>(&*right)) {
        if (comparison.op != ComparisonOperator::Equal) {
            scan.error = QStringLiteral("String <until> expressions only support '='");
            return scan;
        }
        const QByteArray sentinel = string->toUtf8();
        if (sentinel.isEmpty()) {
            scan.error = QStringLiteral("<until> sentinel cannot be empty");
            return scan;
        }
        for (size_t i = 0; i + static_cast<size_t>(sentinel.size()) <= cursor.remaining();
             ++i) {
            if (cursor.data->mid(static_cast<int>(cursor.offset + i), sentinel.size()) ==
                sentinel) {
                scan.byteCount = i;
                scan.found = true;
                return scan;
            }
        }
        scan.byteCount = cursor.remaining();
        return scan;
    }
    for (size_t i = 0; i < cursor.remaining(); ++i) {
        const qint64 value =
            static_cast<unsigned char>(cursor.data->at(static_cast<int>(cursor.offset + i)));
        bool comparable = false;
        if (compareValues(EvaluatedValue{value}, comparison, context, &comparable)) {
            scan.byteCount = i;
            scan.found = true;
            return scan;
        }
        if (!comparable) {
            scan.error = QStringLiteral("Invalid numeric <until> comparison");
            return scan;
        }
    }
    scan.byteCount = cursor.remaining();
    return scan;
}

UntilScan scanStringUntil(const StringType& type,
                          const ComparisonExpression& comparison, ReadCursor& cursor,
                          const DecodeContext& context) {
    QString expressionError;
    const std::optional<EvaluatedValue> right =
        comparison.rightIntegerExpression.has_value()
            ? evaluateIntExpression(*comparison.rightIntegerExpression, context,
                                    &expressionError)
            : context.evaluate(comparison.right);
    if (!right.has_value()) {
        return UntilScan{0, false,
                         expressionError.isEmpty()
                             ? QStringLiteral("Unable to evaluate <until> expression")
                             : expressionError};
    }
    if (const auto* string = std::get_if<QString>(&*right)) {
        if (comparison.op != ComparisonOperator::Equal) {
            return UntilScan{0, false,
                             QStringLiteral("String <until> expressions only support '='")};
        }
        const QByteArray sentinel =
            encodeString(*string, type, context.defaultEndianness);
        if (sentinel.isEmpty()) {
            return UntilScan{0, false,
                             QStringLiteral("<until> sentinel cannot be empty")};
        }
        const size_t step = type.encoding == StringEncoding::Utf16 ? 2U : 1U;
        for (size_t i = 0; i + static_cast<size_t>(sentinel.size()) <= cursor.remaining();
             i += step) {
            if (cursor.data->mid(static_cast<int>(cursor.offset + i), sentinel.size()) ==
                sentinel) {
                return UntilScan{i, true, {}};
            }
        }
        return UntilScan{cursor.remaining(), false, {}};
    }
    const int width = type.encoding == StringEncoding::Utf16 ? 2 : 1;
    const Endianness endian = type.encoding == StringEncoding::Utf16
                                  ? type.endianness
                                  : Endianness::Native;
    for (size_t i = 0; i + static_cast<size_t>(width) <= cursor.remaining();
         i += static_cast<size_t>(width)) {
        bool ok = false;
        const quint64 raw = readUnsignedAt(*cursor.data, cursor.offset + i, width,
                                           endian, context.defaultEndianness, &ok);
        bool comparable = false;
        if (ok && compareValues(EvaluatedValue{static_cast<qint64>(raw)}, comparison,
                                context, &comparable)) {
            return UntilScan{i, true, {}};
        }
        if (!comparable) {
            return UntilScan{0, false,
                             QStringLiteral("Invalid numeric <until> comparison")};
        }
    }
    return UntilScan{cursor.remaining(), false, {}};
}

DecodeResult decodeByteField(const QString& name, const FieldAttributes& attributes,
                             ReadCursor& cursor, const DecodeContext& context) {
    if (attributes.lengthMode == LengthMode::None) {
        return decodeByteScalar(name, attributes.decoration, cursor, context);
    }

    DecodeResult result;
    setSourceOffset(result.node, cursor);
    result.node.name = name;
    result.node.typeName = QStringLiteral("byte");
    result.node.decoration = attributes.decoration;
    result.node.endianness = QStringLiteral("n/a");
    result.node.scalarKind = VisualizedScalarKind::Bytes;
    const size_t start = cursor.offset;
    size_t requested = 0;

    if (attributes.lengthMode == LengthMode::Until) {
        const UntilScan scan =
            scanByteUntil(*attributes.untilExpression, cursor, context);
        if (!scan.error.isEmpty()) {
            invalidate(result, scan.error);
            return result;
        }
        requested = scan.byteCount;
        cursor.offset += requested;
        if (!scan.found) {
            invalidate(result, QStringLiteral("<until> condition was not found"));
        }
    } else {
        qint64 count = 0;
        QString error;
        if (!evaluateCount(*attributes.lengthExpression, context, &count, &error)) {
            invalidate(result, error);
            return result;
        }
        requested = static_cast<size_t>(count);
        const size_t available = qMin(requested, cursor.remaining());
        cursor.offset += available;
        if (attributes.lengthMode == LengthMode::Fixed) {
            result.node.bytesMissing = static_cast<int>(requested - available);
        }
    }
    result.node.rawBytes = cursor.bytesFrom(start);
    result.node.valueText =
        QStringLiteral("%1 byte(s)").arg(result.node.rawBytes.size());
    result.scalarValue = EvaluatedValue{result.node.rawBytes};
    return result;
}

DecodeResult decodeStringField(const QString& name, const QString& typeName,
                               const StringType& type,
                               const FieldAttributes& attributes, ReadCursor& cursor,
                               const DecodeContext& context) {
    DecodeResult result;
    setSourceOffset(result.node, cursor);
    result.node.name = name;
    result.node.typeName = typeName;
    result.node.decoration =
        combineDecoration(type.decoration, attributes.decoration);
    result.node.endianness =
        type.encoding == StringEncoding::Utf16 ? endiannessLabel(type.endianness)
                                               : QStringLiteral("n/a");
    result.node.scalarKind = VisualizedScalarKind::String;
    result.node.stringEncoding = type.encoding;
    result.node.declaredEndianness = type.endianness;
    result.node.effectiveEndianness =
        type.encoding == StringEncoding::Utf16
            ? effectiveDecodeEndianness(type.endianness,
                                        context.defaultEndianness)
            : Endianness::Native;
    result.node.byteOrderUnitWidth =
        type.encoding == StringEncoding::Utf16 &&
                type.endianness != Endianness::Native
            ? 2
            : 0;
    const size_t start = cursor.offset;
    size_t contentBytes = 0;

    if (attributes.lengthMode == LengthMode::Until) {
        const UntilScan scan =
            scanStringUntil(type, *attributes.untilExpression, cursor, context);
        if (!scan.error.isEmpty()) {
            invalidate(result, scan.error);
            return result;
        }
        contentBytes = scan.byteCount;
        cursor.offset += contentBytes;
        if (!scan.found) {
            invalidate(result, QStringLiteral("<until> condition was not found"));
        }
    } else if (attributes.lengthMode == LengthMode::Fixed) {
        qint64 count = 0;
        QString error;
        if (!evaluateCount(*attributes.lengthExpression, context, &count, &error)) {
            invalidate(result, error);
            return result;
        }
        const size_t requested = static_cast<size_t>(count);
        contentBytes = qMin(requested, cursor.remaining());
        cursor.offset += contentBytes;
        result.node.bytesMissing = static_cast<int>(requested - contentBytes);
    } else {
        size_t limit = cursor.remaining();
        if (attributes.lengthMode == LengthMode::Maximum) {
            qint64 count = 0;
            QString error;
            if (!evaluateCount(*attributes.lengthExpression, context, &count, &error)) {
                invalidate(result, error);
                return result;
            }
            limit = qMin(limit, static_cast<size_t>(count));
        }
        const int terminatorWidth =
            type.encoding == StringEncoding::Utf16 ? 2 : 1;
        const size_t step = static_cast<size_t>(terminatorWidth);
        bool foundTerminator = false;
        for (size_t i = 0; i + step <= limit; i += step) {
            bool isTerminator = true;
            for (int byte = 0; byte < terminatorWidth; ++byte) {
                if (cursor.data->at(static_cast<int>(cursor.offset + i) + byte) != 0) {
                    isTerminator = false;
                    break;
                }
            }
            if (isTerminator) {
                contentBytes = i;
                cursor.offset += i + step;
                foundTerminator = true;
                break;
            }
        }
        if (!foundTerminator) {
            contentBytes = limit;
            cursor.offset += limit;
            if (attributes.lengthMode == LengthMode::None) {
                invalidate(result, QStringLiteral("String terminator was not found"));
            }
        }
    }

    result.node.rawBytes = cursor.bytesFrom(start);
    const QByteArray content =
        cursor.data->mid(static_cast<int>(start), static_cast<int>(contentBytes));
    const QString decoded =
        decodeString(content, type, context.defaultEndianness);
    result.node.valueText = decoded;
    result.node.stringValue = decoded;
    result.scalarValue = EvaluatedValue{decoded};
    if (type.encoding == StringEncoding::Utf16 && contentBytes % 2U != 0U) {
        invalidate(result,
                   QStringLiteral("UTF-16 field ended with an incomplete code unit"));
    }
    return result;
}

DecodeResult decodePodField(const QString& name, const QString& typeName,
                            const PodType& pod, const FieldAttributes& attributes,
                            ReadCursor& cursor, const DecodeContext& context) {
    if (attributes.lengthMode == LengthMode::None) {
        return decodePodScalar(name, typeName, pod, attributes.decoration,
                               attributes.bitfields, cursor, context);
    }

    DecodeResult result;
    setSourceOffset(result.node, cursor);
    result.node.name = name;
    result.node.typeName = typeName + QStringLiteral("[]");
    result.node.valueKind = VisualizedValueKind::Array;
    result.node.decoration =
        combineDecoration(pod.decoration, attributes.decoration);
    result.node.endianness = endiannessLabel(pod.endianness);
    const size_t start = cursor.offset;
    const int width = podKindWidthBytes(pod.kind);
    qint64 requestedCount = 0;

    if (attributes.lengthMode == LengthMode::Until) {
        bool foundMatch = false;
        while (cursor.remaining() >= static_cast<size_t>(width) &&
               result.node.children.size() < kMaxDynamicElements) {
            bool ok = false;
            const quint64 raw = readUnsignedAt(*cursor.data, cursor.offset, width,
                                               pod.endianness,
                                               context.defaultEndianness, &ok);
            bool comparable = false;
            const EvaluatedValue value =
                podKindIsSigned(pod.kind)
                    ? EvaluatedValue{integerValue(raw, pod.kind)}
                    : EvaluatedValue{raw};
            if (ok && compareValues(value, *attributes.untilExpression, context,
                                    &comparable)) {
                foundMatch = true;
                break;
            }
            if (!comparable) {
                invalidate(result,
                           QStringLiteral("Invalid numeric <until> comparison"));
                return result;
            }
            const QString elementName =
                QStringLiteral("%1[%2]").arg(name).arg(result.node.children.size());
            DecodeResult element =
                decodePodScalar(elementName, typeName, pod, {}, {}, cursor, context);
            result.node.children.push_back(element.node);
        }
        if (cursor.remaining() < static_cast<size_t>(width) && cursor.remaining() > 0) {
            const QString elementName =
                QStringLiteral("%1[%2]").arg(name).arg(result.node.children.size());
            DecodeResult partial =
                decodePodScalar(elementName, typeName, pod, {}, {}, cursor, context);
            result.node.children.push_back(partial.node);
        }
        if (!foundMatch &&
            result.node.children.size() >= kMaxDynamicElements) {
            invalidate(result, QStringLiteral("<until> exceeded the element limit"));
        } else if (!foundMatch && cursor.remaining() == 0) {
            invalidate(result, QStringLiteral("<until> condition was not found"));
        }
    } else {
        QString error;
        if (!evaluateCount(*attributes.lengthExpression, context,
                           &requestedCount, &error)) {
            invalidate(result, error);
            return result;
        }
        for (qint64 i = 0; i < requestedCount && cursor.remaining() > 0; ++i) {
            if (attributes.lengthMode == LengthMode::Maximum &&
                cursor.remaining() < static_cast<size_t>(width)) {
                break;
            }
            const QString elementName =
                QStringLiteral("%1[%2]").arg(name).arg(i);
            DecodeResult element =
                decodePodScalar(elementName, typeName, pod, {}, {}, cursor, context);
            result.node.children.push_back(element.node);
            if (element.node.bytesMissing > 0) {
                break;
            }
        }
        if (attributes.lengthMode == LengthMode::Fixed) {
            const size_t expected =
                static_cast<size_t>(requestedCount) * static_cast<size_t>(width);
            result.node.bytesMissing =
                static_cast<int>(expected - qMin(expected, cursor.offset - start));
        }
    }

    result.node.rawBytes = cursor.bytesFrom(start);
    result.node.valueText =
        QStringLiteral("%1 item(s)").arg(result.node.children.size());
    return result;
}

DecodeResult decodeType(const StructureGraph& graph, const ResolvedType& type,
                        const QString& name, const QString& typeName,
                        const FieldAttributes& attributes, ReadCursor& cursor,
                        DecodeContext& context);

DecodeResult decodeStruct(const StructureGraph& graph, const StructNode& structNode,
                          const QString& name, ReadCursor& cursor,
                          DecodeContext& context);

std::optional<int> staticFieldSize(const StructureGraph& graph,
                                   const ResolvedType& type,
                                   const FieldAttributes& attributes) {
    qint64 elementCount = 1;
    if (attributes.lengthMode != LengthMode::None) {
        if (attributes.lengthMode != LengthMode::Fixed ||
            !attributes.lengthExpression.has_value() ||
            attributes.lengthExpression->kind != IntExpressionKind::Integer) {
            return std::nullopt;
        }
        const IntExpression& expression = *attributes.lengthExpression;
        if (expression.integerIsUnsigned) {
            if (expression.unsignedIntegerValue >
                static_cast<quint64>(std::numeric_limits<int>::max())) {
                return std::nullopt;
            }
            elementCount = static_cast<qint64>(expression.unsignedIntegerValue);
        } else {
            if (expression.integerValue < 0) {
                return std::nullopt;
            }
            elementCount = expression.integerValue;
        }
    }

    qint64 size = 0;
    if (const auto* pod = std::get_if<PodType>(&type)) {
        size = elementCount * podKindWidthBytes(pod->kind);
    } else if (std::holds_alternative<ByteType>(type) ||
               std::holds_alternative<StringType>(type)) {
        if (attributes.lengthMode == LengthMode::None &&
            std::holds_alternative<StringType>(type)) {
            return std::nullopt;
        }
        size = elementCount;
    } else if (const auto* ref = std::get_if<StructRefType>(&type)) {
        if (attributes.lengthMode != LengthMode::None) {
            return std::nullopt;
        }
        const std::optional<int> structSize =
            graph.staticStructLayoutSizeBytes(ref->structName);
        if (!structSize.has_value()) {
            return std::nullopt;
        }
        size = *structSize;
    }
    if (size < 0 || size > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(size);
}

DecodeResult decodeMember(const StructureGraph& graph, const StructMember& member,
                          ReadCursor& cursor, DecodeContext& context) {
    if (!member.attributes.sourceRole.isEmpty()) {
        auto found = context.externalCursors.find(member.attributes.sourceRole);
        if (found == context.externalCursors.end()) {
            DecodeResult missing;
            missing.node.name = member.name;
            missing.node.typeName = member.typeDisplayName;
            missing.node.declarationRange = member.nameRange;
            missing.node.decoration = member.attributes.decoration;
            invalidate(missing, QStringLiteral("External source role '%1' is not bound")
                                    .arg(member.attributes.sourceRole));
            missing.invalidatesContainer = true;
            return missing;
        }
        StructMember externalMember = member;
        externalMember.attributes.sourceRole.clear();
        return decodeMember(graph, externalMember, found.value(), context);
    }
    DecodeResult result;
    setSourceOffset(result.node, cursor);
    result.node.declarationRange = member.nameRange;
    result.node.hasCondition =
        member.attributes.conditionExpression.has_value();
    const QString memberName =
        member.name.isEmpty() ? member.typeDisplayName : member.name;
    const bool isStructMember = std::holds_alternative<StructRefType>(member.type);

    if (member.attributes.whenExpression.has_value()) {
        bool comparable = false;
        QString error;
        const bool include = compareIntegerExpressions(
            *member.attributes.whenExpression, context, &comparable, &error);
        if (!comparable) {
            result.node.name = memberName;
            result.node.typeName = member.typeDisplayName;
            result.node.decoration = member.attributes.decoration;
            invalidate(result, error.isEmpty()
                                   ? QStringLiteral("Unable to evaluate /when expression")
                                   : error);
            result.invalidatesContainer = true;
            return result;
        }
        if (!include) {
            result.hasNode = false;
            return result;
        }
    }

    if (member.attributes.repeatExpression.has_value()) {
        result.node.valueKind = VisualizedValueKind::Array;
        qint64 repeatCount = 0;
        QString error;
        if (!evaluateCount(*member.attributes.repeatExpression, context,
                           &repeatCount, &error)) {
            result.node.name = memberName;
            result.node.typeName = member.typeDisplayName + QStringLiteral("[]");
            result.node.decoration = member.attributes.decoration;
            invalidate(result, error);
            return result;
        }
        result.node.name = memberName;
        result.node.typeName = member.typeDisplayName + QStringLiteral("[]");
        result.node.decoration = member.attributes.decoration;
        result.node.endianness = QStringLiteral("n/a");
        const size_t start = cursor.offset;
        FieldAttributes elementAttributes = member.attributes;
        elementAttributes.repeatExpression.reset();
        elementAttributes.variableName.clear();
        elementAttributes.conditionExpression.reset();
        qint64 decodedCount = 0;
        for (qint64 i = 0; i < repeatCount; ++i) {
            if (cursor.remaining() == 0) {
                break;
            }
            const QString itemName =
                QStringLiteral("%1[%2]").arg(memberName).arg(i);
            const size_t before = cursor.offset;
            DecodeResult item =
                decodeType(graph, member.type, itemName, member.typeDisplayName,
                           elementAttributes, cursor, context);
            item.node.declarationRange = member.nameRange;
            result.node.children.push_back(item.node);
            result.node.bytesMissing += item.node.bytesMissing;
            if (repeatCount == 1) {
                result.exports = item.exports;
            }
            if (!item.valid) {
                result.valid = false;
                result.node.valid = false;
                result.node.errorMessage = item.node.errorMessage;
                break;
            }
            if (item.node.bytesMissing > 0) {
                if (const std::optional<int> itemSize =
                        staticFieldSize(graph, member.type, elementAttributes);
                    itemSize.has_value()) {
                    const qint64 missingWholeItems =
                        (repeatCount - i - 1) * static_cast<qint64>(*itemSize);
                    result.node.bytesMissing += static_cast<int>(qMin(
                        missingWholeItems,
                        static_cast<qint64>(std::numeric_limits<int>::max() -
                                            result.node.bytesMissing)));
                }
                invalidate(result, QStringLiteral("Repeated field was truncated"));
                break;
            }
            ++decodedCount;
            if (cursor.offset == before) {
                invalidate(result,
                           QStringLiteral("Repeated field consumed no bytes"));
                break;
            }
        }
        if (result.valid && decodedCount < repeatCount) {
            if (const std::optional<int> itemSize =
                    staticFieldSize(graph, member.type, elementAttributes);
                itemSize.has_value()) {
                const qint64 missing =
                    (repeatCount - decodedCount) * static_cast<qint64>(*itemSize);
                result.node.bytesMissing +=
                    static_cast<int>(qMin(
                        missing,
                        static_cast<qint64>(std::numeric_limits<int>::max())));
            }
            invalidate(result,
                       QStringLiteral("Expected %1 repeated item(s), decoded %2")
                           .arg(repeatCount)
                           .arg(decodedCount));
        }
        result.node.rawBytes = cursor.bytesFrom(start);
        result.node.valueText =
            QStringLiteral("%1 item(s)").arg(result.node.children.size());
    } else {
        result = decodeType(graph, member.type, memberName,
                            member.typeDisplayName, member.attributes,
                            cursor, context);
    }

    result.node.hasCondition =
        member.attributes.conditionExpression.has_value();
    result.node.declarationRange = member.nameRange;
    if (result.node.valueKind == VisualizedValueKind::Array) {
        for (VisualizedNode& child : result.node.children) {
            child.declarationRange = member.nameRange;
        }
    }
    if (!member.attributes.variableName.isEmpty() && result.scalarValue.has_value()) {
        context.bind(member.attributes.variableName, *result.scalarValue);
    }
    if (member.attributes.conditionExpression.has_value()) {
        if (!isStructMember && result.node.bytesMissing > 0) {
            invalidate(result,
                       QStringLiteral("/cond field could not be decoded completely"));
            result.invalidatesContainer = true;
            return result;
        }
        const std::optional<EvaluatedValue> conditionValue =
            isStructMember ? std::optional<EvaluatedValue>{EvaluatedValue{result.valid}}
                           : result.scalarValue;
        if (!conditionValue.has_value()) {
            invalidate(result, QStringLiteral("/cond field could not be decoded"));
            result.invalidatesContainer = true;
            return result;
        }
        bool comparable = false;
        const bool matches =
            compareValues(*conditionValue,
                          *member.attributes.conditionExpression, context,
                          &comparable);
        if (!comparable) {
            invalidate(result, QStringLiteral("Unable to evaluate /cond expression"));
            result.invalidatesContainer = true;
        } else if (!matches) {
            invalidate(result, QStringLiteral("/cond expression failed"));
            result.invalidatesContainer = true;
        } else {
            result.invalidatesContainer = !result.valid && !isStructMember;
        }
    } else {
        result.invalidatesContainer = !result.valid && !isStructMember;
    }
    return result;
}

DecodeResult decodeStruct(const StructureGraph& graph, const StructNode& structNode,
                          const QString& name, ReadCursor& cursor,
                          DecodeContext& context) {
    DecodeResult result;
    setSourceOffset(result.node, cursor);
    result.node.name = name;
    result.node.typeName = structNode.name;
    result.node.declarationRange = structNode.nameRange;
    result.node.valueKind = VisualizedValueKind::Object;
    result.node.endianness = QStringLiteral("n/a");
    context.enterScope();
    for (const StructMember& member : structNode.members) {
        DecodeResult field = decodeMember(graph, member, cursor, context);
        if (!field.hasNode) {
            continue;
        }
        result.node.children.push_back(field.node);
        const QString memberName =
            member.name.isEmpty() ? member.typeDisplayName : member.name;
        for (auto it = field.exports.constBegin(); it != field.exports.constEnd();
             ++it) {
            context.bind(memberName + QLatin1Char('.') + it.key(), it.value());
        }
        result.node.bytesMissing += field.node.bytesMissing;
        if (field.invalidatesContainer) {
            result.valid = false;
            result.node.valid = false;
            result.node.errorMessage = field.node.errorMessage;
            break;
        }
    }
    int assertionIndex = 0;
    for (const ComparisonExpression& assertion : structNode.assertions) {
        VisualizedNode assertionNode;
        assertionNode.name = QStringLiteral("assert[%1]").arg(assertionIndex++);
        assertionNode.typeName = QStringLiteral("assert");
        assertionNode.valueText = assertion.sourceText;
        assertionNode.declarationRange = assertion.range;
        assertionNode.hasCondition = true;

        bool comparable = false;
        QString error;
        const bool matches =
            compareIntegerExpressions(assertion, context, &comparable, &error);
        if (!comparable || !matches) {
            assertionNode.valid = false;
            assertionNode.errorMessage =
                !comparable && !error.isEmpty()
                    ? error
                    : QStringLiteral("/assert expression failed");
            if (assertionNode.valueText.isEmpty()) {
                assertionNode.valueText =
                    QStringLiteral("invalid: %1").arg(assertionNode.errorMessage);
            } else {
                assertionNode.valueText +=
                    QStringLiteral(" [invalid: %1]").arg(assertionNode.errorMessage);
            }
            result.valid = false;
            result.node.valid = false;
            result.node.errorMessage = assertionNode.errorMessage;
        }
        result.node.children.push_back(assertionNode);
    }
    result.exports = context.leaveScope();
    result.node.valueText =
        result.valid ? QStringLiteral("struct")
                     : QStringLiteral("invalid: %1").arg(result.node.errorMessage);
    return result;
}

DecodeResult decodeType(const StructureGraph& graph, const ResolvedType& type,
                        const QString& name, const QString& typeName,
                        const FieldAttributes& attributes, ReadCursor& cursor,
                        DecodeContext& context) {
    if (const auto* pod = std::get_if<PodType>(&type)) {
        return decodePodField(name, typeName, *pod, attributes, cursor, context);
    }
    if (const auto* string = std::get_if<StringType>(&type)) {
        return decodeStringField(name, typeName, *string, attributes,
                                 cursor, context);
    }
    if (std::holds_alternative<ByteType>(type)) {
        return decodeByteField(name, attributes, cursor, context);
    }
    if (const auto* ref = std::get_if<StructRefType>(&type)) {
        const StructNode* nested = graph.findStruct(ref->structName);
        if (nested != nullptr) {
            return decodeStruct(graph, *nested, name, cursor, context);
        }
    }
    DecodeResult result;
    setSourceOffset(result.node, cursor);
    result.node.name = name;
    result.node.typeName = typeName;
    invalidate(result, QStringLiteral("Unable to resolve field type"));
    return result;
}

DecodeResult decodeEntry(const StructureGraph& graph, const QString& entryName,
                         const QString& chunkLabel, ReadCursor& cursor,
                         DecodeContext& context) {
    if (const StandaloneMemberNode* standalone =
            graph.findStandaloneMember(entryName)) {
        StructMember member;
        member.name = chunkLabel;
        member.type = standalone->type;
        member.typeDisplayName = standalone->typeDisplayName;
        member.attributes = standalone->attributes;
        member.nameRange = standalone->nameRange;
        return decodeMember(graph, member, cursor, context);
    }
    if (const StructNode* structNode = graph.findStruct(entryName)) {
        return decodeStruct(graph, *structNode, chunkLabel, cursor, context);
    }
    ResolvedType resolved;
    QString displayName;
    if (graph.resolveTypeName(entryName, &resolved, &displayName)) {
        DecodeResult result =
            decodeType(graph, resolved, chunkLabel, displayName,
                       FieldAttributes{}, cursor, context);
        result.node.declarationRange = graph.nameRangeForEntry(entryName);
        return result;
    }
    DecodeResult result;
    setSourceOffset(result.node, cursor);
    result.node.name = chunkLabel;
    invalidate(result, QStringLiteral("Unknown entry '%1'").arg(entryName));
    return result;
}

}  // namespace

VisualizedNode visualize(const StructureGraph& graph, const QString& entryName,
                         const QByteArray& dataBuffer, size_t dataStartOffset,
                         int entryCount, Endianness defaultEndianness) {
    VisualizationSource primary;
    primary.bytes = dataBuffer;
    return visualize(graph, entryName, primary, dataStartOffset, entryCount, {},
                     defaultEndianness);
}

VisualizedNode visualize(const StructureGraph& graph, const QString& entryName,
                         const VisualizationSource& primarySource,
                         size_t dataStartOffset, int entryCount,
                         const QHash<QString, VisualizationSource>& externalSources,
                         Endianness defaultEndianness) {
    VisualizedNode root;
    root.name = QStringLiteral("root");
    root.valueKind = VisualizedValueKind::Array;
    if (entryCount < 1) {
        entryCount = 1;
    }
    if (dataStartOffset > static_cast<size_t>(primarySource.bytes.size())) {
        return root;
    }

    ReadCursor cursor{&primarySource.bytes, dataStartOffset,
                      primarySource.baseOffset, primarySource.filePath};
    DecodeContext sharedContext;
    sharedContext.defaultEndianness = defaultEndianness;
    for (auto it = externalSources.constBegin(); it != externalSources.constEnd(); ++it) {
        sharedContext.externalCursors.insert(
            it.key(), ReadCursor{&it->bytes, 0, it->baseOffset, it->filePath});
    }
    for (int i = 0; i < entryCount && cursor.remaining() > 0; ++i) {
        DecodeContext& context = sharedContext;
        context.enterScope();
        const size_t before = cursor.offset;
        const QString chunkLabel =
            QStringLiteral("%1[%2]").arg(entryName).arg(i);
        DecodeResult decoded =
            decodeEntry(graph, entryName, chunkLabel, cursor, context);
        context.leaveScope();
        if (decoded.hasNode) {
            root.children.push_back(decoded.node);
        }
        if (!decoded.valid || cursor.offset == before) {
            break;
        }
    }
    updateSourceLengths(root);
    return root;
}

}  // namespace breco

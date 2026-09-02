#include "brecolang/render/OutformRenderer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>

namespace breco::lang {

namespace {

struct ValueRef {
    quint32 id = kInvalidId;
    bool temporary = false;

    bool valid() const { return id != kInvalidId; }
};

struct TempValue {
    DecodedValueKind kind = DecodedValueKind::Invalid;
    TypeId type = kInvalidId;
    quint64 unsignedValue = 0;
    qint64 signedValue = 0;
    double floatingValue = 0.0;
    bool booleanValue = false;
    QString stringValue;
    QByteArray ownedBytes;
    ByteSpanValue sourceBytes;
    QVector<ValueRef> elements;
    DecodedNodeId node = kInvalidId;
};

class Output {
public:
    Output(QIODevice* device, quint64 limit) : m_device(device), m_limit(limit) {
        if (m_device == nullptr || !m_device->isWritable()) {
            m_error = QStringLiteral("Outform output device is not writable");
        }
    }

    bool write(QByteArrayView bytes) {
        if (!m_error.isEmpty()) {
            return false;
        }
        if (static_cast<quint64>(bytes.size()) > m_limit -
                qMin(m_limit, m_written)) {
            m_error = QStringLiteral("Outform output exceeds max_transform_output");
            return false;
        }
        qsizetype complete = 0;
        while (complete < bytes.size()) {
            const qint64 amount =
                m_device->write(bytes.data() + complete, bytes.size() - complete);
            if (amount <= 0) {
                m_error = m_device->errorString().isEmpty()
                              ? QStringLiteral("Could not write outform output")
                              : m_device->errorString();
                return false;
            }
            complete += static_cast<qsizetype>(amount);
            m_written += static_cast<quint64>(amount);
        }
        return true;
    }

    bool writeSource(const RenderStore& store, const ByteSpanValue& span) {
        constexpr qsizetype kChunkBytes = 64 * 1024;
        ByteSource* source = store.input(span.input);
        if (source == nullptr) {
            m_error = QStringLiteral("Source input for emitted bytes is not bound");
            return false;
        }
        quint64 offset = store.logicalOffset(span);
        quint64 remaining = span.length;
        while (remaining > 0) {
            const qsizetype amount = static_cast<qsizetype>(
                qMin<quint64>(remaining, static_cast<quint64>(kChunkBytes)));
            const ByteReadResult read = source->read(offset, amount);
            if (!read.ok() || read.view.data() == nullptr) {
                m_error = read.error.isEmpty()
                              ? QStringLiteral("Could not read emitted source bytes")
                              : read.error;
                return false;
            }
            if (!write(QByteArrayView(read.view.data(), read.view.length))) {
                return false;
            }
            offset += static_cast<quint64>(amount);
            remaining -= static_cast<quint64>(amount);
        }
        return true;
    }

    QString error() const { return m_error; }
    quint64 written() const { return m_written; }

private:
    QIODevice* m_device = nullptr;
    quint64 m_limit = 0;
    quint64 m_written = 0;
    QString m_error;
};

class Renderer {
public:
    Renderer(const RenderStore& store, const OutformDesc& outform,
             QIODevice* output)
        : m_store(store), m_program(store.program()), m_tree(store.tree()),
          m_outform(outform), m_output(output,
                                      m_program.limits.maxTransformOutput) {
        m_slots.resize(outform.slotCount);
        if (!m_slots.isEmpty()) {
            m_slots[0] = {store.rootValue(), false};
        }
    }

    OutformRenderResult run() {
        if (!m_output.error().isEmpty()) {
            return {false, m_output.error(), 0};
        }
        if (m_store.rootValue() >=
            static_cast<DecodedValueId>(m_tree.values.size())) {
            return {false, QStringLiteral("Decoded result has no renderable root"),
                    0};
        }
        const DecodedValue& root = m_tree.values.at(m_store.rootValue());
        if (root.type != m_outform.targetType) {
            return {false,
                    QStringLiteral("Outform target type does not match the decoded entry"),
                    0};
        }
        if (!execBlock(m_outform.statements)) {
            return {false, m_error.isEmpty() ? m_output.error() : m_error,
                    m_output.written()};
        }
        return {true, {}, m_output.written()};
    }

private:
    const DecodedValue* baseValue(ValueRef ref) const {
        return !ref.temporary && ref.id <
                   static_cast<DecodedValueId>(m_tree.values.size())
                   ? &m_tree.values.at(ref.id)
                   : nullptr;
    }

    const TempValue* tempValue(ValueRef ref) const {
        return ref.temporary && ref.id < static_cast<quint32>(m_temporaries.size())
                   ? &m_temporaries.at(ref.id)
                   : nullptr;
    }

    DecodedValueKind kind(ValueRef ref) const {
        if (const DecodedValue* value = baseValue(ref)) {
            return value->kind;
        }
        if (const TempValue* value = tempValue(ref)) {
            return value->kind;
        }
        return DecodedValueKind::Invalid;
    }

    TypeId type(ValueRef ref) const {
        if (const DecodedValue* value = baseValue(ref)) {
            return value->type;
        }
        if (const TempValue* value = tempValue(ref)) {
            return value->type;
        }
        return kInvalidId;
    }

    DecodedNodeId node(ValueRef ref) const {
        if (const DecodedValue* value = baseValue(ref)) {
            return value->node;
        }
        if (const TempValue* value = tempValue(ref)) {
            return value->node;
        }
        return kInvalidId;
    }

    ValueRef addTemp(TempValue value) {
        const quint32 id = static_cast<quint32>(m_temporaries.size());
        m_temporaries.push_back(std::move(value));
        return {id, true};
    }

    ValueRef addUnsigned(TypeId type, quint64 value) {
        TempValue temp;
        temp.kind = DecodedValueKind::UnsignedInteger;
        temp.type = type;
        temp.unsignedValue = value;
        return addTemp(std::move(temp));
    }

    ValueRef addSigned(TypeId type, qint64 value) {
        TempValue temp;
        temp.kind = DecodedValueKind::SignedInteger;
        temp.type = type;
        temp.signedValue = value;
        return addTemp(std::move(temp));
    }

    ValueRef addFloat(TypeId type, double value) {
        TempValue temp;
        temp.kind = DecodedValueKind::FloatingPoint;
        temp.type = type;
        temp.floatingValue = value;
        return addTemp(std::move(temp));
    }

    ValueRef addBoolean(TypeId type, bool value) {
        TempValue temp;
        temp.kind = DecodedValueKind::Boolean;
        temp.type = type;
        temp.booleanValue = value;
        return addTemp(std::move(temp));
    }

    ValueRef addString(TypeId type, QString value) {
        TempValue temp;
        temp.kind = DecodedValueKind::String;
        temp.type = type;
        temp.stringValue = std::move(value);
        return addTemp(std::move(temp));
    }

    ValueRef addBytes(TypeId type, QByteArray value) {
        TempValue temp;
        temp.kind = DecodedValueKind::OwnedBytes;
        temp.type = type;
        temp.ownedBytes = std::move(value);
        return addTemp(std::move(temp));
    }

    ValueRef addSourceBytes(TypeId type, ByteSpanValue span) {
        TempValue temp;
        temp.kind = DecodedValueKind::SourceBytes;
        temp.type = type;
        temp.sourceBytes = span;
        return addTemp(std::move(temp));
    }

    ValueRef addSequence(TypeId type, QVector<ValueRef> values) {
        TempValue temp;
        temp.kind = DecodedValueKind::Sequence;
        temp.type = type;
        temp.elements = std::move(values);
        return addTemp(std::move(temp));
    }

    std::optional<quint64> asUnsigned(ValueRef ref) const {
        if (const DecodedValue* value = baseValue(ref)) {
            switch (value->kind) {
                case DecodedValueKind::UnsignedInteger: return value->unsignedValue;
                case DecodedValueKind::SignedInteger:
                    return static_cast<quint64>(value->signedValue);
                case DecodedValueKind::Boolean:
                    return value->booleanValue ? 1 : 0;
                case DecodedValueKind::FloatingPoint:
                    if (std::isfinite(value->floatingValue) &&
                        value->floatingValue >= 0.0) {
                        return static_cast<quint64>(value->floatingValue);
                    }
                    return std::nullopt;
                default: return std::nullopt;
            }
        }
        if (const TempValue* value = tempValue(ref)) {
            switch (value->kind) {
                case DecodedValueKind::UnsignedInteger: return value->unsignedValue;
                case DecodedValueKind::SignedInteger:
                    return static_cast<quint64>(value->signedValue);
                case DecodedValueKind::Boolean:
                    return value->booleanValue ? 1 : 0;
                case DecodedValueKind::FloatingPoint:
                    if (std::isfinite(value->floatingValue) &&
                        value->floatingValue >= 0.0) {
                        return static_cast<quint64>(value->floatingValue);
                    }
                    return std::nullopt;
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<qint64> asSigned(ValueRef ref) const {
        if (const DecodedValue* value = baseValue(ref)) {
            if (value->kind == DecodedValueKind::SignedInteger)
                return value->signedValue;
        }
        if (const TempValue* value = tempValue(ref)) {
            if (value->kind == DecodedValueKind::SignedInteger)
                return value->signedValue;
        }
        const auto value = asUnsigned(ref);
        return value.has_value() ? std::optional<qint64>(static_cast<qint64>(*value))
                                 : std::nullopt;
    }

    std::optional<double> asDouble(ValueRef ref) const {
        if (const DecodedValue* value = baseValue(ref)) {
            if (value->kind == DecodedValueKind::FloatingPoint)
                return value->floatingValue;
            if (value->kind == DecodedValueKind::SignedInteger)
                return static_cast<double>(value->signedValue);
            if (value->kind == DecodedValueKind::UnsignedInteger)
                return static_cast<double>(value->unsignedValue);
            if (value->kind == DecodedValueKind::Boolean)
                return value->booleanValue ? 1.0 : 0.0;
        }
        if (const TempValue* value = tempValue(ref)) {
            if (value->kind == DecodedValueKind::FloatingPoint)
                return value->floatingValue;
            if (value->kind == DecodedValueKind::SignedInteger)
                return static_cast<double>(value->signedValue);
            if (value->kind == DecodedValueKind::UnsignedInteger)
                return static_cast<double>(value->unsignedValue);
            if (value->kind == DecodedValueKind::Boolean)
                return value->booleanValue ? 1.0 : 0.0;
        }
        return std::nullopt;
    }

    bool truth(ValueRef ref) const {
        if (!ref.valid() || kind(ref) == DecodedValueKind::Null) {
            return false;
        }
        if (const DecodedValue* value = baseValue(ref)) {
            if (value->kind == DecodedValueKind::Boolean)
                return value->booleanValue;
        }
        if (const TempValue* value = tempValue(ref)) {
            if (value->kind == DecodedValueKind::Boolean)
                return value->booleanValue;
        }
        return asDouble(ref).value_or(0.0) != 0.0;
    }

    QString scalarString(ValueRef ref) const {
        if (const DecodedValue* value = baseValue(ref)) {
            switch (value->kind) {
                case DecodedValueKind::String:
                    return m_tree.valueStrings.value(value->payload);
                case DecodedValueKind::Boolean:
                    return value->booleanValue ? QStringLiteral("true")
                                               : QStringLiteral("false");
                case DecodedValueKind::UnsignedInteger:
                    return QString::number(value->unsignedValue);
                case DecodedValueKind::SignedInteger:
                    return QString::number(value->signedValue);
                case DecodedValueKind::FloatingPoint:
                    return QString::number(value->floatingValue, 'g', 15);
                case DecodedValueKind::Null: return QString();
                default: return QString();
            }
        }
        if (const TempValue* value = tempValue(ref)) {
            switch (value->kind) {
                case DecodedValueKind::String: return value->stringValue;
                case DecodedValueKind::Boolean:
                    return value->booleanValue ? QStringLiteral("true")
                                               : QStringLiteral("false");
                case DecodedValueKind::UnsignedInteger:
                    return QString::number(value->unsignedValue);
                case DecodedValueKind::SignedInteger:
                    return QString::number(value->signedValue);
                case DecodedValueKind::FloatingPoint:
                    return QString::number(value->floatingValue, 'g', 15);
                case DecodedValueKind::Null: return QString();
                default: return QString();
            }
        }
        return {};
    }

    std::optional<ByteSpanValue> sourceSpan(ValueRef ref) const {
        if (const DecodedValue* value = baseValue(ref)) {
            if (value->kind == DecodedValueKind::SourceBytes &&
                value->payload < static_cast<quint32>(m_tree.spans.size())) {
                return m_tree.spans.at(value->payload);
            }
        }
        if (const TempValue* value = tempValue(ref)) {
            if (value->kind == DecodedValueKind::SourceBytes) {
                return value->sourceBytes;
            }
        }
        return std::nullopt;
    }

    std::optional<QByteArray> bytes(ValueRef ref) {
        if (const DecodedValue* value = baseValue(ref)) {
            if (value->kind == DecodedValueKind::OwnedBytes) {
                return m_tree.ownedBytes.value(value->payload);
            }
        }
        if (const TempValue* value = tempValue(ref)) {
            if (value->kind == DecodedValueKind::OwnedBytes) {
                return value->ownedBytes;
            }
        }
        const auto span = sourceSpan(ref);
        if (!span.has_value()) {
            return std::nullopt;
        }
        if (span->length > m_program.limits.maxTransformOutput ||
            span->length > static_cast<quint64>(
                               std::numeric_limits<qsizetype>::max())) {
            m_error = QStringLiteral("Byte transform exceeds max_transform_output");
            return std::nullopt;
        }
        ByteSource* source = m_store.input(span->input);
        if (source == nullptr) {
            m_error = QStringLiteral("Source input for byte transform is not bound");
            return std::nullopt;
        }
        const ByteReadResult read = source->read(
            m_store.logicalOffset(*span), static_cast<qsizetype>(span->length));
        if (!read.ok() || read.view.data() == nullptr) {
            m_error = read.error.isEmpty()
                          ? QStringLiteral("Could not read source bytes")
                          : read.error;
            return std::nullopt;
        }
        return QByteArray(read.view.data(), read.view.length);
    }

    QVector<ValueRef> elements(ValueRef ref) const {
        QVector<ValueRef> result;
        if (const DecodedValue* value = baseValue(ref)) {
            if ((value->kind != DecodedValueKind::Sequence &&
                 value->kind != DecodedValueKind::Aggregate) ||
                value->elements.first >
                    static_cast<quint32>(m_tree.valueRefs.size()) ||
                value->elements.count >
                    static_cast<quint32>(m_tree.valueRefs.size()) -
                        value->elements.first) {
                return result;
            }
            result.reserve(value->elements.count);
            for (quint32 i = 0; i < value->elements.count; ++i) {
                result.push_back(
                    {m_tree.valueRefs.at(value->elements.first + i), false});
            }
            return result;
        }
        if (const TempValue* value = tempValue(ref)) {
            if (value->kind == DecodedValueKind::Sequence) {
                return value->elements;
            }
        }
        return result;
    }

    ValueRef eval(ExpressionId id) {
        if (id >= static_cast<ExpressionId>(m_program.expressions.size())) {
            m_error = QStringLiteral("Invalid outform expression");
            return {};
        }
        const Expression& expression = m_program.expressions.at(id);
        switch (expression.kind) {
            case ExpressionKind::UnsignedInteger:
            case ExpressionKind::Constant:
            case ExpressionKind::EnumValue:
                return addUnsigned(expression.type, expression.unsignedValue);
            case ExpressionKind::FloatingPoint:
                return addFloat(expression.type, expression.floatingValue);
            case ExpressionKind::String:
                return addString(expression.type,
                                 m_program.symbol(expression.symbol));
            case ExpressionKind::Boolean:
                return addBoolean(expression.type, expression.booleanValue);
            case ExpressionKind::Slot:
                if (expression.slot < static_cast<quint32>(m_slots.size()) &&
                    m_slots.at(expression.slot).valid()) {
                    return m_slots.at(expression.slot);
                }
                m_error = QStringLiteral("Outform value '%1' is unavailable")
                              .arg(m_program.symbol(expression.symbol));
                return {};
            case ExpressionKind::Member: return evalMember(expression);
            case ExpressionKind::MetadataMember: return evalMetadata(expression);
            case ExpressionKind::Unary: return evalUnary(expression);
            case ExpressionKind::Binary: return evalBinary(expression);
            case ExpressionKind::Call: return evalCall(expression);
            case ExpressionKind::ByteArray: return evalByteArray(expression);
            case ExpressionKind::InterpolatedString:
                return evalInterpolated(expression);
            case ExpressionKind::TypeName:
            case ExpressionKind::Invalid:
                m_error = QStringLiteral("Expression cannot be evaluated in an outform");
                return {};
        }
        return {};
    }

    ValueRef evalMember(const Expression& expression) {
        const ValueRef base = eval(
            m_program.expressionRefs.at(expression.operands.first));
        if (!base.valid()) {
            return {};
        }
        const DecodedValue* object = baseValue(base);
        if (object == nullptr || object->kind != DecodedValueKind::Object) {
            m_error = QStringLiteral("Outform member base is not a record");
            return {};
        }
        const DecodedFieldValue* field =
            m_tree.findField(base.id, expression.symbol);
        if (field == nullptr) {
            m_error = QStringLiteral("Decoded field '%1' is absent")
                          .arg(m_program.symbol(expression.symbol));
            return {};
        }
        return {field->value, false};
    }

    ValueRef evalMetadata(const Expression& expression) {
        const ValueRef base = eval(
            m_program.expressionRefs.at(expression.operands.first));
        if (!base.valid()) {
            return {};
        }
        const QString member = m_program.symbol(expression.symbol);
        const DecodedNodeId nodeId = node(base);
        const DecodedNode* decodedNode =
            nodeId < static_cast<DecodedNodeId>(m_tree.nodes.size())
                ? &m_tree.nodes.at(nodeId)
                : nullptr;
        const std::optional<ByteSpanValue> directSpan = sourceSpan(base);
        if (member == QStringLiteral("value")) {
            return base;
        }
        if (member == QStringLiteral("valid")) {
            return addBoolean(expression.type,
                              decodedNode == nullptr || decodedNode->valid);
        }
        if (member == QStringLiteral("offset") ||
            member == QStringLiteral("length")) {
            const quint64 value =
                decodedNode != nullptr && decodedNode->hasSourceSpan
                    ? (member == QStringLiteral("offset") ? decodedNode->offset
                                                          : decodedNode->length)
                    : (directSpan.has_value()
                           ? (member == QStringLiteral("offset")
                                  ? directSpan->offset
                                  : directSpan->length)
                           : 0);
            return addUnsigned(expression.type, value);
        }
        if (member == QStringLiteral("name")) {
            return addString(expression.type,
                             decodedNode != nullptr
                                 ? m_tree.name(decodedNode->name)
                                 : QString());
        }
        if (member == QStringLiteral("type")) {
            const TypeId valueType = decodedNode != nullptr
                                         ? decodedNode->type
                                         : type(base);
            QString value;
            if (valueType < static_cast<TypeId>(m_program.types.size())) {
                const TypeDesc& descriptor = m_program.types.at(valueType);
                value = descriptor.name != kInvalidId
                            ? m_program.symbol(descriptor.name)
                            : QStringLiteral("type#%1").arg(valueType);
            }
            return addString(expression.type, value);
        }
        if (member == QStringLiteral("input")) {
            return addString(expression.type,
                             decodedNode != nullptr
                                 ? m_store.inputRole(decodedNode->input)
                                 : (directSpan.has_value()
                                        ? m_store.inputRole(directSpan->input)
                                        : QString()));
        }
        if (member == QStringLiteral("path")) {
            return addString(expression.type,
                             decodedNode != nullptr
                                 ? m_store.inputPath(decodedNode->input)
                                 : (directSpan.has_value()
                                        ? m_store.inputPath(directSpan->input)
                                        : QString()));
        }
        if (member == QStringLiteral("error")) {
            return addString(
                expression.type,
                decodedNode != nullptr && decodedNode->error != kInvalidId
                    ? m_tree.name(decodedNode->error)
                    : QString());
        }
        if (member == QStringLiteral("bytes")) {
            if (decodedNode != nullptr && decodedNode->hasSourceSpan) {
                return addSourceBytes(
                    expression.type,
                    {decodedNode->input, decodedNode->offset,
                     decodedNode->length});
            }
            if (kind(base) == DecodedValueKind::SourceBytes ||
                kind(base) == DecodedValueKind::OwnedBytes) {
                return base;
            }
            m_error = QStringLiteral("Metadata '@bytes' is unavailable");
            return {};
        }
        if (member == QStringLiteral("children")) {
            QVector<ValueRef> children;
            if (decodedNode != nullptr) {
                DecodedNodeId child = decodedNode->firstChild;
                while (child != kInvalidId &&
                       child < static_cast<DecodedNodeId>(m_tree.nodes.size())) {
                    const DecodedNode& nodeValue = m_tree.nodes.at(child);
                    if (nodeValue.value != kInvalidId) {
                        children.push_back({nodeValue.value, false});
                    }
                    child = nodeValue.nextSibling;
                }
            }
            return addSequence(expression.type, std::move(children));
        }
        if (member == QStringLiteral("spans")) {
            QVector<ValueRef> spans;
            TypeId spanType = kInvalidId;
            if (expression.type < static_cast<TypeId>(m_program.types.size()) &&
                m_program.types.at(expression.type).kind == TypeKind::Sequence) {
                spanType = m_program.types.at(expression.type).elementType;
            }
            if (decodedNode != nullptr &&
                decodedNode->storageLayout <
                    static_cast<quint32>(m_tree.storageLayouts.size())) {
                const StorageLayout& layout =
                    m_tree.storageLayouts.at(decodedNode->storageLayout);
                for (quint32 i = 0; i < layout.spans.count; ++i) {
                    spans.push_back(addSourceBytes(
                        spanType,
                        m_tree.spans.at(layout.spans.first + i)));
                }
            }
            return addSequence(expression.type, std::move(spans));
        }
        m_error = QStringLiteral("Metadata '@%1' is unavailable").arg(member);
        return {};
    }

    ValueRef evalUnary(const Expression& expression) {
        const ValueRef operand = eval(
            m_program.expressionRefs.at(expression.operands.first));
        if (!operand.valid()) return {};
        if (expression.unaryOperator == SyntaxUnaryOperator::LogicalNot) {
            return addBoolean(expression.type, !truth(operand));
        }
        if (kind(operand) == DecodedValueKind::FloatingPoint) {
            return addFloat(expression.type, -asDouble(operand).value_or(0.0));
        }
        return addSigned(expression.type, -asSigned(operand).value_or(0));
    }

    bool equal(ValueRef left, ValueRef right) {
        const DecodedValueKind leftKind = kind(left);
        const DecodedValueKind rightKind = kind(right);
        const bool leftNumeric = leftKind == DecodedValueKind::UnsignedInteger ||
                                 leftKind == DecodedValueKind::SignedInteger ||
                                 leftKind == DecodedValueKind::FloatingPoint ||
                                 leftKind == DecodedValueKind::Boolean;
        const bool rightNumeric = rightKind == DecodedValueKind::UnsignedInteger ||
                                  rightKind == DecodedValueKind::SignedInteger ||
                                  rightKind == DecodedValueKind::FloatingPoint ||
                                  rightKind == DecodedValueKind::Boolean;
        if (leftNumeric && rightNumeric) {
            return asDouble(left).value_or(0.0) ==
                   asDouble(right).value_or(0.0);
        }
        if (leftKind == DecodedValueKind::String &&
            rightKind == DecodedValueKind::String) {
            return scalarString(left) == scalarString(right);
        }
        if (leftKind == DecodedValueKind::Null ||
            rightKind == DecodedValueKind::Null) {
            return leftKind == rightKind;
        }
        return !left.temporary && !right.temporary && left.id == right.id;
    }

    ValueRef evalBinary(const Expression& expression) {
        const ValueRef left = eval(
            m_program.expressionRefs.at(expression.operands.first));
        if (!left.valid()) return {};
        if (expression.binaryOperator == SyntaxBinaryOperator::LogicalAnd &&
            !truth(left)) {
            return addBoolean(expression.type, false);
        }
        if (expression.binaryOperator == SyntaxBinaryOperator::LogicalOr &&
            truth(left)) {
            return addBoolean(expression.type, true);
        }
        const ValueRef right = eval(
            m_program.expressionRefs.at(expression.operands.first + 1));
        if (!right.valid()) return {};
        switch (expression.binaryOperator) {
            case SyntaxBinaryOperator::Equal:
                return addBoolean(expression.type, equal(left, right));
            case SyntaxBinaryOperator::NotEqual:
                return addBoolean(expression.type, !equal(left, right));
            case SyntaxBinaryOperator::LogicalAnd:
                return addBoolean(expression.type, truth(left) && truth(right));
            case SyntaxBinaryOperator::LogicalOr:
                return addBoolean(expression.type, truth(left) || truth(right));
            case SyntaxBinaryOperator::Less:
                return addBoolean(expression.type,
                                  asDouble(left).value_or(0.0) <
                                      asDouble(right).value_or(0.0));
            case SyntaxBinaryOperator::LessEqual:
                return addBoolean(expression.type,
                                  asDouble(left).value_or(0.0) <=
                                      asDouble(right).value_or(0.0));
            case SyntaxBinaryOperator::Greater:
                return addBoolean(expression.type,
                                  asDouble(left).value_or(0.0) >
                                      asDouble(right).value_or(0.0));
            case SyntaxBinaryOperator::GreaterEqual:
                return addBoolean(expression.type,
                                  asDouble(left).value_or(0.0) >=
                                      asDouble(right).value_or(0.0));
            default: break;
        }
        const bool floating =
            kind(left) == DecodedValueKind::FloatingPoint ||
            kind(right) == DecodedValueKind::FloatingPoint;
        if (floating) {
            const double a = asDouble(left).value_or(0.0);
            const double b = asDouble(right).value_or(0.0);
            if ((expression.binaryOperator == SyntaxBinaryOperator::Divide ||
                 expression.binaryOperator == SyntaxBinaryOperator::Remainder) &&
                b == 0.0) {
                m_error = QStringLiteral("Division by zero in outform expression");
                return {};
            }
            if (expression.binaryOperator == SyntaxBinaryOperator::Add)
                return addFloat(expression.type, a + b);
            if (expression.binaryOperator == SyntaxBinaryOperator::Subtract)
                return addFloat(expression.type, a - b);
            if (expression.binaryOperator == SyntaxBinaryOperator::Multiply)
                return addFloat(expression.type, a * b);
            if (expression.binaryOperator == SyntaxBinaryOperator::Divide)
                return addFloat(expression.type, a / b);
            return addFloat(expression.type, std::fmod(a, b));
        }
        const quint64 a = asUnsigned(left).value_or(0);
        const quint64 b = asUnsigned(right).value_or(0);
        if ((expression.binaryOperator == SyntaxBinaryOperator::Divide ||
             expression.binaryOperator == SyntaxBinaryOperator::Remainder) &&
            b == 0) {
            m_error = QStringLiteral("Division by zero in outform expression");
            return {};
        }
        if (expression.binaryOperator == SyntaxBinaryOperator::Add)
            return addUnsigned(expression.type, a + b);
        if (expression.binaryOperator == SyntaxBinaryOperator::Subtract)
            return addUnsigned(expression.type, a - b);
        if (expression.binaryOperator == SyntaxBinaryOperator::Multiply)
            return addUnsigned(expression.type, a * b);
        if (expression.binaryOperator == SyntaxBinaryOperator::Divide)
            return addUnsigned(expression.type, a / b);
        return addUnsigned(expression.type, a % b);
    }

    QString enumName(ValueRef ref) const {
        const TypeId valueType = type(ref);
        const auto numeric = asUnsigned(ref);
        if (!numeric.has_value()) return scalarString(ref);
        for (const EnumDesc& descriptor : m_program.enums) {
            if (descriptor.type != valueType) continue;
            for (quint32 i = 0; i < descriptor.values.count; ++i) {
                const EnumValueDesc& value =
                    m_program.enumValues.at(descriptor.values.first + i);
                if (value.value == *numeric) {
                    return m_program.symbol(value.name);
                }
            }
        }
        return QString::number(*numeric);
    }

    QByteArray integerBytes(quint64 value, int width, bool little) const {
        QByteArray result(width, Qt::Uninitialized);
        for (int i = 0; i < width; ++i) {
            const int shift = little ? i * 8 : (width - i - 1) * 8;
            result[i] = static_cast<char>((value >> shift) & 0xff);
        }
        return result;
    }

    quint64 storedUnsigned(ValueRef ref) const {
        if (const DecodedValue* value = baseValue(ref)) {
            return value->kind == DecodedValueKind::Boolean
                       ? (value->booleanValue ? 1 : 0)
                       : value->unsignedValue;
        }
        if (const TempValue* value = tempValue(ref)) {
            return value->kind == DecodedValueKind::Boolean
                       ? (value->booleanValue ? 1 : 0)
                       : value->unsignedValue;
        }
        return 0;
    }

    qint64 storedSigned(ValueRef ref) const {
        if (const DecodedValue* value = baseValue(ref)) {
            return value->signedValue;
        }
        if (const TempValue* value = tempValue(ref)) {
            return value->signedValue;
        }
        return 0;
    }

    double storedFloat(ValueRef ref) const {
        if (const DecodedValue* value = baseValue(ref)) {
            return value->floatingValue;
        }
        if (const TempValue* value = tempValue(ref)) {
            return value->floatingValue;
        }
        return 0.0;
    }

    QString encoderValueText(ValueRef ref) const {
        if (kind(ref) == DecodedValueKind::FloatingPoint) {
            return QString::number(storedFloat(ref), 'g', 17);
        }
        return scalarString(ref);
    }

    void setEncoderRangeError(const Expression& expression,
                              QStringView function, ValueRef argument,
                              const QString& expected) {
        const SourceSpan span =
            expression.sourceSpan <
                    static_cast<SourceSpanId>(m_program.sourceSpans.size())
                ? m_program.sourceSpans.at(expression.sourceSpan)
                : SourceSpan{};
        m_error = QStringLiteral(
                      "Encoder '%1' expression at source offset %2 cannot encode "
                      "value %3; expected %4")
                      .arg(function.toString())
                      .arg(span.start)
                      .arg(encoderValueText(argument), expected);
    }

    bool checkedIntegerEncoder(const Expression& expression,
                               QStringView function, ValueRef argument,
                               int width, quint64* encoded) {
        const int bits = width * 8;
        const bool signedTarget = function.startsWith(u"i");
        const DecodedValueKind argumentKind = kind(argument);
        if (signedTarget) {
            const qint64 minimum =
                bits == 64 ? std::numeric_limits<qint64>::min()
                           : -(qint64{1} << (bits - 1));
            const qint64 maximum =
                bits == 64 ? std::numeric_limits<qint64>::max()
                           : (qint64{1} << (bits - 1)) - 1;
            const QString expected =
                QStringLiteral("an integer in %1..%2").arg(minimum).arg(maximum);
            qint64 value = 0;
            if (argumentKind == DecodedValueKind::SignedInteger) {
                value = storedSigned(argument);
                if (value < minimum || value > maximum) {
                    setEncoderRangeError(expression, function, argument, expected);
                    return false;
                }
            } else if (argumentKind == DecodedValueKind::UnsignedInteger ||
                       argumentKind == DecodedValueKind::Boolean) {
                const quint64 unsignedValue = storedUnsigned(argument);
                if (unsignedValue > static_cast<quint64>(maximum)) {
                    setEncoderRangeError(expression, function, argument, expected);
                    return false;
                }
                value = static_cast<qint64>(unsignedValue);
            } else if (argumentKind == DecodedValueKind::FloatingPoint) {
                const double floating = storedFloat(argument);
                const double lower = -std::ldexp(1.0, bits - 1);
                const double upper = std::ldexp(1.0, bits - 1);
                if (!std::isfinite(floating) || std::trunc(floating) != floating ||
                    floating < lower || floating >= upper) {
                    setEncoderRangeError(expression, function, argument, expected);
                    return false;
                }
                value = static_cast<qint64>(floating);
            } else {
                setEncoderRangeError(expression, function, argument, expected);
                return false;
            }
            *encoded = static_cast<quint64>(value);
            return true;
        }

        const quint64 maximum =
            bits == 64 ? std::numeric_limits<quint64>::max()
                       : (quint64{1} << bits) - 1;
        const QString expected =
            QStringLiteral("an integer in 0..%1").arg(maximum);
        quint64 value = 0;
        if (argumentKind == DecodedValueKind::SignedInteger) {
            const qint64 signedValue = storedSigned(argument);
            if (signedValue < 0 || static_cast<quint64>(signedValue) > maximum) {
                setEncoderRangeError(expression, function, argument, expected);
                return false;
            }
            value = static_cast<quint64>(signedValue);
        } else if (argumentKind == DecodedValueKind::UnsignedInteger ||
                   argumentKind == DecodedValueKind::Boolean) {
            value = storedUnsigned(argument);
            if (value > maximum) {
                setEncoderRangeError(expression, function, argument, expected);
                return false;
            }
        } else if (argumentKind == DecodedValueKind::FloatingPoint) {
            const double floating = storedFloat(argument);
            const double upper = std::ldexp(1.0, bits);
            if (!std::isfinite(floating) || std::trunc(floating) != floating ||
                floating < 0.0 || floating >= upper) {
                setEncoderRangeError(expression, function, argument, expected);
                return false;
            }
            value = static_cast<quint64>(floating);
        } else {
            setEncoderRangeError(expression, function, argument, expected);
            return false;
        }
        *encoded = value;
        return true;
    }

    ValueRef evalEncoder(const Expression& expression, QStringView function,
                         ValueRef argument) {
        if (function == u"utf8") {
            return addBytes(expression.type, scalarString(argument).toUtf8());
        }
        if (function == u"utf16le" || function == u"utf16be") {
            const QString text = scalarString(argument);
            QByteArray result;
            result.reserve(text.size() * 2);
            const bool little = function == u"utf16le";
            for (QChar character : text) {
                result += integerBytes(character.unicode(), 2, little);
            }
            return addBytes(expression.type, std::move(result));
        }
        const bool little = function.endsWith(u"le") ||
                            function == u"u8" || function == u"i8";
        int width = 0;
        if (function == u"u8" || function == u"i8") width = 1;
        if (function.contains(u"16")) width = 2;
        if (function.contains(u"32")) width = 4;
        if (function.contains(u"64")) width = 8;
        if (function.startsWith(u"f")) {
            const double numeric = asDouble(argument).value_or(0.0);
            if (width == 4) {
                const quint32 bits = std::bit_cast<quint32>(
                    static_cast<float>(numeric));
                return addBytes(expression.type,
                                integerBytes(bits, width, little));
            }
            const quint64 bits = std::bit_cast<quint64>(numeric);
            return addBytes(expression.type, integerBytes(bits, width, little));
        }
        quint64 numeric = 0;
        if (!checkedIntegerEncoder(expression, function, argument, width,
                                   &numeric)) {
            return {};
        }
        return addBytes(expression.type, integerBytes(numeric, width, little));
    }

    ValueRef evalCall(const Expression& expression) {
        if (expression.operands.count == 0) {
            m_error = QStringLiteral("Outform function is missing its argument");
            return {};
        }
        const ValueRef argument = eval(
            m_program.expressionRefs.at(expression.operands.first));
        if (!argument.valid()) return {};
        const QString function = m_program.symbol(expression.symbol);
        if (function == QStringLiteral("present")) {
            return addBoolean(expression.type,
                              kind(argument) != DecodedValueKind::Null &&
                                  kind(argument) != DecodedValueKind::Invalid);
        }
        if (function == QStringLiteral("count")) {
            return addUnsigned(expression.type,
                               static_cast<quint64>(elements(argument).size()));
        }
        if (function == QStringLiteral("int")) {
            return addUnsigned(expression.type,
                               asUnsigned(argument).value_or(0));
        }
        if (function == QStringLiteral("str") ||
            function == QStringLiteral("dec")) {
            return addString(expression.type, scalarString(argument));
        }
        if (function == QStringLiteral("enum_name")) {
            return addString(expression.type, enumName(argument));
        }
        if (function == QStringLiteral("hex")) {
            return addString(expression.type,
                             QStringLiteral("0x%1").arg(
                                 asUnsigned(argument).value_or(0), 0, 16));
        }
        if (function == QStringLiteral("hex_bytes")) {
            const auto value = bytes(argument);
            if (!value.has_value()) {
                if (m_error.isEmpty())
                    m_error = QStringLiteral("hex_bytes requires bytes");
                return {};
            }
            return addString(expression.type,
                             QString::fromLatin1(value->toHex()));
        }
        if (function == QStringLiteral("upper") ||
            function == QStringLiteral("lower")) {
            const QString value = scalarString(argument);
            return addString(expression.type,
                             function == QStringLiteral("upper")
                                 ? value.toUpper()
                                 : value.toLower());
        }
        if (function == QStringLiteral("csv")) {
            QString value = scalarString(argument);
            if (value.contains(QLatin1Char(',')) ||
                value.contains(QLatin1Char('"')) ||
                value.contains(QLatin1Char('\n')) ||
                value.contains(QLatin1Char('\r'))) {
                value.replace(QStringLiteral("\""), QStringLiteral("\"\""));
                value = QLatin1Char('"') + value + QLatin1Char('"');
            }
            return addString(expression.type, std::move(value));
        }
        if (function == QStringLiteral("json")) {
            QString value;
            if (kind(argument) == DecodedValueKind::String) {
                QJsonArray wrapper;
                wrapper.push_back(scalarString(argument));
                QByteArray encoded =
                    QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
                encoded.remove(0, 1);
                encoded.chop(1);
                value = QString::fromUtf8(encoded);
            } else if (kind(argument) == DecodedValueKind::Null) {
                value = QStringLiteral("null");
            } else {
                value = scalarString(argument);
            }
            return addString(expression.type, std::move(value));
        }
        static const QStringList encoders{
            QStringLiteral("u8"),      QStringLiteral("i8"),
            QStringLiteral("u16le"),   QStringLiteral("u16be"),
            QStringLiteral("i16le"),   QStringLiteral("i16be"),
            QStringLiteral("u32le"),   QStringLiteral("u32be"),
            QStringLiteral("i32le"),   QStringLiteral("i32be"),
            QStringLiteral("u64le"),   QStringLiteral("u64be"),
            QStringLiteral("i64le"),   QStringLiteral("i64be"),
            QStringLiteral("f32le"),   QStringLiteral("f32be"),
            QStringLiteral("f64le"),   QStringLiteral("f64be"),
            QStringLiteral("utf8"),    QStringLiteral("utf16le"),
            QStringLiteral("utf16be")};
        if (encoders.contains(function)) {
            return evalEncoder(expression, function, argument);
        }
        m_error = QStringLiteral("Unknown outform function '%1'").arg(function);
        return {};
    }

    ValueRef evalByteArray(const Expression& expression) {
        QByteArray bytes;
        bytes.reserve(expression.operands.count);
        for (quint32 i = 0; i < expression.operands.count; ++i) {
            const ValueRef element = eval(
                m_program.expressionRefs.at(expression.operands.first + i));
            if (!element.valid()) return {};
            bytes.push_back(static_cast<char>(
                asUnsigned(element).value_or(0) & 0xff));
        }
        return addBytes(expression.type, std::move(bytes));
    }

    ValueRef evalInterpolated(const Expression& expression) {
        QString text;
        for (quint32 i = 0; i < expression.textParts.count; ++i) {
            text += m_program.symbol(
                m_program.textPartSymbols.at(expression.textParts.first + i));
            if (i < expression.operands.count) {
                const ValueRef value = eval(
                    m_program.expressionRefs.at(expression.operands.first + i));
                if (!value.valid()) return {};
                text += scalarString(value);
            }
        }
        return addString(expression.type, std::move(text));
    }

    bool emitValue(ValueRef value) {
        if (m_outform.mode == OutformMode::Text) {
            return m_output.write(scalarString(value).toUtf8());
        }
        if (const auto span = sourceSpan(value); span.has_value()) {
            return m_output.writeSource(m_store, *span);
        }
        if (const DecodedValue* decoded = baseValue(value)) {
            if (decoded->kind == DecodedValueKind::OwnedBytes) {
                return m_output.write(m_tree.ownedBytes.value(decoded->payload));
            }
        }
        if (const TempValue* temporary = tempValue(value)) {
            if (temporary->kind == DecodedValueKind::OwnedBytes) {
                return m_output.write(temporary->ownedBytes);
            }
        }
        m_error = QStringLiteral("Binary outform emit value is not bytes");
        return false;
    }

    bool execBlock(IdRange range) {
        if (range.first > static_cast<quint32>(m_program.statementRefs.size()) ||
            range.count > static_cast<quint32>(m_program.statementRefs.size()) -
                              range.first) {
            m_error = QStringLiteral("Invalid outform statement range");
            return false;
        }
        for (quint32 i = 0; i < range.count; ++i) {
            const StatementId id =
                m_program.statementRefs.at(range.first + i);
            if (id >= static_cast<StatementId>(m_program.statements.size())) {
                m_error = QStringLiteral("Invalid outform statement");
                return false;
            }
            const Statement& statement = m_program.statements.at(id);
            if (statement.kind == StatementKind::Emit) {
                const ValueRef value = eval(statement.expression);
                if (!value.valid() || !emitValue(value)) return false;
                continue;
            }
            if (statement.kind == StatementKind::Let) {
                const ValueRef value = eval(statement.expression);
                if (!value.valid()) return false;
                if (statement.resultSlot >=
                    static_cast<quint32>(m_slots.size())) {
                    m_error = QStringLiteral("Invalid outform local slot");
                    return false;
                }
                m_slots[statement.resultSlot] = value;
                continue;
            }
            if (statement.kind == StatementKind::If) {
                const ValueRef condition = eval(statement.condition);
                if (!condition.valid()) return false;
                if (!execBlock(truth(condition) ? statement.statements
                                                : statement.elseStatements)) {
                    return false;
                }
                continue;
            }
            if (statement.kind == StatementKind::For) {
                const ValueRef sequence = eval(statement.expression);
                if (!sequence.valid()) return false;
                const QVector<ValueRef> values = elements(sequence);
                if (static_cast<quint64>(values.size()) >
                    m_program.limits.maxLoopIterations) {
                    m_error = QStringLiteral("Outform loop iteration limit exceeded");
                    return false;
                }
                for (qsizetype index = 0; index < values.size(); ++index) {
                    if (statement.iterationSlot >=
                        static_cast<quint32>(m_slots.size())) {
                        m_error = QStringLiteral("Invalid outform iteration slot");
                        return false;
                    }
                    m_slots[statement.iterationSlot] = values.at(index);
                    if (statement.secondarySlot != kInvalidId) {
                        if (statement.secondarySlot >=
                            static_cast<quint32>(m_slots.size())) {
                            m_error = QStringLiteral("Invalid outform index slot");
                            return false;
                        }
                        m_slots[statement.secondarySlot] =
                            addUnsigned(kInvalidId,
                                        static_cast<quint64>(index));
                    }
                    if (!execBlock(statement.statements)) return false;
                }
                continue;
            }
            m_error = QStringLiteral("Decode statement cannot run in an outform");
            return false;
        }
        return true;
    }

    const RenderStore& m_store;
    const BrecoProgram& m_program;
    const DecodedTree& m_tree;
    const OutformDesc& m_outform;
    Output m_output;
    QVector<ValueRef> m_slots;
    QVector<TempValue> m_temporaries;
    QString m_error;
};

}  // namespace

OutformRenderResult renderOutform(const RenderStore& store,
                                  QStringView outformName, QIODevice* output) {
    const OutformDesc* selected = nullptr;
    QStringList available;
    for (const OutformDesc& outform : store.program().outforms) {
        const QString name = store.program().symbol(outform.name);
        available.push_back(name);
        if (name == outformName) {
            selected = &outform;
        }
    }
    if (selected == nullptr) {
        QString error = QStringLiteral("Unknown outform '%1'")
                            .arg(outformName.toString());
        if (!available.isEmpty()) {
            error += QStringLiteral(". Available outforms: %1")
                         .arg(available.join(QStringLiteral(", ")));
        }
        return {false, std::move(error), 0};
    }
    return Renderer(store, *selected, output).run();
}

}  // namespace breco::lang

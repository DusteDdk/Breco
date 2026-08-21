#include "brecolang/runtime/Interpreter.h"

#include "brecolang/runtime/JsonWriter.h"

#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace breco::lang {

namespace {

enum class Flow {
    Success,
    NoMatch,
    Failure,
    Break,
    Continue,
};

struct Outcome {
    Flow flow = Flow::Success;
    bool committed = false;
    bool fatal = false;
    bool hasDiagnostic = false;
    RuntimeDiagnostic diagnostic;

    bool succeeded() const { return flow == Flow::Success; }
};

struct StatementResult {
    Outcome outcome;
    DecodedValueId value = kInvalidId;
    bool hasValue = false;
};

struct EvalResult {
    Outcome outcome;
    DecodedValueId value = kInvalidId;
};

struct Cursor {
    InputId input = kInvalidId;
    quint64 position = 0;
    std::optional<quint64> end;
};

struct Frame {
    QVector<DecodedValueId> values;
    bool committed = false;
};

struct ProbeAnchor {
    InputId input = kInvalidId;
    quint64 position = 0;
    bool committed = false;
};

bool addWouldOverflow(quint64 left, quint64 right) {
    return right > std::numeric_limits<quint64>::max() - left;
}

class Interpreter {
public:
    explicit Interpreter(const DecodeRequest& request)
        : m_request(request), m_program(*request.program),
          m_store(std::make_shared<DecodedTree>()) {
        if (m_request.mode == DecodeMode::Streaming) {
            m_writer = std::make_unique<JsonWriter>(m_request.output);
        }
    }

    DecodeResult run() {
        DecodeResult result;
        if (!m_request.program) {
            result.diagnostics.push_back(
                {DiagnosticSeverity::Error, QStringLiteral("BRR0001"),
                 QStringLiteral("No compiled BrecoLang program was provided"),
                 {}, {}, false});
            return result;
        }

        const EntryDesc* entry = findEntry();
        if (entry == nullptr) {
            result.diagnostics.push_back(
                {DiagnosticSeverity::Error, QStringLiteral("BRR0002"),
                 QStringLiteral("Unknown or missing entry '%1'")
                     .arg(m_request.entryName),
                 {}, {}, false});
            return result;
        }
        if (entry->input >= static_cast<InputId>(m_request.inputs.size()) ||
            !m_request.inputs.at(entry->input)) {
            result.diagnostics.push_back(
                {DiagnosticSeverity::Error, QStringLiteral("BRR0003"),
                 QStringLiteral("Input '%1' is not bound")
                     .arg(entry->input < static_cast<InputId>(m_program.inputs.size())
                              ? m_program.symbol(m_program.inputs.at(entry->input).name)
                              : QString()),
                 {}, {}, false});
            return result;
        }
        if (m_request.mode == DecodeMode::Streaming &&
            (m_writer == nullptr || !m_writer->errorString().isEmpty())) {
            result.diagnostics.push_back(
                {DiagnosticSeverity::Error, QStringLiteral("BRR0005"),
                 m_writer != nullptr ? m_writer->errorString()
                                     : QStringLiteral("No JSON output was provided"),
                 {}, {}, false});
            return result;
        }

        Cursor cursor;
        cursor.input = entry->input;
        cursor.position = m_request.startOffset;
        cursor.end = source(entry->input)->size();
        if (cursor.end.has_value() && cursor.position > *cursor.end) {
            result.diagnostics.push_back(
                {DiagnosticSeverity::Error, QStringLiteral("BRR0004"),
                 QStringLiteral("Entry offset is beyond the input"), {},
                 byteSpan(cursor.input, cursor.position, 0), true});
            return result;
        }

        Frame frame;
        frame.values.resize(entry->slotCount, kInvalidId);
        DecodedNodeId rootNode = kInvalidId;
        if (!createNode(DecodedNodeKind::Entry, kInvalidId,
                        m_program.symbol(entry->name), entry->resultType,
                        kInvalidId, cursor.input, &rootNode)) {
            result.diagnostics = std::move(m_diagnostics);
            return result;
        }
        const quint64 start = cursor.position;
        if (emitting() && !m_writer->beginObject()) {
            result.diagnostics.push_back(writerDiagnostic(kInvalidId, cursor));
            return result;
        }
        Outcome outcome = execBlock(entry->statements, frame, cursor, rootNode);
        if (outcome.succeeded() && emitting() &&
            (!m_writer->endObject() || !m_writer->finish())) {
            outcome = writerFailure(kInvalidId, cursor);
        }
        if (outcome.succeeded()) {
            const DecodedValueId rootValue =
                buildObject(entry->resultType, frame, rootNode);
            if (rootNode != kInvalidId) {
                m_store->nodes[rootNode].value = rootValue;
                setCompositeLayout(rootNode, entry->resultType, cursor.input,
                                   start, cursor.position);
            }
            result.rootValue = rootValue;
            result.status = DecodeStatus::Success;
        } else {
            result.status = outcome.flow == Flow::NoMatch
                                ? DecodeStatus::NoMatch
                                : DecodeStatus::Error;
            if (outcome.hasDiagnostic) {
                m_diagnostics.push_back(outcome.diagnostic);
            }
        }
        if (m_request.mode == DecodeMode::Tree) {
            m_store->finalizeTopology();
            result.constructedNodes = static_cast<quint64>(m_store->nodes.size());
            result.tree = m_store;
        }
        result.diagnostics = std::move(m_diagnostics);
        result.entryInput = entry->input;
        result.startOffset = start;
        result.endOffset = cursor.position;
        return result;
    }

private:
    const EntryDesc* findEntry() const {
        QString wanted = m_request.entryName;
        if (wanted.isEmpty() && m_program.defaultEntry != kInvalidId) {
            wanted = m_program.symbol(m_program.defaultEntry);
        }
        for (const EntryDesc& entry : m_program.entries) {
            if (m_program.symbol(entry.name) == wanted) {
                return &entry;
            }
        }
        return nullptr;
    }

    ByteSource* source(InputId input) const {
        return input < static_cast<InputId>(m_request.inputs.size())
                   ? m_request.inputs.at(input).get()
                   : nullptr;
    }

    SourceSpan schemaSpan(SourceSpanId id) const {
        return id < static_cast<SourceSpanId>(m_program.sourceSpans.size())
                   ? m_program.sourceSpans.at(id)
                   : SourceSpan{};
    }

    ByteSpanValue byteSpan(InputId input, quint64 logicalOffset,
                           quint64 length) const {
        ByteSource* bytes = source(input);
        return {input,
                bytes != nullptr ? bytes->absoluteOffset(logicalOffset)
                                 : logicalOffset,
                length};
    }

    Outcome failure(QString code, QString message, SourceSpanId span,
                    const Cursor& cursor, Flow flow = Flow::Failure) const {
        Outcome result;
        result.flow = flow;
        result.fatal = code == QStringLiteral("BRR0100") ||
                       code == QStringLiteral("BRR0251") ||
                       code == QStringLiteral("BRR0303");
        result.hasDiagnostic = true;
        result.diagnostic = {DiagnosticSeverity::Error, std::move(code),
                             std::move(message), schemaSpan(span),
                             byteSpan(cursor.input, cursor.position, 0), true};
        return result;
    }

    Outcome success() const { return {}; }

    bool streaming() const {
        return m_request.mode == DecodeMode::Streaming;
    }

    bool emitting() const {
        return streaming() && m_streamSuppressionDepth == 0 && m_writer != nullptr;
    }

    RuntimeDiagnostic writerDiagnostic(SourceSpanId span,
                                       const Cursor& cursor) const {
        return {DiagnosticSeverity::Error, QStringLiteral("BRR0600"),
                m_writer != nullptr && !m_writer->errorString().isEmpty()
                    ? m_writer->errorString()
                    : QStringLiteral("Could not write streaming JSON output"),
                schemaSpan(span), byteSpan(cursor.input, cursor.position, 0), true};
    }

    Outcome writerFailure(SourceSpanId span, const Cursor& cursor) const {
        Outcome outcome;
        outcome.flow = Flow::Failure;
        outcome.fatal = true;
        outcome.hasDiagnostic = true;
        outcome.diagnostic = writerDiagnostic(span, cursor);
        return outcome;
    }

    bool streamName(SymbolId name) {
        return !emitting() || m_writer->name(m_program.symbol(name));
    }

    bool streamValue(DecodedValueId id) {
        if (!emitting()) {
            return true;
        }
        if (id >= static_cast<DecodedValueId>(m_store->values.size())) {
            return m_writer->nullValue();
        }
        const DecodedValue& value = m_store->values.at(id);
        switch (value.kind) {
            case DecodedValueKind::Null:
            case DecodedValueKind::Invalid:
                return m_writer->nullValue();
            case DecodedValueKind::Boolean:
                return m_writer->boolean(value.booleanValue);
            case DecodedValueKind::UnsignedInteger:
                return m_writer->unsignedInteger(value.unsignedValue);
            case DecodedValueKind::SignedInteger:
                return m_writer->signedInteger(value.signedValue);
            case DecodedValueKind::FloatingPoint:
                return m_writer->floatingPoint(value.floatingValue);
            case DecodedValueKind::String:
                return m_writer->string(m_store->valueStrings.value(value.payload));
            case DecodedValueKind::SourceBytes: {
                if (value.payload >= static_cast<quint32>(m_store->spans.size())) {
                    return false;
                }
                const ByteSpanValue& span = m_store->spans.at(value.payload);
                return m_writer->sourceBytesHex(
                    source(span.input), logicalOffset(span.input, span.offset),
                    span.length);
            }
            case DecodedValueKind::OwnedBytes:
                return m_writer->string(
                    QString::fromLatin1(m_store->ownedBytes.value(value.payload).toHex()));
            case DecodedValueKind::Object:
            case DecodedValueKind::Sequence:
                return false;
        }
        return false;
    }

    bool createNode(DecodedNodeKind kind, DecodedNodeId parent,
                    const QString& name, TypeId type, SourceSpanId schema,
                    InputId input, DecodedNodeId* result) {
        *result = kInvalidId;
        if (m_request.mode != DecodeMode::Tree) {
            return true;
        }
        if (static_cast<quint64>(m_store->nodes.size()) >=
            m_program.limits.maxNodes) {
            m_diagnostics.push_back(
                {DiagnosticSeverity::Error, QStringLiteral("BRR0100"),
                 QStringLiteral("Decoded node limit exceeded"),
                 schemaSpan(schema), {}, false});
            return false;
        }
        DecodedNode node;
        node.kind = kind;
        node.parent = parent;
        node.name = m_store->internName(name);
        node.type = type;
        node.schemaSpan = schema;
        node.input = input;
        *result = m_store->addNode(std::move(node));
        return true;
    }

    void setNodeSource(DecodedNodeId node, TypeId type, InputId input,
                       quint64 start, quint64 length,
                       StorageLayoutKind kind = StorageLayoutKind::SourceSlice,
                       quint8 highBit = 0, quint8 lowBit = 0) {
        const ByteSpanValue span = byteSpan(input, start, length);
        const quint32 spanId = m_store->addSpan(span);
        if (node == kInvalidId) {
            return;
        }
        DecodedNode& decoded = m_store->nodes[node];
        decoded.input = input;
        decoded.offset = span.offset;
        decoded.length = length;
        decoded.hasSourceSpan = true;
        StorageLayout layout;
        layout.kind = kind;
        layout.spans = {spanId, 1};
        layout.declaredType = type;
        if (type < static_cast<TypeId>(m_program.types.size())) {
            const TypeDesc* descriptor = &m_program.types.at(type);
            if (descriptor->kind == TypeKind::Bitfield &&
                descriptor->elementType <
                    static_cast<TypeId>(m_program.types.size())) {
                descriptor = &m_program.types.at(descriptor->elementType);
            }
            layout.endianness = descriptor->endianness;
            layout.bitWidth = descriptor->bitWidth;
        }
        layout.highBit = highBit;
        layout.lowBit = lowBit;
        decoded.storageLayout = m_store->addLayout(std::move(layout));
    }

    void setComputedLayout(DecodedNodeId node, TypeId type) {
        if (node == kInvalidId) {
            return;
        }
        StorageLayout layout;
        layout.kind = StorageLayoutKind::Computed;
        layout.declaredType = type;
        if (type < static_cast<TypeId>(m_program.types.size())) {
            const TypeDesc& descriptor = m_program.types.at(type);
            layout.endianness = descriptor.endianness;
            layout.bitWidth = descriptor.bitWidth;
        }
        m_store->nodes[node].storageLayout =
            m_store->addLayout(std::move(layout));
    }

    void setCompositeLayout(DecodedNodeId node, TypeId type, InputId input,
                            quint64 start, quint64 end) {
        if (node == kInvalidId) {
            return;
        }
        QVector<ByteSpanValue> childSpans;
        if (end > start) {
            childSpans.push_back(byteSpan(input, start, end - start));
        }
        for (const DecodedNode& child : std::as_const(m_store->nodes)) {
            if (child.parent != node ||
                child.storageLayout >=
                    static_cast<quint32>(m_store->storageLayouts.size())) {
                continue;
            }
            const StorageLayout& childLayout =
                m_store->storageLayouts.at(child.storageLayout);
            for (quint32 i = 0; i < childLayout.spans.count; ++i) {
                const ByteSpanValue& childSpan =
                    m_store->spans.at(childLayout.spans.first + i);
                if (end == start || childSpan.input != input) {
                    childSpans.push_back(childSpan);
                }
            }
        }
        if (childSpans.isEmpty()) {
            childSpans.push_back(byteSpan(input, start, end - start));
        }
        StorageLayout layout;
        layout.kind = StorageLayoutKind::Composite;
        layout.declaredType = type;
        layout.spans = m_store->appendSpans(childSpans);
        DecodedNode& decoded = m_store->nodes[node];
        decoded.storageLayout = m_store->addLayout(std::move(layout));
        decoded.input = input;
        if (end > start) {
            decoded.offset = source(input)->absoluteOffset(start);
            decoded.length = end - start;
            decoded.hasSourceSpan = true;
        }
    }

    DecodedValueId addUnsigned(TypeId type, quint64 value,
                               DecodedNodeId node = kInvalidId) {
        DecodedValue decoded;
        decoded.kind = DecodedValueKind::UnsignedInteger;
        decoded.type = type;
        decoded.unsignedValue = value;
        decoded.node = node;
        return m_store->addValue(std::move(decoded));
    }

    DecodedValueId addSigned(TypeId type, qint64 value,
                             DecodedNodeId node = kInvalidId) {
        DecodedValue decoded;
        decoded.kind = DecodedValueKind::SignedInteger;
        decoded.type = type;
        decoded.signedValue = value;
        decoded.node = node;
        return m_store->addValue(std::move(decoded));
    }

    DecodedValueId addBoolean(TypeId type, bool value,
                              DecodedNodeId node = kInvalidId) {
        DecodedValue decoded;
        decoded.kind = DecodedValueKind::Boolean;
        decoded.type = type;
        decoded.booleanValue = value;
        decoded.node = node;
        return m_store->addValue(std::move(decoded));
    }

    DecodedValueId addNull(TypeId type) {
        DecodedValue decoded;
        decoded.kind = DecodedValueKind::Null;
        decoded.type = type;
        return m_store->addValue(std::move(decoded));
    }

    DecodedValueId addFloat(TypeId type, double value,
                            DecodedNodeId node = kInvalidId) {
        DecodedValue decoded;
        decoded.kind = DecodedValueKind::FloatingPoint;
        decoded.type = type;
        decoded.floatingValue = value;
        decoded.node = node;
        return m_store->addValue(std::move(decoded));
    }

    DecodedValueId addString(TypeId type, QString value,
                             DecodedNodeId node = kInvalidId) {
        DecodedValue decoded;
        decoded.kind = DecodedValueKind::String;
        decoded.type = type;
        decoded.payload = m_store->addValueString(std::move(value));
        decoded.node = node;
        return m_store->addValue(std::move(decoded));
    }

    DecodedValueId addSourceBytes(TypeId type, InputId input, quint64 offset,
                                  quint64 length,
                                  DecodedNodeId node = kInvalidId) {
        DecodedValue decoded;
        decoded.kind = DecodedValueKind::SourceBytes;
        decoded.type = type;
        decoded.payload = m_store->addSpan(byteSpan(input, offset, length));
        decoded.node = node;
        return m_store->addValue(std::move(decoded));
    }

    DecodedValueId addOwnedBytes(TypeId type, QByteArray bytes) {
        DecodedValue decoded;
        decoded.kind = DecodedValueKind::OwnedBytes;
        decoded.type = type;
        decoded.payload = m_store->addOwnedBytes(std::move(bytes));
        return m_store->addValue(std::move(decoded));
    }

    DecodedValueId addSequence(TypeId type,
                               const QVector<DecodedValueId>& elements,
                               DecodedNodeId node) {
        DecodedValue decoded;
        decoded.kind = DecodedValueKind::Sequence;
        decoded.type = type;
        decoded.elements = m_store->appendValueRefs(elements);
        decoded.node = node;
        return m_store->addValue(std::move(decoded));
    }

    DecodedValueId buildObject(TypeId type, const Frame& frame,
                               DecodedNodeId node) {
        QVector<DecodedFieldValue> fields;
        if (type < static_cast<TypeId>(m_program.types.size())) {
            const TypeDesc& descriptor = m_program.types.at(type);
            for (quint32 i = 0; i < descriptor.fields.count; ++i) {
                const quint32 fieldId =
                    m_program.fieldRefs.at(descriptor.fields.first + i);
                if (fieldId >= static_cast<quint32>(m_program.fields.size())) {
                    continue;
                }
                const FieldDesc& field = m_program.fields.at(fieldId);
                if (field.statement >=
                    static_cast<StatementId>(m_program.statements.size())) {
                    continue;
                }
                const quint32 slot =
                    m_program.statements.at(field.statement).resultSlot;
                if (slot != kInvalidId && slot < static_cast<quint32>(frame.values.size()) &&
                    frame.values.at(slot) != kInvalidId) {
                    fields.push_back(DecodedFieldValue{field.name, frame.values.at(slot)});
                }
            }
        }
        DecodedValue decoded;
        decoded.kind = DecodedValueKind::Object;
        decoded.type = type;
        decoded.fields = m_store->appendFieldValues(fields);
        decoded.node = node;
        return m_store->addValue(std::move(decoded));
    }

    void clearObjectSlots(TypeId type, Frame& frame) const {
        if (type >= static_cast<TypeId>(m_program.types.size())) {
            return;
        }
        const TypeDesc& descriptor = m_program.types.at(type);
        for (quint32 i = 0; i < descriptor.fields.count; ++i) {
            const FieldDesc& field = m_program.fields.at(
                m_program.fieldRefs.at(descriptor.fields.first + i));
            const quint32 slot =
                m_program.statements.at(field.statement).resultSlot;
            if (slot != kInvalidId && slot < static_cast<quint32>(frame.values.size())) {
                frame.values[slot] = kInvalidId;
            }
        }
    }

    Outcome execBlock(IdRange statements, Frame& frame, Cursor& cursor,
                      DecodedNodeId parent) {
        if (statements.first > static_cast<quint32>(m_program.statementRefs.size()) ||
            statements.count > static_cast<quint32>(m_program.statementRefs.size()) -
                                   statements.first) {
            return failure(QStringLiteral("BRR0101"),
                           QStringLiteral("Invalid resolved statement range"),
                           kInvalidId, cursor);
        }
        for (quint32 i = 0; i < statements.count; ++i) {
            const StatementId id = m_program.statementRefs.at(statements.first + i);
            if (id >= static_cast<StatementId>(m_program.statements.size())) {
                return failure(QStringLiteral("BRR0102"),
                               QStringLiteral("Invalid resolved statement"),
                               kInvalidId, cursor);
            }
            const Statement& statement = m_program.statements.at(id);
            if (statement.resultSlot != kInvalidId &&
                statement.resultSlot < static_cast<quint32>(frame.values.size())) {
                frame.values[statement.resultSlot] = kInvalidId;
            }
            StatementResult result = execStatement(statement, frame, cursor, parent);
            if (!result.outcome.succeeded()) {
                result.outcome.committed =
                    result.outcome.committed || frame.committed;
                return result.outcome;
            }
            if (result.hasValue && statement.resultSlot != kInvalidId &&
                statement.resultSlot < static_cast<quint32>(frame.values.size())) {
                frame.values[statement.resultSlot] = result.value;
            }
            releaseProbePrefix(cursor);
        }
        return success();
    }

    StatementResult execStatement(const Statement& statement, Frame& frame,
                                  Cursor& cursor, DecodedNodeId parent) {
        switch (statement.kind) {
            case StatementKind::Field:
                return execField(statement, frame, cursor, parent);
            case StatementKind::ComputedField:
                return execComputed(statement, frame, cursor, parent);
            case StatementKind::BitfieldField:
                return execBitfield(statement, frame, cursor, parent);
            case StatementKind::Identify:
                return {execBlock(statement.statements, frame, cursor, parent),
                        kInvalidId, false};
            case StatementKind::Commit:
                frame.committed = true;
                if (!m_probeAnchors.isEmpty()) {
                    m_probeAnchors.last().committed = true;
                }
                return {};
            case StatementKind::Require:
            case StatementKind::Check:
            case StatementKind::Match:
                return execValidation(statement, frame, cursor, parent);
            case StatementKind::Preserve:
            case StatementKind::Raw:
                return execRaw(statement, frame, cursor, parent);
            case StatementKind::Region:
                return execRegion(statement, frame, cursor, parent);
            case StatementKind::Repeat:
            case StatementKind::While:
                return execLoop(statement, frame, cursor, parent);
            case StatementKind::Many:
                return execMany(statement, frame, cursor, parent);
            case StatementKind::Select:
                return execSelect(statement, frame, cursor, parent);
            case StatementKind::OneOf:
                return execOneOf(statement, frame, cursor, parent);
            case StatementKind::Recover:
                return execRecover(statement, frame, cursor, parent);
            case StatementKind::Continue:
            case StatementKind::Break:
                return execLoopControl(statement, frame, cursor);
            case StatementKind::Let: {
                const EvalResult evaluated = eval(statement.expression, frame, cursor);
                if (!evaluated.outcome.succeeded()) {
                    return {evaluated.outcome, kInvalidId, false};
                }
                return {success(), coerce(evaluated.value, statement.type), true};
            }
            case StatementKind::Emit:
            case StatementKind::If:
            case StatementKind::For:
                return {failure(QStringLiteral("BRR0103"),
                                QStringLiteral("Outform statement cannot run in decode mode"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            case StatementKind::Invalid:
                break;
        }
        return {failure(QStringLiteral("BRR0104"),
                        QStringLiteral("Invalid statement in decoded program"),
                        statement.sourceSpan, cursor),
                kInvalidId, false};
    }

    StatementResult execField(const Statement& statement, Frame& frame,
                              Cursor& cursor, DecodedNodeId parent) {
        if (statement.condition != kInvalidId) {
            const EvalResult condition = eval(statement.condition, frame, cursor);
            if (!condition.outcome.succeeded()) {
                return {condition.outcome, kInvalidId, false};
            }
            if (!asBoolean(condition.value).value_or(false)) {
                const DecodedValueId value =
                    addNull(expressionSlotType(statement.resultSlot));
                if (!streamName(statement.name) || !streamValue(value)) {
                    return {writerFailure(statement.sourceSpan, cursor),
                            kInvalidId, false};
                }
                return {success(), value, true};
            }
        }

        Cursor child = cursor;
        const bool external = statement.input != kInvalidId;
        if (external) {
            if (statement.input >= static_cast<InputId>(m_request.inputs.size()) ||
                !source(statement.input)) {
                return {failure(QStringLiteral("BRR0200"),
                                QStringLiteral("Input '%1' is not bound")
                                    .arg(statement.input <
                                                 static_cast<InputId>(m_program.inputs.size())
                                             ? m_program.symbol(
                                                   m_program.inputs.at(statement.input).name)
                                             : QString()),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
            const EvalResult offset = eval(statement.expression, frame, cursor);
            if (!offset.outcome.succeeded()) {
                return {offset.outcome, kInvalidId, false};
            }
            const auto value = asUnsigned(offset.value);
            if (!value.has_value()) {
                return {failure(QStringLiteral("BRR0201"),
                                QStringLiteral("Input offset is not an integer"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
            child.input = statement.input;
            child.position = *value;
            child.end = source(child.input)->size();
        }
        if (statement.secondaryExpression != kInvalidId) {
            const EvalResult length =
                eval(statement.secondaryExpression, frame, cursor);
            if (!length.outcome.succeeded()) {
                return {length.outcome, kInvalidId, false};
            }
            const auto amount = asUnsigned(length.value);
            if (!amount.has_value() || addWouldOverflow(child.position, *amount)) {
                return {failure(QStringLiteral("BRR0202"),
                                QStringLiteral("Invalid within range"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
            const quint64 end = child.position + *amount;
            if (child.end.has_value() && end > *child.end) {
                return {failure(QStringLiteral("BRR0203"),
                                QStringLiteral("Within range exceeds its input"),
                                statement.sourceSpan, child),
                        kInvalidId, false};
            }
            child.end = end;
        }

        DecodedNodeId node = kInvalidId;
        if (!createNode(nodeKindForType(statement.type), parent,
                        m_program.symbol(statement.name), statement.type,
                        statement.sourceSpan, child.input, &node)) {
            return {failure(QStringLiteral("BRR0100"),
                            QStringLiteral("Decoded node limit exceeded"),
                            statement.sourceSpan, child),
                    kInvalidId, false};
        }
        QVector<DecodedValueId> arguments;
        const Outcome argumentsOutcome =
            evalArguments(statement.arguments, frame, cursor, &arguments);
        if (!argumentsOutcome.succeeded()) {
            return {argumentsOutcome, kInvalidId, false};
        }
        if (!streamName(statement.name)) {
            return {writerFailure(statement.sourceSpan, child), kInvalidId,
                    false};
        }
        DecodedValueId value = kInvalidId;
        Outcome outcome = decodeType(statement.type, arguments, frame, child, node,
                                     &value, statement.sourceSpan);
        if (!outcome.succeeded()) {
            return {outcome, kInvalidId, false};
        }
        if (!external) {
            cursor.position = child.position;
        }
        return {success(), value, true};
    }

    StatementResult execComputed(const Statement& statement, Frame& frame,
                                 Cursor& cursor, DecodedNodeId parent) {
        const EvalResult evaluated = eval(statement.expression, frame, cursor);
        if (!evaluated.outcome.succeeded()) {
            return {evaluated.outcome, kInvalidId, false};
        }
        const DecodedValueId value = coerce(evaluated.value, statement.type);
        DecodedNodeId node = kInvalidId;
        if (!createNode(DecodedNodeKind::Computed, parent,
                        m_program.symbol(statement.name), statement.type,
                        statement.sourceSpan, cursor.input, &node)) {
            return {failure(QStringLiteral("BRR0100"),
                            QStringLiteral("Decoded node limit exceeded"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        if (node != kInvalidId) {
            m_store->nodes[node].value = value;
            setComputedLayout(node, statement.type);
            m_store->values[value].node = node;
        }
        if (!streamName(statement.name) || !streamValue(value)) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        return {success(), value, true};
    }

    StatementResult execBitfield(const Statement& statement, Frame& frame,
                                 Cursor& cursor, DecodedNodeId parent) {
        Q_UNUSED(frame);
        if (statement.type >= static_cast<TypeId>(m_program.types.size())) {
            return {failure(QStringLiteral("BRR0210"),
                            QStringLiteral("Invalid bitfield type"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        const TypeId storage = m_program.types.at(statement.type).elementType;
        DecodedNodeId node = kInvalidId;
        if (!createNode(DecodedNodeKind::Bitfield, parent,
                        m_program.symbol(statement.name), statement.type,
                        statement.sourceSpan, cursor.input, &node)) {
            return {failure(QStringLiteral("BRR0100"),
                            QStringLiteral("Decoded node limit exceeded"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        const quint64 start = cursor.position;
        DecodedValueId storageValue = kInvalidId;
        if (streaming()) {
            ++m_streamSuppressionDepth;
        }
        Outcome outcome = decodePrimitive(storage, cursor, node, &storageValue,
                                          statement.sourceSpan);
        if (streaming()) {
            --m_streamSuppressionDepth;
        }
        if (!outcome.succeeded()) {
            return {outcome, kInvalidId, false};
        }
        if (!streamName(statement.name) ||
            (emitting() && !m_writer->beginObject())) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        const quint64 bits = asUnsigned(storageValue).value_or(0);
        QVector<DecodedFieldValue> decodedMembers;
        for (quint32 i = 0; i < statement.bitMembers.count; ++i) {
            const quint32 memberId =
                m_program.bitMemberRefs.at(statement.bitMembers.first + i);
            const BitMember& member = m_program.bitMembers.at(memberId);
            const quint8 width = member.highBit - member.lowBit + 1;
            const quint64 mask = width == 64
                                     ? std::numeric_limits<quint64>::max()
                                     : ((quint64{1} << width) - 1);
            const quint64 memberValue = (bits >> member.lowBit) & mask;
            TypeId memberType = kInvalidId;
            const TypeDesc& bitfield = m_program.types.at(statement.type);
            for (quint32 fieldIndex = 0; fieldIndex < bitfield.fields.count;
                 ++fieldIndex) {
                const FieldDesc& field = m_program.fields.at(
                    m_program.fieldRefs.at(bitfield.fields.first + fieldIndex));
                if (field.name == member.name) {
                    memberType = field.type;
                    break;
                }
            }
            DecodedNodeId child = kInvalidId;
            if (!createNode(DecodedNodeKind::BitMember, node,
                            m_program.symbol(member.name), memberType,
                            member.sourceSpan, cursor.input, &child)) {
                return {failure(QStringLiteral("BRR0100"),
                                QStringLiteral("Decoded node limit exceeded"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
            const DecodedValueId childValue =
                width == 1 ? addBoolean(memberType, memberValue != 0, child)
                           : addUnsigned(memberType, memberValue, child);
            if (!streamName(member.name) || !streamValue(childValue)) {
                return {writerFailure(member.sourceSpan, cursor), kInvalidId,
                        false};
            }
            decodedMembers.push_back({member.name, childValue});
            if (child != kInvalidId) {
                m_store->nodes[child].value = childValue;
                setNodeSource(child, memberType, cursor.input, start,
                              cursor.position - start,
                              StorageLayoutKind::BitSlice, member.highBit,
                              member.lowBit);
            }
        }
        DecodedValue bitfieldValue;
        bitfieldValue.kind = DecodedValueKind::Object;
        bitfieldValue.type = statement.type;
        bitfieldValue.fields = m_store->appendFieldValues(decodedMembers);
        bitfieldValue.node = node;
        const DecodedValueId result =
            m_store->addValue(std::move(bitfieldValue));
        if (node != kInvalidId) {
            m_store->nodes[node].type = statement.type;
            m_store->nodes[node].kind = DecodedNodeKind::Bitfield;
            m_store->nodes[node].value = result;
            setNodeSource(node, statement.type, cursor.input, start,
                          cursor.position - start);
        }
        if (emitting() && !m_writer->endObject()) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        return {success(), result, true};
    }

    StatementResult execValidation(const Statement& statement, Frame& frame,
                                   Cursor& cursor, DecodedNodeId parent) {
        const EvalResult evaluated = eval(statement.expression, frame, cursor);
        if (!evaluated.outcome.succeeded()) {
            return {evaluated.outcome, kInvalidId, false};
        }
        if (asBoolean(evaluated.value).value_or(false)) {
            return {};
        }
        const QString message = statement.message != kInvalidId
                                    ? m_program.symbol(statement.message)
                                    : QStringLiteral("Validation failed");
        if (statement.kind == StatementKind::Check) {
            RuntimeDiagnostic diagnostic{
                DiagnosticSeverity::Warning, QStringLiteral("BRR0220"), message,
                schemaSpan(statement.sourceSpan),
                byteSpan(cursor.input, cursor.position, 0), true};
            m_diagnostics.push_back(std::move(diagnostic));
            if (parent != kInvalidId) {
                m_store->nodes[parent].valid = false;
                m_store->nodes[parent].error = m_store->internName(message);
            }
            return {};
        }
        const Flow flow = statement.kind == StatementKind::Match
                              ? Flow::NoMatch
                              : Flow::Failure;
        return {failure(statement.kind == StatementKind::Match
                            ? QStringLiteral("BRR0221")
                            : QStringLiteral("BRR0222"),
                        message, statement.sourceSpan, cursor, flow),
                kInvalidId, false};
    }

    StatementResult execRaw(const Statement& statement, Frame& frame,
                            Cursor& cursor, DecodedNodeId parent) {
        Q_UNUSED(frame);
        const auto end = cursorEnd(cursor);
        if (!end.has_value() || *end < cursor.position) {
            return {failure(QStringLiteral("BRR0230"),
                            QStringLiteral("Could not determine remaining input"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        const quint64 start = cursor.position;
        const quint64 length = *end - start;
        DecodedNodeId node = kInvalidId;
        const DecodedNodeKind kind = statement.kind == StatementKind::Preserve
                                         ? DecodedNodeKind::Preserve
                                         : DecodedNodeKind::Raw;
        if (!createNode(kind, parent, m_program.symbol(statement.name),
                        statement.type, statement.sourceSpan, cursor.input,
                        &node)) {
            return {failure(QStringLiteral("BRR0100"),
                            QStringLiteral("Decoded node limit exceeded"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        const DecodedValueId value =
            addSourceBytes(statement.type, cursor.input, start, length, node);
        if (!streamName(statement.name) ||
            (emitting() &&
             !m_writer->sourceBytesHex(source(cursor.input), start, length))) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        cursor.position = *end;
        if (node != kInvalidId) {
            m_store->nodes[node].value = value;
            setNodeSource(node, statement.type, cursor.input, start, length);
        }
        return {success(), value, true};
    }

    StatementResult execRegion(const Statement& statement, Frame& frame,
                               Cursor& cursor, DecodedNodeId parent) {
        const EvalResult length = eval(statement.expression, frame, cursor);
        if (!length.outcome.succeeded()) {
            return {length.outcome, kInvalidId, false};
        }
        const auto amount = asUnsigned(length.value);
        if (!amount.has_value() || addWouldOverflow(cursor.position, *amount)) {
            return {failure(QStringLiteral("BRR0240"),
                            QStringLiteral("Invalid region length"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        const quint64 start = cursor.position;
        const quint64 end = start + *amount;
        if (cursor.end.has_value() && end > *cursor.end) {
            return {failure(QStringLiteral("BRR0241"),
                            QStringLiteral("Region exceeds its containing input"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        DecodedNodeId node = kInvalidId;
        if (!createNode(DecodedNodeKind::Region, parent,
                        m_program.symbol(statement.name), statement.type,
                        statement.sourceSpan, cursor.input, &node)) {
            return {failure(QStringLiteral("BRR0100"),
                            QStringLiteral("Decoded node limit exceeded"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        Cursor region{cursor.input, start, end};
        clearObjectSlots(statement.type, frame);
        if (!streamName(statement.name) ||
            (emitting() && !m_writer->beginObject())) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        Outcome outcome = execBlock(statement.statements, frame, region, node);
        if (!outcome.succeeded()) {
            return {outcome, kInvalidId, false};
        }
        if (emitting() && !m_writer->endObject()) {
            return {writerFailure(statement.sourceSpan, region), kInvalidId,
                    false};
        }
        const DecodedValueId value = buildObject(statement.type, frame, node);
        cursor.position = end;
        if (node != kInvalidId) {
            m_store->nodes[node].value = value;
            setCompositeLayout(node, statement.type, cursor.input, start, end);
        }
        return {success(), value, true};
    }

    StatementResult execLoop(const Statement& statement, Frame& frame,
                             Cursor& cursor, DecodedNodeId parent) {
        if (streaming() && m_streamSuppressionDepth == 0 &&
            m_streamLoopReplayDepth == 0) {
            const Cursor savedCursor = cursor;
            const Frame savedFrame = frame;
            const DecodedTreeCheckpoint checkpoint = m_store->checkpoint();
            const qsizetype diagnosticCheckpoint = m_diagnostics.size();
            const QVector<ProbeAnchor> savedAnchors = m_probeAnchors;
            const quint32 savedDepth = m_depth;

            ++m_streamSuppressionDepth;
            const StatementResult trial = execLoop(statement, frame, cursor, parent);
            --m_streamSuppressionDepth;

            cursor = savedCursor;
            frame = savedFrame;
            m_store->rollback(checkpoint);
            m_diagnostics.resize(diagnosticCheckpoint);
            m_probeAnchors = savedAnchors;
            m_depth = savedDepth;
            if (!trial.outcome.succeeded()) {
                return {trial.outcome, kInvalidId, false};
            }

            ++m_streamLoopReplayDepth;
            StatementResult replay = execLoop(statement, frame, cursor, parent);
            --m_streamLoopReplayDepth;
            return replay;
        }

        DecodedNodeId node = kInvalidId;
        if (!createNode(DecodedNodeKind::Sequence, parent,
                        m_program.symbol(statement.name), statement.type,
                        statement.sourceSpan, cursor.input, &node)) {
            return {failure(QStringLiteral("BRR0100"),
                            QStringLiteral("Decoded node limit exceeded"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        const quint64 start = cursor.position;
        for (quint32 i = 0; i < statement.initializers.count; ++i) {
            const Statement& initializer = m_program.statements.at(
                m_program.statementRefs.at(statement.initializers.first + i));
            StatementResult initialized =
                execStatement(initializer, frame, cursor, node);
            if (!initialized.outcome.succeeded()) {
                return initialized;
            }
            if (initialized.hasValue && initializer.resultSlot != kInvalidId) {
                frame.values[initializer.resultSlot] = initialized.value;
            }
        }

        std::optional<quint64> count;
        if (statement.kind == StatementKind::Repeat &&
            statement.expression != kInvalidId) {
            const EvalResult evaluated = eval(statement.expression, frame, cursor);
            if (!evaluated.outcome.succeeded()) {
                return {evaluated.outcome, kInvalidId, false};
            }
            count = asUnsigned(evaluated.value);
            if (!count.has_value()) {
                return {failure(QStringLiteral("BRR0250"),
                                QStringLiteral("Repeat count is not an integer"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
        }
        const TypeId itemType =
            statement.type < static_cast<TypeId>(m_program.types.size())
                ? m_program.types.at(statement.type).elementType
                : kInvalidId;
        QVector<DecodedValueId> items;
        if (!streamName(statement.name) ||
            (emitting() && !m_writer->beginArray())) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        for (quint64 iteration = 0;; ++iteration) {
            if (count.has_value() && iteration >= *count) {
                break;
            }
            if (statement.iterationSlot != kInvalidId &&
                statement.iterationSlot < static_cast<quint32>(frame.values.size())) {
                frame.values[statement.iterationSlot] =
                    addUnsigned(expressionSlotType(statement.iterationSlot), iteration);
            }
            if (statement.kind == StatementKind::While) {
                const EvalResult condition = eval(statement.condition, frame, cursor);
                if (!condition.outcome.succeeded()) {
                    return {condition.outcome, kInvalidId, false};
                }
                if (!asBoolean(condition.value).value_or(false)) {
                    break;
                }
            }
            if (iteration >= m_program.limits.maxLoopIterations) {
                return {failure(QStringLiteral("BRR0251"),
                                QStringLiteral("Loop iteration limit exceeded"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
            clearObjectSlots(itemType, frame);
            DecodedNodeId itemNode = kInvalidId;
            if (!createNode(DecodedNodeKind::SequenceItem, node,
                            QStringLiteral("[%1]").arg(iteration), itemType,
                            statement.sourceSpan, cursor.input, &itemNode)) {
                return {failure(QStringLiteral("BRR0100"),
                                QStringLiteral("Decoded node limit exceeded"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
            const quint64 itemStart = cursor.position;
            if (emitting() && !m_writer->beginObject()) {
                return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                        false};
            }
            Outcome body = execBlock(statement.statements, frame, cursor, itemNode);
            if (body.flow == Flow::Failure || body.flow == Flow::NoMatch) {
                return {body, kInvalidId, false};
            }
            if (emitting() && !m_writer->endObject()) {
                return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                        false};
            }
            const DecodedValueId item = buildObject(itemType, frame, itemNode);
            items.push_back(item);
            if (itemNode != kInvalidId) {
                m_store->nodes[itemNode].value = item;
                setCompositeLayout(itemNode, itemType, cursor.input, itemStart,
                                   cursor.position);
            }
            if (body.flow == Flow::Break) {
                break;
            }
            bool stop = false;
            if (statement.kind == StatementKind::Repeat &&
                statement.condition != kInvalidId) {
                const EvalResult until = eval(statement.condition, frame, cursor);
                if (!until.outcome.succeeded()) {
                    return {until.outcome, kInvalidId, false};
                }
                stop = asBoolean(until.value).value_or(false);
            }
            if (stop) {
                break;
            }
            releaseProbePrefix(cursor);
            if (!count.has_value() && cursor.position == itemStart) {
                return {failure(QStringLiteral("BRR0252"),
                                QStringLiteral("Unbounded loop made no input progress"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
        }
        if (emitting() && !m_writer->endArray()) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        const DecodedValueId sequence = addSequence(statement.type, items, node);
        if (node != kInvalidId) {
            m_store->nodes[node].value = sequence;
            setCompositeLayout(node, statement.type, cursor.input, start,
                               cursor.position);
        }
        return {success(), sequence, true};
    }

    StatementResult execMany(const Statement& statement, Frame& frame,
                             Cursor& cursor, DecodedNodeId parent) {
        DecodedNodeId node = kInvalidId;
        if (!createNode(DecodedNodeKind::Sequence, parent,
                        m_program.symbol(statement.name), statement.type,
                        statement.sourceSpan, cursor.input, &node)) {
            return {failure(QStringLiteral("BRR0100"),
                            QStringLiteral("Decoded node limit exceeded"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        const quint64 start = cursor.position;
        const TypeId itemType = m_program.types.at(statement.type).elementType;
        QVector<DecodedValueId> items;
        if (!streamName(statement.name) ||
            (emitting() && !m_writer->beginArray())) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        for (quint64 iteration = 0; iteration < m_program.limits.maxLoopIterations;
             ++iteration) {
            if (isAtEnd(cursor)) {
                break;
            }
            const quint64 itemStart = cursor.position;
            const DecodedTreeCheckpoint checkpoint = m_store->checkpoint();
            const qsizetype diagnosticCheckpoint = m_diagnostics.size();
            DecodedNodeId itemNode = kInvalidId;
            if (!createNode(DecodedNodeKind::SequenceItem, node,
                            QStringLiteral("[%1]").arg(iteration), itemType,
                            statement.sourceSpan, cursor.input, &itemNode)) {
                return {failure(QStringLiteral("BRR0100"),
                                QStringLiteral("Decoded node limit exceeded"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
            QVector<DecodedValueId> arguments;
            Outcome argumentOutcome =
                evalArguments(statement.arguments, frame, cursor, &arguments);
            if (!argumentOutcome.succeeded()) {
                return {argumentOutcome, kInvalidId, false};
            }
            m_probeAnchors.push_back({cursor.input, cursor.position, false});
            DecodedValueId item = kInvalidId;
            if (streaming()) {
                ++m_streamSuppressionDepth;
            }
            Outcome attempt = decodeType(itemType, arguments, frame, cursor,
                                         itemNode, &item, statement.sourceSpan);
            if (streaming()) {
                --m_streamSuppressionDepth;
            }
            m_probeAnchors.pop_back();
            if (streaming() && attempt.succeeded()) {
                cursor.position = itemStart;
                m_store->rollback(checkpoint);
                m_diagnostics.resize(diagnosticCheckpoint);
                arguments.clear();
                argumentOutcome =
                    evalArguments(statement.arguments, frame, cursor, &arguments);
                if (!argumentOutcome.succeeded()) {
                    return {argumentOutcome, kInvalidId, false};
                }
                m_probeAnchors.push_back({cursor.input, cursor.position, false});
                attempt = decodeType(itemType, arguments, frame, cursor,
                                     itemNode, &item, statement.sourceSpan);
                m_probeAnchors.pop_back();
            }
            if (!attempt.succeeded()) {
                cursor.position = itemStart;
                m_store->rollback(checkpoint);
                if (attempt.committed || attempt.fatal) {
                    return {attempt, kInvalidId, false};
                }
                m_diagnostics.resize(diagnosticCheckpoint);
                break;
            }
            if (cursor.position == itemStart) {
                return {failure(QStringLiteral("BRR0260"),
                                QStringLiteral("many item made no input progress"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
            items.push_back(item);
            releaseProbePrefix(cursor);
        }
        if (items.size() >=
            static_cast<qsizetype>(m_program.limits.maxLoopIterations) &&
            !isAtEnd(cursor)) {
            return {failure(QStringLiteral("BRR0251"),
                            QStringLiteral("Loop iteration limit exceeded"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        if (emitting() && !m_writer->endArray()) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        const DecodedValueId sequence = addSequence(statement.type, items, node);
        if (node != kInvalidId) {
            m_store->nodes[node].value = sequence;
            setCompositeLayout(node, statement.type, cursor.input, start,
                               cursor.position);
        }
        return {success(), sequence, true};
    }

    StatementResult execSelect(const Statement& statement, Frame& frame,
                               Cursor& cursor, DecodedNodeId parent) {
        DecodedValueId selector = kInvalidId;
        if (statement.expression != kInvalidId) {
            const EvalResult evaluated = eval(statement.expression, frame, cursor);
            if (!evaluated.outcome.succeeded()) {
                return {evaluated.outcome, kInvalidId, false};
            }
            selector = evaluated.value;
        }
        const SelectCase* selected = nullptr;
        const SelectCase* fallback = nullptr;
        for (quint32 i = 0; i < statement.selectCases.count; ++i) {
            const SelectCase& candidate =
                m_program.selectCases.at(statement.selectCases.first + i);
            if (candidate.isDefault) {
                fallback = &candidate;
                continue;
            }
            const EvalResult test = eval(candidate.expression, frame, cursor);
            if (!test.outcome.succeeded()) {
                return {test.outcome, kInvalidId, false};
            }
            const bool matches = candidate.isConditional
                                     ? asBoolean(test.value).value_or(false)
                                     : valuesEqual(selector, test.value);
            if (matches) {
                selected = &candidate;
                break;
            }
        }
        if (selected == nullptr) {
            selected = fallback;
        }
        if (selected == nullptr) {
            return {failure(QStringLiteral("BRR0270"),
                            QStringLiteral("No select branch matched"),
                            statement.sourceSpan, cursor, Flow::NoMatch),
                    kInvalidId, false};
        }
        DecodedNodeId node = kInvalidId;
        if (!createNode(DecodedNodeKind::Select, parent,
                        m_program.symbol(statement.name), statement.type,
                        statement.sourceSpan, cursor.input, &node)) {
            return {failure(QStringLiteral("BRR0100"),
                            QStringLiteral("Decoded node limit exceeded"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        const quint64 start = cursor.position;
        DecodedValueId value = kInvalidId;
        Outcome outcome;
        if (!streamName(statement.name)) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        if (selected->statements.count > 0) {
            clearObjectSlots(selected->resultType, frame);
            if (emitting() && !m_writer->beginObject()) {
                return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                        false};
            }
            outcome = execBlock(selected->statements, frame, cursor, node);
            if (outcome.succeeded()) {
                value = buildObject(selected->resultType, frame, node);
                if (emitting() && !m_writer->endObject()) {
                    return {writerFailure(selected->sourceSpan, cursor),
                            kInvalidId, false};
                }
            }
        } else {
            QVector<DecodedValueId> arguments;
            outcome = evalArguments(selected->arguments, frame, cursor, &arguments);
            if (outcome.succeeded()) {
                outcome = decodeType(selected->resultType, arguments, frame,
                                     cursor, node, &value, selected->sourceSpan);
            }
        }
        if (!outcome.succeeded()) {
            return {outcome, kInvalidId, false};
        }
        if (node != kInvalidId) {
            m_store->nodes[node].value = value;
            setCompositeLayout(node, statement.type, cursor.input, start,
                               cursor.position);
        }
        return {success(), value, true};
    }

    StatementResult execOneOf(const Statement& statement, Frame& frame,
                              Cursor& cursor, DecodedNodeId parent) {
        DecodedNodeId node = kInvalidId;
        if (!createNode(DecodedNodeKind::Alternative, parent,
                        m_program.symbol(statement.name), statement.type,
                        statement.sourceSpan, cursor.input, &node)) {
            return {failure(QStringLiteral("BRR0100"),
                            QStringLiteral("Decoded node limit exceeded"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        const quint64 start = cursor.position;
        Outcome last = failure(QStringLiteral("BRR0280"),
                               QStringLiteral("No one_of alternative matched"),
                               statement.sourceSpan, cursor, Flow::NoMatch);
        if (streaming()) {
            for (quint32 i = 0; i < statement.alternatives.count; ++i) {
                const Alternative& alternative =
                    m_program.alternatives.at(statement.alternatives.first + i);
                const quint64 savedPosition = cursor.position;
                const DecodedTreeCheckpoint checkpoint = m_store->checkpoint();
                const qsizetype diagnosticCheckpoint = m_diagnostics.size();
                QVector<DecodedValueId> arguments;
                Outcome outcome =
                    evalArguments(alternative.arguments, frame, cursor, &arguments);
                if (!outcome.succeeded()) {
                    return {outcome, kInvalidId, false};
                }
                m_probeAnchors.push_back({cursor.input, cursor.position, false});
                ++m_streamSuppressionDepth;
                DecodedValueId trialValue = kInvalidId;
                outcome = decodeType(alternative.type, arguments, frame, cursor,
                                     kInvalidId, &trialValue,
                                     alternative.sourceSpan);
                --m_streamSuppressionDepth;
                const bool probeCommitted = m_probeAnchors.last().committed;
                m_probeAnchors.pop_back();
                const quint64 committedEnd = cursor.position;
                cursor.position = savedPosition;
                m_store->rollback(checkpoint);
                m_diagnostics.resize(diagnosticCheckpoint);
                if (outcome.succeeded()) {
                    if (!streamName(statement.name)) {
                        return {writerFailure(statement.sourceSpan, cursor),
                                kInvalidId, false};
                    }
                    arguments.clear();
                    outcome = evalArguments(alternative.arguments, frame, cursor,
                                            &arguments);
                    if (!outcome.succeeded()) {
                        return {outcome, kInvalidId, false};
                    }
                    m_probeAnchors.push_back(
                        {cursor.input, cursor.position, false});
                    DecodedValueId value = kInvalidId;
                    outcome = decodeType(alternative.type, arguments, frame,
                                         cursor, node, &value,
                                         alternative.sourceSpan);
                    m_probeAnchors.pop_back();
                    if (!outcome.succeeded()) {
                        return {outcome, kInvalidId, false};
                    }
                    Q_UNUSED(committedEnd);
                    return {success(), value, true};
                }
                last = outcome;
                if (outcome.committed || probeCommitted || outcome.fatal) {
                    outcome.committed = true;
                    return {outcome, kInvalidId, false};
                }
            }
            return {last, kInvalidId, false};
        }
        for (quint32 i = 0; i < statement.alternatives.count; ++i) {
            const Alternative& alternative =
                m_program.alternatives.at(statement.alternatives.first + i);
            const quint64 savedPosition = cursor.position;
            const DecodedTreeCheckpoint checkpoint = m_store->checkpoint();
            const qsizetype diagnosticCheckpoint = m_diagnostics.size();
            QVector<DecodedValueId> arguments;
            Outcome outcome =
                evalArguments(alternative.arguments, frame, cursor, &arguments);
            if (!outcome.succeeded()) {
                return {outcome, kInvalidId, false};
            }
            const DecodedNode savedNode =
                node != kInvalidId ? m_store->nodes.at(node) : DecodedNode{};
            m_probeAnchors.push_back({cursor.input, cursor.position, false});
            DecodedValueId value = kInvalidId;
            outcome = decodeType(alternative.type, arguments, frame, cursor,
                                 node, &value, alternative.sourceSpan);
            const bool probeCommitted = m_probeAnchors.last().committed;
            m_probeAnchors.pop_back();
            if (outcome.succeeded()) {
                if (node != kInvalidId) {
                    m_store->nodes[node].value = value;
                    setCompositeLayout(node, statement.type, cursor.input, start,
                                       cursor.position);
                }
                return {success(), value, true};
            }
            last = outcome;
            cursor.position = savedPosition;
            m_store->rollback(checkpoint);
            if (node != kInvalidId) {
                m_store->nodes[node] = savedNode;
            }
            if (outcome.committed || probeCommitted || outcome.fatal) {
                outcome.committed = true;
                return {outcome, kInvalidId, false};
            }
            m_diagnostics.resize(diagnosticCheckpoint);
        }
        return {last, kInvalidId, false};
    }

    StatementResult execRecover(const Statement& statement, Frame& frame,
                                Cursor& cursor, DecodedNodeId parent) {
        DecodedNodeId node = kInvalidId;
        if (!createNode(DecodedNodeKind::Sequence, parent,
                        m_program.symbol(statement.name), statement.type,
                        statement.sourceSpan, cursor.input, &node)) {
            return {failure(QStringLiteral("BRR0100"),
                            QStringLiteral("Decoded node limit exceeded"),
                            statement.sourceSpan, cursor),
                    kInvalidId, false};
        }
        const quint64 start = cursor.position;
        const TypeId itemType = m_program.types.at(statement.type).elementType;
        quint64 maxProbe = m_program.limits.maxProbeBytes;
        if (statement.secondaryExpression != kInvalidId) {
            const EvalResult evaluated =
                eval(statement.secondaryExpression, frame, cursor);
            if (!evaluated.outcome.succeeded()) {
                return {evaluated.outcome, kInvalidId, false};
            }
            maxProbe = asUnsigned(evaluated.value).value_or(maxProbe);
        }
        QVector<DecodedValueId> items;
        if (!streamName(statement.name) ||
            (emitting() && !m_writer->beginArray())) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        for (quint64 iteration = 0;; ++iteration) {
            if (isAtEnd(cursor)) {
                break;
            }
            if (iteration >= m_program.limits.maxLoopIterations) {
                return {failure(QStringLiteral("BRR0251"),
                                QStringLiteral("Loop iteration limit exceeded"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
            const quint64 attemptStart = cursor.position;
            const DecodedTreeCheckpoint checkpoint = m_store->checkpoint();
            const qsizetype diagnosticCheckpoint = m_diagnostics.size();
            DecodedNodeId itemNode = kInvalidId;
            if (!createNode(DecodedNodeKind::SequenceItem, node,
                            QStringLiteral("[%1]").arg(items.size()), itemType,
                            statement.sourceSpan, cursor.input, &itemNode)) {
                return {failure(QStringLiteral("BRR0100"),
                                QStringLiteral("Decoded node limit exceeded"),
                                statement.sourceSpan, cursor),
                        kInvalidId, false};
            }
            QVector<DecodedValueId> arguments;
            Outcome argumentOutcome =
                evalArguments(statement.arguments, frame, cursor, &arguments);
            if (!argumentOutcome.succeeded()) {
                return {argumentOutcome, kInvalidId, false};
            }
            m_probeAnchors.push_back({cursor.input, cursor.position, false});
            DecodedValueId item = kInvalidId;
            if (streaming()) {
                ++m_streamSuppressionDepth;
            }
            Outcome attempt = decodeType(itemType, arguments, frame, cursor,
                                         itemNode, &item, statement.sourceSpan);
            if (streaming()) {
                --m_streamSuppressionDepth;
            }
            m_probeAnchors.pop_back();
            if (streaming() && attempt.succeeded()) {
                cursor.position = attemptStart;
                m_store->rollback(checkpoint);
                m_diagnostics.resize(diagnosticCheckpoint);
                arguments.clear();
                argumentOutcome =
                    evalArguments(statement.arguments, frame, cursor, &arguments);
                if (!argumentOutcome.succeeded()) {
                    return {argumentOutcome, kInvalidId, false};
                }
                m_probeAnchors.push_back({cursor.input, cursor.position, false});
                attempt = decodeType(itemType, arguments, frame, cursor,
                                     itemNode, &item, statement.sourceSpan);
                m_probeAnchors.pop_back();
            }
            if (attempt.succeeded()) {
                if (cursor.position == attemptStart) {
                    return {failure(QStringLiteral("BRR0290"),
                                    QStringLiteral("Recovered item made no input progress"),
                                    statement.sourceSpan, cursor),
                            kInvalidId, false};
                }
                items.push_back(item);
                releaseProbePrefix(cursor);
                continue;
            }
            if (attempt.fatal) {
                return {attempt, kInvalidId, false};
            }
            cursor.position = attemptStart;
            m_store->rollback(checkpoint);
            m_diagnostics.resize(diagnosticCheckpoint);

            const auto candidate = findSync(statement, cursor, maxProbe);
            const quint64 end = cursorEnd(cursor).value_or(cursor.position);
            if (!candidate.has_value()) {
                const quint64 probeEnd = qMin(
                    end, addWouldOverflow(attemptStart, maxProbe)
                             ? std::numeric_limits<quint64>::max()
                             : attemptStart + maxProbe);
                if (probeEnd > attemptStart) {
                    Outcome gap = emitGap(statement, node, cursor, attemptStart,
                                          probeEnd - attemptStart);
                    if (!gap.succeeded()) {
                        return {gap, kInvalidId, false};
                    }
                    cursor.position = probeEnd;
                }
                if (cursor.position < end) {
                    return {failure(QStringLiteral("BRR0291"),
                                    QStringLiteral("Recovery probe limit exceeded"),
                                    statement.sourceSpan, cursor),
                            kInvalidId, false};
                }
                break;
            }
            if (*candidate > attemptStart) {
                Outcome gap = emitGap(statement, node, cursor, attemptStart,
                                      *candidate - attemptStart);
                if (!gap.succeeded()) {
                    return {gap, kInvalidId, false};
                }
            }
            cursor.position = *candidate;
            releaseProbePrefix(cursor);
        }
        if (emitting() && !m_writer->endArray()) {
            return {writerFailure(statement.sourceSpan, cursor), kInvalidId,
                    false};
        }
        const DecodedValueId sequence = addSequence(statement.type, items, node);
        if (node != kInvalidId) {
            m_store->nodes[node].value = sequence;
            setCompositeLayout(node, statement.type, cursor.input, start,
                               cursor.position);
        }
        return {success(), sequence, true};
    }

    Outcome emitGap(const Statement& statement, DecodedNodeId parent,
                    const Cursor& cursor, quint64 start, quint64 length) {
        DecodedNodeId gap = kInvalidId;
        const QString name = statement.gapsName != kInvalidId
                                 ? m_program.symbol(statement.gapsName)
                                 : QStringLiteral("gap");
        if (!createNode(DecodedNodeKind::Gap, parent, name, bytesType(),
                        statement.sourceSpan, cursor.input, &gap)) {
            return failure(QStringLiteral("BRR0100"),
                           QStringLiteral("Decoded node limit exceeded"),
                           statement.sourceSpan, cursor);
        }
        const DecodedValueId value =
            addSourceBytes(bytesType(), cursor.input, start, length, gap);
        if (emitting()) {
            const QString inputRole =
                cursor.input < static_cast<InputId>(m_program.inputs.size())
                    ? m_program.symbol(m_program.inputs.at(cursor.input).name)
                    : QString();
            if (!m_writer->beginObject() ||
                !m_writer->name(QStringLiteral("@gap")) ||
                !m_writer->string(name) ||
                !m_writer->name(QStringLiteral("@input")) ||
                !m_writer->string(inputRole) ||
                !m_writer->name(QStringLiteral("@offset")) ||
                !m_writer->unsignedInteger(
                    source(cursor.input)->absoluteOffset(start)) ||
                !m_writer->name(QStringLiteral("@length")) ||
                !m_writer->unsignedInteger(length) ||
                !m_writer->name(QStringLiteral("bytes")) ||
                !m_writer->sourceBytesHex(source(cursor.input), start, length) ||
                !m_writer->endObject()) {
                return writerFailure(statement.sourceSpan, cursor);
            }
        }
        if (gap != kInvalidId) {
            m_store->nodes[gap].value = value;
            setNodeSource(gap, bytesType(), cursor.input, start, length);
        }
        return success();
    }

    std::optional<quint64> findSync(const Statement& statement,
                                    Cursor cursor,
                                    quint64 maxProbe) {
        const auto knownEnd = cursorEnd(cursor);
        if (!knownEnd.has_value()) {
            return std::nullopt;
        }
        quint64 first = cursor.position;
        if (addWouldOverflow(first, statement.stepBytes)) {
            return std::nullopt;
        }
        first += statement.stepBytes;
        const quint64 limit = qMin(
            *knownEnd, addWouldOverflow(cursor.position, maxProbe)
                           ? std::numeric_limits<quint64>::max()
                           : cursor.position + maxProbe);
        for (quint64 candidate = first; candidate < limit;) {
            for (quint32 i = 0; i < statement.bytePatterns.count; ++i) {
                const quint32 patternId = m_program.bytePatternRefs.at(
                    statement.bytePatterns.first + i);
                const QByteArray& pattern = m_program.bytePatterns.at(patternId).bytes;
                if (static_cast<quint64>(pattern.size()) > limit - candidate) {
                    continue;
                }
                const ByteReadResult read =
                    source(cursor.input)->read(candidate, pattern.size());
                if (read.ok() && read.view.data() != nullptr &&
                    std::memcmp(read.view.data(), pattern.constData(),
                                static_cast<size_t>(pattern.size())) == 0) {
                    return candidate;
                }
            }
            if (addWouldOverflow(candidate, statement.stepBytes)) {
                break;
            }
            candidate += statement.stepBytes;
        }
        return std::nullopt;
    }

    StatementResult execLoopControl(const Statement& statement, Frame& frame,
                                    Cursor& cursor) {
        bool take = true;
        if (statement.condition != kInvalidId) {
            const EvalResult condition = eval(statement.condition, frame, cursor);
            if (!condition.outcome.succeeded()) {
                return {condition.outcome, kInvalidId, false};
            }
            take = asBoolean(condition.value).value_or(false);
        }
        if (!take) {
            return {};
        }
        Outcome outcome;
        outcome.flow = statement.kind == StatementKind::Break ? Flow::Break
                                                               : Flow::Continue;
        return {outcome, kInvalidId, false};
    }

    Outcome decodeType(TypeId type, const QVector<DecodedValueId>& arguments,
                       Frame& caller, Cursor& cursor, DecodedNodeId node,
                       DecodedValueId* value, SourceSpanId span) {
        if (type >= static_cast<TypeId>(m_program.types.size())) {
            return failure(QStringLiteral("BRR0300"),
                           QStringLiteral("Invalid decoded type"), span, cursor);
        }
        const TypeDesc& descriptor = m_program.types.at(type);
        if (descriptor.kind == TypeKind::Enum) {
            Outcome outcome =
                decodePrimitive(descriptor.elementType, cursor, node, value, span);
            if (outcome.succeeded()) {
                m_store->values[*value].type = type;
                if (node != kInvalidId) {
                    m_store->nodes[node].type = type;
                }
            }
            return outcome;
        }
        if (descriptor.kind == TypeKind::UnsignedInteger ||
            descriptor.kind == TypeKind::SignedInteger ||
            descriptor.kind == TypeKind::FloatingPoint) {
            return decodePrimitive(type, cursor, node, value, span);
        }
        if (descriptor.kind != TypeKind::Record) {
            return failure(QStringLiteral("BRR0301"),
                           QStringLiteral("Type cannot be decoded directly"), span,
                           cursor);
        }
        const RecordDesc* record = nullptr;
        for (const RecordDesc& candidate : m_program.records) {
            if (candidate.type == type) {
                record = &candidate;
                break;
            }
        }
        if (record == nullptr) {
            return failure(QStringLiteral("BRR0302"),
                           QStringLiteral("Missing record descriptor"), span,
                           cursor);
        }
        if (m_depth >= m_program.limits.maxParseDepth) {
            return failure(QStringLiteral("BRR0303"),
                           QStringLiteral("Parse depth limit exceeded"), span,
                           cursor);
        }
        ++m_depth;
        Frame frame;
        frame.values.resize(record->slotCount, kInvalidId);
        const quint32 parameterCount = qMin(record->parameters.count,
                                            static_cast<quint32>(arguments.size()));
        for (quint32 i = 0; i < parameterCount; ++i) {
            const ParameterDesc& parameter =
                m_program.parameters.at(record->parameters.first + i);
            if (parameter.slot < static_cast<quint32>(frame.values.size())) {
                frame.values[parameter.slot] = coerce(arguments.at(i), parameter.type);
            }
        }
        const quint64 start = cursor.position;
        if (emitting() && !m_writer->beginObject()) {
            --m_depth;
            return writerFailure(span, cursor);
        }
        Outcome outcome = execBlock(record->statements, frame, cursor, node);
        outcome.committed = outcome.committed || frame.committed;
        if (outcome.succeeded()) {
            if (emitting() && !m_writer->endObject()) {
                --m_depth;
                return writerFailure(span, cursor);
            }
            *value = buildObject(type, frame, node);
            if (node != kInvalidId) {
                m_store->nodes[node].kind = DecodedNodeKind::Record;
                m_store->nodes[node].value = *value;
                setCompositeLayout(node, type, cursor.input, start,
                                   cursor.position);
            }
        }
        --m_depth;
        Q_UNUSED(caller);
        return outcome;
    }

    Outcome decodePrimitive(TypeId type, Cursor& cursor, DecodedNodeId node,
                            DecodedValueId* value, SourceSpanId span) {
        if (type >= static_cast<TypeId>(m_program.types.size())) {
            return failure(QStringLiteral("BRR0310"),
                           QStringLiteral("Invalid primitive type"), span, cursor);
        }
        const TypeDesc& descriptor = m_program.types.at(type);
        if (descriptor.bitWidth == 0 || descriptor.bitWidth % 8 != 0 ||
            descriptor.bitWidth > 64) {
            return failure(QStringLiteral("BRR0311"),
                           QStringLiteral("Unsupported primitive width"), span,
                           cursor);
        }
        if (descriptor.bitWidth > 8 && descriptor.endianness == Endianness::None) {
            return failure(QStringLiteral("BRR0312"),
                           QStringLiteral("Multi-byte decoded fields require declared endianness"),
                           span, cursor);
        }
        const qsizetype bytes = descriptor.bitWidth / 8;
        if (cursor.end.has_value() &&
            (cursor.position > *cursor.end ||
             static_cast<quint64>(bytes) > *cursor.end - cursor.position)) {
            return failure(QStringLiteral("BRR0313"),
                           QStringLiteral("Unexpected end of region"), span, cursor);
        }
        for (const ProbeAnchor& anchor : std::as_const(m_probeAnchors)) {
            if (!anchor.committed && anchor.input == cursor.input &&
                cursor.position >= anchor.position &&
                static_cast<quint64>(bytes) >
                    m_program.limits.maxProbeBytes -
                        qMin(m_program.limits.maxProbeBytes,
                             cursor.position - anchor.position)) {
                return failure(QStringLiteral("BRR0314"),
                               QStringLiteral("Speculative decode probe limit exceeded"),
                               span, cursor);
            }
        }
        const quint64 start = cursor.position;
        ByteReadResult read = source(cursor.input)->read(cursor.position, bytes);
        if (!read.ok()) {
            return failure(read.status == ByteReadStatus::EndOfInput
                               ? QStringLiteral("BRR0313")
                               : QStringLiteral("BRR0315"),
                           read.status == ByteReadStatus::EndOfInput
                               ? QStringLiteral("Unexpected end of input")
                               : read.error,
                           span, cursor);
        }
        quint64 raw = 0;
        const uchar* data = reinterpret_cast<const uchar*>(read.view.data());
        if (descriptor.endianness == Endianness::Big) {
            for (qsizetype i = 0; i < bytes; ++i) {
                raw = (raw << 8) | data[i];
            }
        } else {
            for (qsizetype i = bytes; i > 0; --i) {
                raw = (raw << 8) | data[i - 1];
            }
        }
        cursor.position += static_cast<quint64>(bytes);
        if (descriptor.kind == TypeKind::FloatingPoint) {
            if (descriptor.bitWidth == 32) {
                const quint32 bits = static_cast<quint32>(raw);
                *value = addFloat(type, static_cast<double>(std::bit_cast<float>(bits)),
                                  node);
            } else {
                *value = addFloat(type, std::bit_cast<double>(raw), node);
            }
        } else if (descriptor.kind == TypeKind::SignedInteger) {
            qint64 signedValue = 0;
            if (descriptor.bitWidth == 64) {
                signedValue = std::bit_cast<qint64>(raw);
            } else {
                const quint64 sign = quint64{1} << (descriptor.bitWidth - 1);
                const quint64 mask = (quint64{1} << descriptor.bitWidth) - 1;
                signedValue = static_cast<qint64>(
                    (raw & sign) ? (raw | ~mask) : raw);
            }
            *value = addSigned(type, signedValue, node);
        } else {
            *value = addUnsigned(type, raw, node);
        }
        if (node != kInvalidId) {
            m_store->nodes[node].value = *value;
            setNodeSource(node, type, cursor.input, start,
                          static_cast<quint64>(bytes));
        }
        if (!streamValue(*value)) {
            return writerFailure(span, cursor);
        }
        return success();
    }

    Outcome evalArguments(IdRange range, Frame& frame, Cursor& cursor,
                          QVector<DecodedValueId>* values) {
        for (quint32 i = 0; i < range.count; ++i) {
            const ExpressionId id =
                m_program.expressionRefs.at(range.first + i);
            const EvalResult evaluated = eval(id, frame, cursor);
            if (!evaluated.outcome.succeeded()) {
                return evaluated.outcome;
            }
            values->push_back(evaluated.value);
        }
        return success();
    }

    EvalResult eval(ExpressionId id, Frame& frame, Cursor& cursor) {
        if (id >= static_cast<ExpressionId>(m_program.expressions.size())) {
            return {failure(QStringLiteral("BRR0400"),
                            QStringLiteral("Invalid resolved expression"),
                            kInvalidId, cursor),
                    kInvalidId};
        }
        const Expression& expression = m_program.expressions.at(id);
        switch (expression.kind) {
            case ExpressionKind::UnsignedInteger:
            case ExpressionKind::Constant:
            case ExpressionKind::EnumValue:
                return {success(), addUnsigned(expression.type,
                                               expression.unsignedValue)};
            case ExpressionKind::FloatingPoint:
                return {success(), addFloat(expression.type,
                                            expression.floatingValue)};
            case ExpressionKind::String:
                return {success(), addString(expression.type,
                                             m_program.symbol(expression.symbol))};
            case ExpressionKind::Boolean:
                return {success(), addBoolean(expression.type,
                                              expression.booleanValue)};
            case ExpressionKind::Slot:
                return evalSlot(expression, frame, cursor);
            case ExpressionKind::Member:
                return evalMember(expression, frame, cursor);
            case ExpressionKind::MetadataMember:
                return evalMetadata(expression, frame, cursor);
            case ExpressionKind::Unary:
                return evalUnary(expression, frame, cursor);
            case ExpressionKind::Binary:
                return evalBinary(expression, frame, cursor);
            case ExpressionKind::Call:
                return evalCall(expression, frame, cursor);
            case ExpressionKind::ByteArray:
                return evalByteArray(expression, frame, cursor);
            case ExpressionKind::InterpolatedString:
                return evalInterpolated(expression, frame, cursor);
            case ExpressionKind::TypeName:
            case ExpressionKind::Invalid:
                break;
        }
        return {failure(QStringLiteral("BRR0401"),
                        QStringLiteral("Expression cannot be evaluated"),
                        expression.sourceSpan, cursor),
                kInvalidId};
    }

    EvalResult evalSlot(const Expression& expression, Frame& frame,
                        Cursor& cursor) {
        if (expression.slot != kInvalidId) {
            if (expression.slot < static_cast<quint32>(frame.values.size()) &&
                frame.values.at(expression.slot) != kInvalidId) {
                return {success(), frame.values.at(expression.slot)};
            }
            return {failure(QStringLiteral("BRR0410"),
                            QStringLiteral("Value '%1' is not available")
                                .arg(m_program.symbol(expression.symbol)),
                            expression.sourceSpan, cursor),
                    kInvalidId};
        }
        const QString name = m_program.symbol(expression.symbol);
        if (name == QStringLiteral("remaining")) {
            const auto end = cursorEnd(cursor);
            if (!end.has_value() || *end < cursor.position) {
                return {failure(QStringLiteral("BRR0411"),
                                QStringLiteral("Could not determine remaining bytes"),
                                expression.sourceSpan, cursor),
                        kInvalidId};
            }
            return {success(), addUnsigned(expression.type,
                                           *end - cursor.position)};
        }
        if (name == QStringLiteral("at_end")) {
            return {success(), addBoolean(expression.type, isAtEnd(cursor))};
        }
        return {failure(QStringLiteral("BRR0412"),
                        QStringLiteral("Unbound runtime value '%1'").arg(name),
                        expression.sourceSpan, cursor),
                kInvalidId};
    }

    EvalResult evalMember(const Expression& expression, Frame& frame,
                          Cursor& cursor) {
        const ExpressionId baseId =
            m_program.expressionRefs.at(expression.operands.first);
        const EvalResult base = eval(baseId, frame, cursor);
        if (!base.outcome.succeeded()) {
            return base;
        }
        const DecodedFieldValue* field =
            m_store->findField(base.value, expression.symbol);
        if (field == nullptr) {
            return {failure(QStringLiteral("BRR0420"),
                            QStringLiteral("Decoded field '%1' is absent")
                                .arg(m_program.symbol(expression.symbol)),
                            expression.sourceSpan, cursor),
                    kInvalidId};
        }
        return {success(), field->value};
    }

    EvalResult evalMetadata(const Expression& expression, Frame& frame,
                            Cursor& cursor) {
        const EvalResult base = eval(
            m_program.expressionRefs.at(expression.operands.first), frame, cursor);
        if (!base.outcome.succeeded()) {
            return base;
        }
        if (base.value >= static_cast<DecodedValueId>(m_store->values.size())) {
            return {failure(QStringLiteral("BRR0430"),
                            QStringLiteral("Metadata base is invalid"),
                            expression.sourceSpan, cursor),
                    kInvalidId};
        }
        const DecodedValue& value = m_store->values.at(base.value);
        const DecodedNode* node =
            value.node != kInvalidId &&
                    value.node < static_cast<DecodedNodeId>(m_store->nodes.size())
                ? &m_store->nodes.at(value.node)
                : nullptr;
        const QString member = m_program.symbol(expression.symbol);
        if (member == QStringLiteral("valid")) {
            return {success(), addBoolean(expression.type,
                                          node == nullptr || node->valid)};
        }
        if (member == QStringLiteral("offset") ||
            member == QStringLiteral("length")) {
            const quint64 number = node != nullptr && node->hasSourceSpan
                                       ? (member == QStringLiteral("offset")
                                              ? node->offset
                                              : node->length)
                                       : 0;
            return {success(), addUnsigned(expression.type, number)};
        }
        if (member == QStringLiteral("name")) {
            return {success(), addString(expression.type,
                                         node != nullptr
                                             ? m_store->name(node->name)
                                             : QString())};
        }
        if (member == QStringLiteral("type")) {
            const TypeId type = node != nullptr ? node->type : value.type;
            return {success(), addString(expression.type, typeName(type))};
        }
        if (member == QStringLiteral("input")) {
            const InputId input = node != nullptr ? node->input : kInvalidId;
            return {success(), addString(
                                   expression.type,
                                   input < static_cast<InputId>(m_program.inputs.size())
                                       ? m_program.symbol(m_program.inputs.at(input).name)
                                       : QString())};
        }
        if (member == QStringLiteral("path")) {
            const InputId input = node != nullptr ? node->input : kInvalidId;
            return {success(), addString(expression.type,
                                         source(input) != nullptr
                                             ? source(input)->path()
                                             : QString())};
        }
        if (member == QStringLiteral("bytes") && node != nullptr &&
            node->hasSourceSpan) {
            const quint64 logical = logicalOffset(node->input, node->offset);
            return {success(), addSourceBytes(expression.type, node->input,
                                              logical, node->length)};
        }
        if (member == QStringLiteral("value")) {
            return {success(), base.value};
        }
        return {failure(QStringLiteral("BRR0431"),
                        QStringLiteral("Metadata '@%1' is unavailable")
                            .arg(member),
                        expression.sourceSpan, cursor),
                kInvalidId};
    }

    EvalResult evalUnary(const Expression& expression, Frame& frame,
                         Cursor& cursor) {
        const EvalResult operand = eval(
            m_program.expressionRefs.at(expression.operands.first), frame, cursor);
        if (!operand.outcome.succeeded()) {
            return operand;
        }
        if (expression.unaryOperator == SyntaxUnaryOperator::LogicalNot) {
            return {success(), addBoolean(expression.type,
                                          !asBoolean(operand.value).value_or(false))};
        }
        const DecodedValue& value = m_store->values.at(operand.value);
        if (value.kind == DecodedValueKind::FloatingPoint) {
            return {success(), addFloat(expression.type, -value.floatingValue)};
        }
        if (value.kind == DecodedValueKind::SignedInteger) {
            return {success(), addSigned(expression.type, -value.signedValue)};
        }
        return {success(), addSigned(expression.type,
                                     -static_cast<qint64>(
                                         asUnsigned(operand.value).value_or(0)))};
    }

    EvalResult evalBinary(const Expression& expression, Frame& frame,
                          Cursor& cursor) {
        const EvalResult left = eval(
            m_program.expressionRefs.at(expression.operands.first), frame, cursor);
        if (!left.outcome.succeeded()) {
            return left;
        }
        if (expression.binaryOperator == SyntaxBinaryOperator::LogicalAnd &&
            !asBoolean(left.value).value_or(false)) {
            return {success(), addBoolean(expression.type, false)};
        }
        if (expression.binaryOperator == SyntaxBinaryOperator::LogicalOr &&
            asBoolean(left.value).value_or(false)) {
            return {success(), addBoolean(expression.type, true)};
        }
        const EvalResult right = eval(
            m_program.expressionRefs.at(expression.operands.first + 1), frame,
            cursor);
        if (!right.outcome.succeeded()) {
            return right;
        }
        switch (expression.binaryOperator) {
            case SyntaxBinaryOperator::Equal:
                return {success(), addBoolean(expression.type,
                                              valuesEqual(left.value, right.value))};
            case SyntaxBinaryOperator::NotEqual:
                return {success(), addBoolean(expression.type,
                                              !valuesEqual(left.value, right.value))};
            case SyntaxBinaryOperator::LogicalAnd:
                return {success(), addBoolean(
                                       expression.type,
                                       asBoolean(left.value).value_or(false) &&
                                           asBoolean(right.value).value_or(false))};
            case SyntaxBinaryOperator::LogicalOr:
                return {success(), addBoolean(
                                       expression.type,
                                       asBoolean(left.value).value_or(false) ||
                                           asBoolean(right.value).value_or(false))};
            default:
                break;
        }
        const bool floating =
            m_store->values.at(left.value).kind == DecodedValueKind::FloatingPoint ||
            m_store->values.at(right.value).kind == DecodedValueKind::FloatingPoint;
        if (expression.binaryOperator == SyntaxBinaryOperator::Less ||
            expression.binaryOperator == SyntaxBinaryOperator::LessEqual ||
            expression.binaryOperator == SyntaxBinaryOperator::Greater ||
            expression.binaryOperator == SyntaxBinaryOperator::GreaterEqual) {
            const double a = asDouble(left.value).value_or(0.0);
            const double b = asDouble(right.value).value_or(0.0);
            bool result = false;
            if (expression.binaryOperator == SyntaxBinaryOperator::Less) result = a < b;
            if (expression.binaryOperator == SyntaxBinaryOperator::LessEqual) result = a <= b;
            if (expression.binaryOperator == SyntaxBinaryOperator::Greater) result = a > b;
            if (expression.binaryOperator == SyntaxBinaryOperator::GreaterEqual) result = a >= b;
            return {success(), addBoolean(expression.type, result)};
        }
        if (floating) {
            const double a = asDouble(left.value).value_or(0.0);
            const double b = asDouble(right.value).value_or(0.0);
            if ((expression.binaryOperator == SyntaxBinaryOperator::Divide ||
                 expression.binaryOperator == SyntaxBinaryOperator::Remainder) &&
                b == 0.0) {
                return {failure(QStringLiteral("BRR0440"),
                                QStringLiteral("Division by zero"),
                                expression.sourceSpan, cursor),
                        kInvalidId};
            }
            double result = 0.0;
            if (expression.binaryOperator == SyntaxBinaryOperator::Add) result = a + b;
            if (expression.binaryOperator == SyntaxBinaryOperator::Subtract) result = a - b;
            if (expression.binaryOperator == SyntaxBinaryOperator::Multiply) result = a * b;
            if (expression.binaryOperator == SyntaxBinaryOperator::Divide) result = a / b;
            if (expression.binaryOperator == SyntaxBinaryOperator::Remainder) result = std::fmod(a, b);
            return {success(), addFloat(expression.type, result)};
        }
        const quint64 a = asUnsigned(left.value).value_or(0);
        const quint64 b = asUnsigned(right.value).value_or(0);
        if ((expression.binaryOperator == SyntaxBinaryOperator::Divide ||
             expression.binaryOperator == SyntaxBinaryOperator::Remainder) &&
            b == 0) {
            return {failure(QStringLiteral("BRR0440"),
                            QStringLiteral("Division by zero"),
                            expression.sourceSpan, cursor),
                    kInvalidId};
        }
        quint64 result = 0;
        if (expression.binaryOperator == SyntaxBinaryOperator::Add) result = a + b;
        if (expression.binaryOperator == SyntaxBinaryOperator::Subtract) result = a - b;
        if (expression.binaryOperator == SyntaxBinaryOperator::Multiply) result = a * b;
        if (expression.binaryOperator == SyntaxBinaryOperator::Divide) result = a / b;
        if (expression.binaryOperator == SyntaxBinaryOperator::Remainder) result = a % b;
        return {success(), addUnsigned(expression.type, result)};
    }

    EvalResult evalCall(const Expression& expression, Frame& frame,
                        Cursor& cursor) {
        QVector<DecodedValueId> arguments;
        Outcome outcome =
            evalArguments(expression.operands, frame, cursor, &arguments);
        if (!outcome.succeeded()) {
            return {outcome, kInvalidId};
        }
        const DecodedValueId argument = arguments.value(0, kInvalidId);
        const QString function = m_program.symbol(expression.symbol);
        if (function == QStringLiteral("int")) {
            return {success(), addUnsigned(expression.type,
                                           asUnsigned(argument).value_or(0))};
        }
        if (function == QStringLiteral("present")) {
            return {success(), addBoolean(expression.type,
                                          argument != kInvalidId &&
                                              m_store->values.at(argument).kind !=
                                                  DecodedValueKind::Null)};
        }
        if (function == QStringLiteral("count")) {
            quint64 count = 0;
            if (argument < static_cast<DecodedValueId>(m_store->values.size()) &&
                m_store->values.at(argument).kind == DecodedValueKind::Sequence) {
                count = m_store->values.at(argument).elements.count;
            }
            return {success(), addUnsigned(expression.type, count)};
        }
        return {failure(QStringLiteral("BRR0450"),
                        QStringLiteral("Function '%1' is not available in decode mode")
                            .arg(function),
                        expression.sourceSpan, cursor),
                kInvalidId};
    }

    EvalResult evalByteArray(const Expression& expression, Frame& frame,
                             Cursor& cursor) {
        QByteArray bytes;
        for (quint32 i = 0; i < expression.operands.count; ++i) {
            const EvalResult element = eval(
                m_program.expressionRefs.at(expression.operands.first + i), frame,
                cursor);
            if (!element.outcome.succeeded()) {
                return element;
            }
            bytes.push_back(static_cast<char>(
                asUnsigned(element.value).value_or(0) & 0xff));
        }
        return {success(), addOwnedBytes(expression.type, std::move(bytes))};
    }

    EvalResult evalInterpolated(const Expression& expression, Frame& frame,
                                Cursor& cursor) {
        QString text;
        for (quint32 i = 0; i < expression.textParts.count; ++i) {
            text += m_program.symbol(
                m_program.textPartSymbols.at(expression.textParts.first + i));
            if (i < expression.operands.count) {
                const EvalResult value = eval(
                    m_program.expressionRefs.at(expression.operands.first + i),
                    frame, cursor);
                if (!value.outcome.succeeded()) {
                    return value;
                }
                text += scalarString(value.value);
            }
        }
        return {success(), addString(expression.type, std::move(text))};
    }

    std::optional<quint64> asUnsigned(DecodedValueId id) const {
        if (id >= static_cast<DecodedValueId>(m_store->values.size())) {
            return std::nullopt;
        }
        const DecodedValue& value = m_store->values.at(id);
        switch (value.kind) {
            case DecodedValueKind::UnsignedInteger: return value.unsignedValue;
            case DecodedValueKind::SignedInteger:
                return static_cast<quint64>(value.signedValue);
            case DecodedValueKind::Boolean: return value.booleanValue ? 1 : 0;
            case DecodedValueKind::FloatingPoint:
                if (value.floatingValue >= 0.0 && std::isfinite(value.floatingValue)) {
                    return static_cast<quint64>(value.floatingValue);
                }
                break;
            default: break;
        }
        return std::nullopt;
    }

    std::optional<double> asDouble(DecodedValueId id) const {
        if (id >= static_cast<DecodedValueId>(m_store->values.size())) {
            return std::nullopt;
        }
        const DecodedValue& value = m_store->values.at(id);
        if (value.kind == DecodedValueKind::FloatingPoint) return value.floatingValue;
        if (value.kind == DecodedValueKind::SignedInteger)
            return static_cast<double>(value.signedValue);
        if (value.kind == DecodedValueKind::UnsignedInteger)
            return static_cast<double>(value.unsignedValue);
        if (value.kind == DecodedValueKind::Boolean) return value.booleanValue ? 1.0 : 0.0;
        return std::nullopt;
    }

    std::optional<bool> asBoolean(DecodedValueId id) const {
        if (id >= static_cast<DecodedValueId>(m_store->values.size())) {
            return std::nullopt;
        }
        const DecodedValue& value = m_store->values.at(id);
        if (value.kind == DecodedValueKind::Boolean) return value.booleanValue;
        if (value.kind == DecodedValueKind::UnsignedInteger) return value.unsignedValue != 0;
        if (value.kind == DecodedValueKind::SignedInteger) return value.signedValue != 0;
        return std::nullopt;
    }

    bool valuesEqual(DecodedValueId left, DecodedValueId right) const {
        if (left == kInvalidId || right == kInvalidId ||
            left >= static_cast<DecodedValueId>(m_store->values.size()) ||
            right >= static_cast<DecodedValueId>(m_store->values.size())) {
            return false;
        }
        const DecodedValue& a = m_store->values.at(left);
        const DecodedValue& b = m_store->values.at(right);
        if ((a.kind == DecodedValueKind::UnsignedInteger ||
             a.kind == DecodedValueKind::SignedInteger ||
             a.kind == DecodedValueKind::FloatingPoint ||
             a.kind == DecodedValueKind::Boolean) &&
            (b.kind == DecodedValueKind::UnsignedInteger ||
             b.kind == DecodedValueKind::SignedInteger ||
             b.kind == DecodedValueKind::FloatingPoint ||
             b.kind == DecodedValueKind::Boolean)) {
            return asDouble(left).value_or(0.0) == asDouble(right).value_or(0.0);
        }
        if (a.kind == DecodedValueKind::String &&
            b.kind == DecodedValueKind::String) {
            return m_store->valueStrings.value(a.payload) ==
                   m_store->valueStrings.value(b.payload);
        }
        return left == right;
    }

    QString scalarString(DecodedValueId id) const {
        if (id >= static_cast<DecodedValueId>(m_store->values.size())) {
            return {};
        }
        const DecodedValue& value = m_store->values.at(id);
        if (value.kind == DecodedValueKind::String)
            return m_store->valueStrings.value(value.payload);
        if (value.kind == DecodedValueKind::Boolean)
            return value.booleanValue ? QStringLiteral("true") : QStringLiteral("false");
        if (value.kind == DecodedValueKind::SignedInteger)
            return QString::number(value.signedValue);
        if (value.kind == DecodedValueKind::UnsignedInteger)
            return QString::number(value.unsignedValue);
        if (value.kind == DecodedValueKind::FloatingPoint)
            return QString::number(value.floatingValue, 'g', 15);
        return {};
    }

    DecodedValueId coerce(DecodedValueId id, TypeId target) {
        if (id >= static_cast<DecodedValueId>(m_store->values.size()) ||
            target >= static_cast<TypeId>(m_program.types.size())) {
            return id;
        }
        const DecodedValue& value = m_store->values.at(id);
        const TypeKind kind = m_program.types.at(target).kind;
        if (value.type == target) return id;
        if (kind == TypeKind::FloatingPoint)
            return addFloat(target, asDouble(id).value_or(0.0), value.node);
        if (kind == TypeKind::SignedInteger)
            return addSigned(target, static_cast<qint64>(asUnsigned(id).value_or(0)),
                             value.node);
        if (kind == TypeKind::UnsignedInteger || kind == TypeKind::Enum)
            return addUnsigned(target, asUnsigned(id).value_or(0), value.node);
        return id;
    }

    std::optional<quint64> cursorEnd(Cursor& cursor) {
        if (cursor.end.has_value()) {
            return cursor.end;
        }
        ByteSource* bytes = source(cursor.input);
        if (bytes == nullptr) {
            return std::nullopt;
        }
        if (const auto known = bytes->size(); known.has_value()) {
            cursor.end = known;
            return known;
        }
        quint64 probe = cursor.position;
        constexpr qsizetype kChunk = 64 * 1024;
        for (;;) {
            const ByteReadResult read = bytes->read(probe, kChunk);
            if (read.status == ByteReadStatus::Error) {
                return std::nullopt;
            }
            if (read.status == ByteReadStatus::EndOfInput) {
                const auto known = bytes->size();
                if (known.has_value()) {
                    cursor.end = known;
                }
                return known;
            }
            probe += static_cast<quint64>(kChunk);
        }
    }

    bool isAtEnd(Cursor& cursor) {
        if (cursor.end.has_value()) {
            return cursor.position >= *cursor.end;
        }
        ByteSource* bytes = source(cursor.input);
        const ByteReadResult read = bytes->readByte(cursor.position);
        if (read.status == ByteReadStatus::EndOfInput) {
            cursor.end = bytes->size().value_or(cursor.position);
            return true;
        }
        return false;
    }

    quint64 logicalOffset(InputId input, quint64 absolute) const {
        ByteSource* bytes = source(input);
        if (bytes == nullptr) return absolute;
        const quint64 base = bytes->absoluteOffset(0);
        return absolute >= base ? absolute - base : 0;
    }

    QString typeName(TypeId type) const {
        if (type >= static_cast<TypeId>(m_program.types.size())) return {};
        const TypeDesc& descriptor = m_program.types.at(type);
        if (descriptor.name != kInvalidId) return m_program.symbol(descriptor.name);
        if (descriptor.kind == TypeKind::Sequence)
            return QStringLiteral("sequence<%1>").arg(typeName(descriptor.elementType));
        return QStringLiteral("type#%1").arg(type);
    }

    TypeId bytesType() const {
        for (TypeId id = 0; id < static_cast<TypeId>(m_program.types.size()); ++id) {
            if (m_program.types.at(id).kind == TypeKind::Bytes) return id;
        }
        return kInvalidId;
    }

    TypeId expressionSlotType(quint32 slot) const {
        for (const Expression& expression : m_program.expressions) {
            if (expression.kind == ExpressionKind::Slot && expression.slot == slot) {
                return expression.type;
            }
        }
        return kInvalidId;
    }

    DecodedNodeKind nodeKindForType(TypeId type) const {
        if (type >= static_cast<TypeId>(m_program.types.size()))
            return DecodedNodeKind::Field;
        const TypeKind kind = m_program.types.at(type).kind;
        if (kind == TypeKind::Record) return DecodedNodeKind::Record;
        if (kind == TypeKind::Sequence) return DecodedNodeKind::Sequence;
        return DecodedNodeKind::Field;
    }

    void releaseProbePrefix(const Cursor& cursor) {
        if ((m_request.mode == DecodeMode::Probe ||
             m_request.mode == DecodeMode::Streaming) &&
            m_probeAnchors.isEmpty() && m_streamSuppressionDepth == 0) {
            if (ByteSource* bytes = source(cursor.input); bytes != nullptr) {
                bytes->releaseBefore(cursor.position);
            }
        }
    }

    const DecodeRequest& m_request;
    const BrecoProgram& m_program;
    std::shared_ptr<DecodedTree> m_store;
    std::unique_ptr<JsonWriter> m_writer;
    QVector<RuntimeDiagnostic> m_diagnostics;
    QVector<ProbeAnchor> m_probeAnchors;
    quint32 m_streamSuppressionDepth = 0;
    quint32 m_streamLoopReplayDepth = 0;
    quint32 m_depth = 0;
};

}  // namespace

DecodeResult decodeBrecoProgram(const DecodeRequest& request) {
    if (!request.program) {
        DecodeResult result;
        result.diagnostics.push_back(
            {DiagnosticSeverity::Error, QStringLiteral("BRR0001"),
             QStringLiteral("No compiled BrecoLang program was provided"), {},
             {}, false});
        return result;
    }
    return Interpreter(request).run();
}

}  // namespace breco::lang

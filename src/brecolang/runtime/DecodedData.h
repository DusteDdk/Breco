#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include "brecolang/ir/BrecoProgram.h"
#include "brecolang/runtime/DecodeTypes.h"

namespace breco::lang {

using DecodedNodeId = quint32;
using DecodedValueId = quint32;
using DecodedNameId = quint32;

enum class DecodedValueKind {
    Invalid,
    Null,
    Boolean,
    UnsignedInteger,
    SignedInteger,
    FloatingPoint,
    String,
    SourceBytes,
    OwnedBytes,
    Object,
    Sequence,
    Reference,
};

struct ByteSpanValue {
    InputId input = kInvalidId;
    quint64 offset = 0;
    quint64 length = 0;
};

struct DecodedFieldValue {
    SymbolId name = kInvalidId;
    DecodedValueId value = kInvalidId;
};

struct DecodedValue {
    DecodedValueKind kind = DecodedValueKind::Invalid;
    TypeId type = kInvalidId;
    quint64 unsignedValue = 0;
    qint64 signedValue = 0;
    double floatingValue = 0.0;
    bool booleanValue = false;
    quint32 payload = kInvalidId;
    IdRange fields;
    IdRange elements;
    quint64 logicalCount = 0;
    DecodedNodeId node = kInvalidId;
};

enum class StorageLayoutKind {
    None,
    SourceSlice,
    Composite,
    Computed,
    BitSlice,
};

struct StorageLayout {
    StorageLayoutKind kind = StorageLayoutKind::None;
    IdRange spans;
    TypeId declaredType = kInvalidId;
    Endianness endianness = Endianness::None;
    quint16 bitWidth = 0;
    quint8 highBit = 0;
    quint8 lowBit = 0;
};

enum class DecodedNodeKind {
    Entry,
    Field,
    Computed,
    Record,
    Region,
    Sequence,
    SequenceItem,
    Select,
    Alternative,
    Raw,
    Preserve,
    Bitfield,
    BitMember,
    Gap,
    Reference,
};

struct DecodedNode {
    DecodedNodeKind kind = DecodedNodeKind::Field;
    DecodedNodeId parent = kInvalidId;
    DecodedNodeId firstChild = kInvalidId;
    DecodedNodeId nextSibling = kInvalidId;
    quint32 rowInParent = 0;
    DecodedNameId name = kInvalidId;
    DecodedNameId error = kInvalidId;
    TypeId type = kInvalidId;
    DecodedValueId value = kInvalidId;
    quint32 storageLayout = kInvalidId;
    SourceSpanId schemaSpan = kInvalidId;
    InputId input = kInvalidId;
    quint64 offset = 0;
    quint64 length = 0;
    bool hasSourceSpan = false;
    bool valid = true;
};

struct DecodedTreeCheckpoint {
    qsizetype nodes = 0;
    qsizetype values = 0;
    qsizetype fieldValues = 0;
    qsizetype valueRefs = 0;
    qsizetype spans = 0;
    qsizetype layouts = 0;
    qsizetype valueStrings = 0;
    qsizetype ownedBytes = 0;
    qsizetype locators = 0;
    qsizetype references = 0;
};

class DecodedTree {
public:
    QVector<DecodedNode> nodes;
    QVector<DecodedValue> values;
    QVector<DecodedFieldValue> fieldValues;
    QVector<DecodedValueId> valueRefs;
    QVector<ByteSpanValue> spans;
    QVector<StorageLayout> storageLayouts;
    QVector<QString> names;
    QVector<QString> valueStrings;
    QVector<QByteArray> ownedBytes;
    QVector<MaterializationLocator> locators;
    QVector<ReferenceHandle> references;

    DecodedNameId internName(const QString& name);
    const QString& name(DecodedNameId id) const;
    DecodedNodeId addNode(DecodedNode node);
    DecodedValueId addValue(DecodedValue value);
    quint32 addSpan(ByteSpanValue span);
    quint32 addLayout(StorageLayout layout);
    quint32 addValueString(QString value);
    quint32 addOwnedBytes(QByteArray value);
    IdRange appendFieldValues(const QVector<DecodedFieldValue>& fields);
    IdRange appendValueRefs(const QVector<DecodedValueId>& valuesToAppend);
    IdRange appendSpans(const QVector<ByteSpanValue>& spansToAppend);

    DecodedTreeCheckpoint checkpoint() const;
    void rollback(const DecodedTreeCheckpoint& checkpoint);
    void finalizeTopology();

    const DecodedFieldValue* findField(DecodedValueId object,
                                       SymbolId name) const;
    QString displayValue(DecodedValueId value,
                         const BrecoProgram& program) const;

private:
    QHash<QString, DecodedNameId> m_nameIds;
};

}  // namespace breco::lang

#include "brecolang/runtime/DecodedData.h"

#include <QStringList>

#include <cmath>

namespace breco::lang {

DecodedNameId DecodedTree::internName(const QString& text) {
    const auto found = m_nameIds.constFind(text);
    if (found != m_nameIds.constEnd()) {
        return *found;
    }
    const DecodedNameId id = static_cast<DecodedNameId>(names.size());
    names.push_back(text);
    m_nameIds.insert(text, id);
    return id;
}

const QString& DecodedTree::name(DecodedNameId id) const {
    static const QString empty;
    return id < static_cast<DecodedNameId>(names.size()) ? names.at(id) : empty;
}

DecodedNodeId DecodedTree::addNode(DecodedNode node) {
    const DecodedNodeId id = static_cast<DecodedNodeId>(nodes.size());
    nodes.push_back(std::move(node));
    return id;
}

DecodedValueId DecodedTree::addValue(DecodedValue value) {
    const DecodedValueId id = static_cast<DecodedValueId>(values.size());
    values.push_back(std::move(value));
    return id;
}

quint32 DecodedTree::addSpan(ByteSpanValue span) {
    const quint32 id = static_cast<quint32>(spans.size());
    spans.push_back(span);
    return id;
}

quint32 DecodedTree::addLayout(StorageLayout layout) {
    const quint32 id = static_cast<quint32>(storageLayouts.size());
    storageLayouts.push_back(std::move(layout));
    return id;
}

quint32 DecodedTree::addValueString(QString value) {
    const quint32 id = static_cast<quint32>(valueStrings.size());
    valueStrings.push_back(std::move(value));
    return id;
}

quint32 DecodedTree::addOwnedBytes(QByteArray value) {
    const quint32 id = static_cast<quint32>(ownedBytes.size());
    ownedBytes.push_back(std::move(value));
    return id;
}

IdRange DecodedTree::appendFieldValues(
    const QVector<DecodedFieldValue>& fieldsToAppend) {
    const IdRange range{static_cast<quint32>(fieldValues.size()),
                        static_cast<quint32>(fieldsToAppend.size())};
    fieldValues += fieldsToAppend;
    return range;
}

IdRange DecodedTree::appendValueRefs(
    const QVector<DecodedValueId>& valuesToAppend) {
    const IdRange range{static_cast<quint32>(valueRefs.size()),
                        static_cast<quint32>(valuesToAppend.size())};
    valueRefs += valuesToAppend;
    return range;
}

IdRange DecodedTree::appendSpans(
    const QVector<ByteSpanValue>& spansToAppend) {
    const IdRange range{static_cast<quint32>(spans.size()),
                        static_cast<quint32>(spansToAppend.size())};
    spans += spansToAppend;
    return range;
}

DecodedTreeCheckpoint DecodedTree::checkpoint() const {
    return {nodes.size(),        values.size(),       fieldValues.size(),
            valueRefs.size(),    spans.size(),        storageLayouts.size(),
            valueStrings.size(), ownedBytes.size(),   locators.size(),
            references.size()};
}

void DecodedTree::rollback(const DecodedTreeCheckpoint& point) {
    nodes.resize(point.nodes);
    values.resize(point.values);
    fieldValues.resize(point.fieldValues);
    valueRefs.resize(point.valueRefs);
    spans.resize(point.spans);
    storageLayouts.resize(point.layouts);
    valueStrings.resize(point.valueStrings);
    ownedBytes.resize(point.ownedBytes);
    locators.resize(point.locators);
    references.resize(point.references);
}

void DecodedTree::finalizeTopology() {
    QVector<DecodedNodeId> lastChildren(nodes.size(), kInvalidId);
    QVector<quint32> childCounts(nodes.size(), 0);
    for (DecodedNode& node : nodes) {
        node.firstChild = kInvalidId;
        node.nextSibling = kInvalidId;
        node.rowInParent = 0;
    }
    for (DecodedNodeId id = 0; id < static_cast<DecodedNodeId>(nodes.size()); ++id) {
        DecodedNode& node = nodes[id];
        if (node.parent == kInvalidId ||
            node.parent >= static_cast<DecodedNodeId>(nodes.size())) {
            continue;
        }
        DecodedNode& parent = nodes[node.parent];
        node.rowInParent = childCounts[node.parent]++;
        if (parent.firstChild == kInvalidId) {
            parent.firstChild = id;
        } else {
            nodes[lastChildren[node.parent]].nextSibling = id;
        }
        lastChildren[node.parent] = id;
    }
}

const DecodedFieldValue* DecodedTree::findField(DecodedValueId object,
                                                SymbolId fieldName) const {
    if (object >= static_cast<DecodedValueId>(values.size())) {
        return nullptr;
    }
    const DecodedValue& value = values.at(object);
    if (value.kind != DecodedValueKind::Object ||
        value.fields.first > static_cast<quint32>(fieldValues.size()) ||
        value.fields.count > static_cast<quint32>(fieldValues.size()) -
                                 value.fields.first) {
        return nullptr;
    }
    for (quint32 i = 0; i < value.fields.count; ++i) {
        const DecodedFieldValue& field = fieldValues.at(value.fields.first + i);
        if (field.name == fieldName) {
            return &field;
        }
    }
    return nullptr;
}

QString DecodedTree::displayValue(DecodedValueId id,
                                  const BrecoProgram& program) const {
    if (id >= static_cast<DecodedValueId>(values.size())) {
        return {};
    }
    const DecodedValue& value = values.at(id);
    switch (value.kind) {
        case DecodedValueKind::Null:
            return QStringLiteral("null");
        case DecodedValueKind::Boolean:
            return value.booleanValue ? QStringLiteral("true")
                                      : QStringLiteral("false");
        case DecodedValueKind::UnsignedInteger: {
            if (value.type < static_cast<TypeId>(program.types.size()) &&
                program.types.at(value.type).kind == TypeKind::Enum) {
                for (const EnumDesc& enumeration : program.enums) {
                    if (enumeration.type != value.type) {
                        continue;
                    }
                    for (quint32 i = 0; i < enumeration.values.count; ++i) {
                        const EnumValueDesc& member =
                            program.enumValues.at(enumeration.values.first + i);
                        if (member.value == value.unsignedValue) {
                            return QStringLiteral("%1 (%2)")
                                .arg(program.symbol(member.name))
                                .arg(value.unsignedValue);
                        }
                    }
                }
            }
            return QString::number(value.unsignedValue);
        }
        case DecodedValueKind::SignedInteger:
            return QString::number(value.signedValue);
        case DecodedValueKind::FloatingPoint:
            return QString::number(value.floatingValue, 'g', 15);
        case DecodedValueKind::String:
            return value.payload < static_cast<quint32>(valueStrings.size())
                       ? valueStrings.at(value.payload)
                       : QString();
        case DecodedValueKind::SourceBytes:
            if (value.payload < static_cast<quint32>(spans.size())) {
                return QStringLiteral("%1 bytes").arg(spans.at(value.payload).length);
            }
            return QStringLiteral("bytes");
        case DecodedValueKind::OwnedBytes:
            return value.payload < static_cast<quint32>(ownedBytes.size())
                       ? QString::fromLatin1(ownedBytes.at(value.payload).toHex(' '))
                       : QString();
        case DecodedValueKind::Object:
            return QStringLiteral("%1 fields").arg(value.fields.count);
        case DecodedValueKind::Sequence:
            return QStringLiteral("%1 items").arg(value.logicalCount);
        case DecodedValueKind::Reference: {
            if (value.payload >= static_cast<quint32>(references.size())) {
                return QStringLiteral("invalid reference");
            }
            const ReferenceHandle& reference = references.at(value.payload);
            if (reference.isNull) {
                return QStringLiteral("null");
            }
            return QStringLiteral("%1 0x%2 +%3")
                .arg(reference.strength == ReferenceStrength::Follow
                         ? QStringLiteral("follow")
                         : QStringLiteral("weak"))
                .arg(reference.target.logicalOffset, 0, 16)
                .arg(reference.target.regionLength);
        }
        case DecodedValueKind::Invalid:
            break;
    }
    return {};
}

}  // namespace breco::lang

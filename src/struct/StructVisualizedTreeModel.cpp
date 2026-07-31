#include "struct/StructVisualizedTreeModel.h"

#include <QColor>
#include <QStringList>

#include <algorithm>

namespace breco {

namespace {

const QColor kInvalidEvenBackground(255, 235, 235);
const QColor kInvalidOddBackground(255, 208, 208);
const QColor kValidEvenBackground(235, 255, 235);
const QColor kValidOddBackground(208, 240, 208);

QString validityText(const VisualizedNode& node) {
    QStringList details;
    if (node.hasCondition) {
        details.push_back(node.valid ? QStringLiteral("true")
                                     : QStringLiteral("false"));
    }
    if (node.bytesMissing > 0) {
        details.push_back(
            node.bytesMissing == 1
                ? QStringLiteral("1 missing byte")
                : QStringLiteral("%1 missing bytes").arg(node.bytesMissing));
    }
    return details.join(QStringLiteral(", "));
}

QString displayTypeText(const VisualizedNode& node) {
    return node.valueKind == VisualizedValueKind::Array ? QString() : node.typeName;
}

QString displayItemCount(qsizetype count) {
    if (count == 0) {
        return QStringLiteral("(empty)");
    }
    if (count == 1) {
        return QStringLiteral("1 item");
    }
    return QStringLiteral("%1 items").arg(count);
}

QString displayValueText(const VisualizedNode& node) {
    if (node.valueKind == VisualizedValueKind::Object) {
        return QString();
    }
    if (node.valueKind == VisualizedValueKind::Array) {
        return displayItemCount(node.children.size());
    }
    if (!node.valid && !node.errorMessage.isEmpty() &&
        !node.valueText.contains(node.errorMessage)) {
        return node.valueText.isEmpty()
                   ? QStringLiteral("invalid: %1").arg(node.errorMessage)
                   : node.valueText +
                         QStringLiteral(" [invalid: %1]").arg(node.errorMessage);
    }
    return node.valueText;
}

}  // namespace

StructVisualizedTreeModel::StructVisualizedTreeModel(QObject* parent)
    : QAbstractItemModel(parent) {}

void StructVisualizedTreeModel::setRoot(const VisualizedNode& root) {
    beginResetModel();
    m_nodes.clear();
    m_parentIndices.clear();
    m_childIndices.clear();
    flatten(root, -1, true);
    endResetModel();
}

void StructVisualizedTreeModel::flatten(const VisualizedNode& node, int parentIndex,
                                        bool flattenThisNode) {
    if (flattenThisNode) {
        for (const VisualizedNode& child : node.children) {
            flatten(child, -1);
        }
        return;
    }
    const int index = m_nodes.size();
    m_nodes.push_back(node);
    m_parentIndices.push_back(parentIndex);
    m_childIndices.resize(m_nodes.size());
    if (parentIndex >= 0) {
        m_childIndices[parentIndex].push_back(index);
    }
    for (const VisualizedNode& child : node.children) {
        flatten(child, index, false);
    }
}

int StructVisualizedTreeModel::nodeIndexForModelIndex(const QModelIndex& index) const {
    if (!index.isValid()) {
        return -1;
    }
    const int nodeIndex = static_cast<int>(index.internalId());
    return nodeIndex >= 0 && nodeIndex < m_nodes.size() ? nodeIndex : -1;
}

const VisualizedNode* StructVisualizedTreeModel::nodeForIndex(
    const QModelIndex& index) const {
    const int nodeIndex = nodeIndexForModelIndex(index);
    return nodeIndex >= 0 ? &m_nodes.at(nodeIndex) : nullptr;
}

QVector<const VisualizedNode*> StructVisualizedTreeModel::nodesForIndexes(
    const QModelIndexList& indexes) const {
    QVector<int> nodeIndices;
    nodeIndices.reserve(indexes.size());
    for (const QModelIndex& index : indexes) {
        if (index.column() != 0) {
            continue;
        }
        const int nodeIndex = nodeIndexForModelIndex(index);
        if (nodeIndex >= 0 && !nodeIndices.contains(nodeIndex)) {
            nodeIndices.push_back(nodeIndex);
        }
    }
    std::sort(nodeIndices.begin(), nodeIndices.end());

    QVector<const VisualizedNode*> nodes;
    nodes.reserve(nodeIndices.size());
    for (int nodeIndex : nodeIndices) {
        nodes.push_back(&m_nodes.at(nodeIndex));
    }
    return nodes;
}

QVector<const VisualizedNode*> StructVisualizedTreeModel::topLevelNodes() const {
    QVector<const VisualizedNode*> nodes;
    for (int i = 0; i < m_nodes.size(); ++i) {
        if (m_parentIndices.at(i) < 0) {
            nodes.push_back(&m_nodes.at(i));
        }
    }
    return nodes;
}

QModelIndex StructVisualizedTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (column < 0 || column >= ColumnCount || row < 0) {
        return QModelIndex();
    }
    const int parentNode = nodeIndexForModelIndex(parent);
    QVector<int> siblings;
    if (parentNode < 0) {
        for (int i = 0; i < m_nodes.size(); ++i) {
            if (m_parentIndices.at(i) < 0) {
                siblings.push_back(i);
            }
        }
    } else {
        siblings = m_childIndices.at(parentNode);
    }
    if (row >= siblings.size()) {
        return QModelIndex();
    }
  return createIndex(row, column, static_cast<quintptr>(siblings.at(row)));
}

QModelIndex StructVisualizedTreeModel::parent(const QModelIndex& index) const {
    if (!index.isValid()) {
        return QModelIndex();
    }
    const int nodeIndex = nodeIndexForModelIndex(index);
    if (nodeIndex < 0) {
        return QModelIndex();
    }
    const int parentNode = m_parentIndices.at(nodeIndex);
    if (parentNode < 0) {
        return QModelIndex();
    }
    const int grandparent = m_parentIndices.at(parentNode);
    QVector<int> siblings;
    if (grandparent < 0) {
        for (int i = 0; i < m_nodes.size(); ++i) {
            if (m_parentIndices.at(i) < 0) {
                siblings.push_back(i);
            }
        }
    } else {
        siblings = m_childIndices.at(grandparent);
    }
    const int row = siblings.indexOf(parentNode);
    return createIndex(row, 0, static_cast<quintptr>(parentNode));
}

int StructVisualizedTreeModel::rowCount(const QModelIndex& parent) const {
    const int parentNode = nodeIndexForModelIndex(parent);
    if (parentNode < 0) {
        int count = 0;
        for (int i = 0; i < m_parentIndices.size(); ++i) {
            if (m_parentIndices.at(i) < 0) {
                ++count;
            }
        }
        return count;
    }
    return m_childIndices.at(parentNode).size();
}

int StructVisualizedTreeModel::columnCount(const QModelIndex& parent) const {
    Q_UNUSED(parent);
    return ColumnCount;
}

QVariant StructVisualizedTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return QVariant();
    }
    const int nodeIndex = nodeIndexForModelIndex(index);
    if (nodeIndex < 0) {
        return QVariant();
    }
    const VisualizedNode& node = m_nodes.at(nodeIndex);
    if (role == SourceOffsetRole) {
        return node.hasSourceOffset
                   ? QVariant::fromValue<qulonglong>(node.sourceOffset)
                   : QVariant();
    }
    if (role == SourceFilePathRole) {
        return node.hasSourceOffset ? QVariant(node.sourceFilePath)
                                    : QVariant();
    }
    if (role == SourceLengthRole) {
        return node.hasSourceOffset
                   ? QVariant::fromValue<qulonglong>(node.sourceLength)
                   : QVariant();
    }
    if (role == EvenRowBackgroundRole || role == OddRowBackgroundRole) {
        const bool oddRow = role == OddRowBackgroundRole;
        if (node.bytesMissing > 0 || (node.hasCondition && !node.valid)) {
            return oddRow ? kInvalidOddBackground
                          : kInvalidEvenBackground;
        }
        if (node.hasCondition) {
            return oddRow ? kValidOddBackground
                          : kValidEvenBackground;
        }
        return QVariant();
    }
    if (role == Qt::BackgroundRole) {
        return data(index, (index.row() % 2) != 0
                               ? OddRowBackgroundRole
                               : EvenRowBackgroundRole);
    }
    if (role != Qt::DisplayRole) {
        return QVariant();
    }
    switch (index.column()) {
        case Name:
            return node.name;
        case Type:
            return displayTypeText(node);
        case Value:
            return displayValueText(node);
        case Bytes:
            return QString::fromLatin1(node.rawBytes.toHex(' ').toUpper());
        case Valid:
            return validityText(node);
        default:
            return QVariant();
    }
}

QVariant StructVisualizedTreeModel::headerData(int section, Qt::Orientation orientation,
                                               int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QVariant();
    }
    switch (section) {
        case Name:
            return QStringLiteral("Name");
        case Type:
            return QStringLiteral("Type");
        case Value:
            return QStringLiteral("Value");
        case Bytes:
            return QStringLiteral("Bytes");
        case Valid:
            return QStringLiteral("Valid");
        default:
            return QVariant();
    }
}

}  // namespace breco

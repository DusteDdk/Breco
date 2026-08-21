#include "brecolang/gui/DecodedTreeModel.h"

#include <QBrush>
#include <QColor>

namespace breco::lang {

DecodedTreeModel::DecodedTreeModel(QObject* parent) : QAbstractItemModel(parent) {}

void DecodedTreeModel::setDocument(
    std::shared_ptr<const BrecoProgram> program,
    std::shared_ptr<const DecodedTree> tree) {
    beginResetModel();
    m_program = std::move(program);
    m_tree = std::move(tree);
    endResetModel();
}

void DecodedTreeModel::clear() {
    setDocument({}, {});
}

DecodedNodeId DecodedTreeModel::childAt(DecodedNodeId parentId, int row) const {
    if (!m_tree || parentId >= static_cast<DecodedNodeId>(m_tree->nodes.size()) ||
        row < 0) {
        return kInvalidId;
    }
    DecodedNodeId child = m_tree->nodes.at(parentId).firstChild;
    for (int current = 0; child != kInvalidId && current < row; ++current) {
        child = m_tree->nodes.at(child).nextSibling;
    }
    return child;
}

int DecodedTreeModel::rootCount() const {
    if (!m_tree) {
        return 0;
    }
    int count = 0;
    for (const DecodedNode& node : m_tree->nodes) {
        if (node.parent == kInvalidId) {
            ++count;
        }
    }
    return count;
}

DecodedNodeId DecodedTreeModel::rootAt(int row) const {
    if (!m_tree || row < 0) {
        return kInvalidId;
    }
    int current = 0;
    for (DecodedNodeId id = 0;
         id < static_cast<DecodedNodeId>(m_tree->nodes.size()); ++id) {
        if (m_tree->nodes.at(id).parent == kInvalidId) {
            if (current == row) {
                return id;
            }
            ++current;
        }
    }
    return kInvalidId;
}

QModelIndex DecodedTreeModel::index(int row, int column,
                                    const QModelIndex& parentIndex) const {
    if (!m_tree || row < 0 || column < 0 || column >= columnCount()) {
        return {};
    }
    const DecodedNodeId id = parentIndex.isValid()
                                 ? childAt(static_cast<DecodedNodeId>(
                                               parentIndex.internalId()),
                                           row)
                                 : rootAt(row);
    return id != kInvalidId ? createIndex(row, column, quintptr{id})
                            : QModelIndex{};
}

QModelIndex DecodedTreeModel::parent(const QModelIndex& child) const {
    const DecodedNode* node = nodeForIndex(child);
    if (node == nullptr || node->parent == kInvalidId || !m_tree) {
        return {};
    }
    const DecodedNode& parentNode = m_tree->nodes.at(node->parent);
    return createIndex(static_cast<int>(parentNode.rowInParent), 0,
                       quintptr{node->parent});
}

int DecodedTreeModel::rowCount(const QModelIndex& parentIndex) const {
    if (!m_tree || parentIndex.column() > 0) {
        return 0;
    }
    if (!parentIndex.isValid()) {
        return rootCount();
    }
    const DecodedNode* node = nodeForIndex(parentIndex);
    if (node == nullptr) {
        return 0;
    }
    int count = 0;
    for (DecodedNodeId child = node->firstChild; child != kInvalidId;
         child = m_tree->nodes.at(child).nextSibling) {
        ++count;
    }
    return count;
}

int DecodedTreeModel::columnCount(const QModelIndex&) const { return 6; }

QString DecodedTreeModel::typeName(TypeId type) const {
    if (!m_program || type >= static_cast<TypeId>(m_program->types.size())) {
        return {};
    }
    const TypeDesc& descriptor = m_program->types.at(type);
    if (descriptor.name != kInvalidId) {
        return m_program->symbol(descriptor.name);
    }
    if (descriptor.kind == TypeKind::Sequence) {
        return QStringLiteral("sequence<%1>").arg(typeName(descriptor.elementType));
    }
    if (descriptor.kind == TypeKind::Optional) {
        return QStringLiteral("optional<%1>").arg(typeName(descriptor.elementType));
    }
    if (descriptor.kind == TypeKind::Variant) {
        return QStringLiteral("variant");
    }
    return QStringLiteral("type#%1").arg(type);
}

QVariant DecodedTreeModel::data(const QModelIndex& modelIndex, int role) const {
    const DecodedNode* node = nodeForIndex(modelIndex);
    if (node == nullptr || !m_program || !m_tree) {
        return {};
    }
    if (role == Qt::ForegroundRole && !node->valid) {
        return QBrush(QColor(190, 40, 40));
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    switch (modelIndex.column()) {
        case 0: return m_tree->name(node->name);
        case 1: return typeName(node->type);
        case 2: return m_tree->displayValue(node->value, *m_program);
        case 3:
            return node->input < static_cast<InputId>(m_program->inputs.size())
                       ? m_program->symbol(m_program->inputs.at(node->input).name)
                       : QString();
        case 4:
            return node->hasSourceSpan
                       ? QStringLiteral("0x%1").arg(node->offset, 0, 16)
                       : QString();
        case 5:
            return node->hasSourceSpan ? QString::number(node->length) : QString();
        default: break;
    }
    return {};
}

QVariant DecodedTreeModel::headerData(int section, Qt::Orientation orientation,
                                      int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    static const QStringList headers{
        QStringLiteral("Name"),   QStringLiteral("Type"),
        QStringLiteral("Value"),  QStringLiteral("Input"),
        QStringLiteral("Offset"), QStringLiteral("Length")};
    return headers.value(section);
}

const DecodedNode* DecodedTreeModel::nodeForIndex(
    const QModelIndex& modelIndex) const {
    if (!modelIndex.isValid() || !m_tree) {
        return nullptr;
    }
    const DecodedNodeId id =
        static_cast<DecodedNodeId>(modelIndex.internalId());
    return id < static_cast<DecodedNodeId>(m_tree->nodes.size())
               ? &m_tree->nodes.at(id)
               : nullptr;
}

}  // namespace breco::lang

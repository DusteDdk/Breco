#pragma once

#include <QAbstractItemModel>
#include <QVector>

#include "struct/VisualizedNode.h"

namespace breco {

class StructVisualizedTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Column {
        Name = 0,
        Type,
        Value,
        Bytes,
        Valid,
        ColumnCount,
    };

    enum Role {
        EvenRowBackgroundRole = Qt::UserRole + 1,
        OddRowBackgroundRole,
        SourceOffsetRole,
        SourceFilePathRole,
        SourceLengthRole,
    };

    explicit StructVisualizedTreeModel(QObject* parent = nullptr);

    void setRoot(const VisualizedNode& root);
    const VisualizedNode* nodeForIndex(const QModelIndex& index) const;
    QVector<const VisualizedNode*> nodesForIndexes(const QModelIndexList& indexes) const;
    QVector<const VisualizedNode*> topLevelNodes() const;

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    struct NodeRef {
        int index = -1;
    };

    QVector<VisualizedNode> m_nodes;
    QVector<int> m_parentIndices;
    QVector<QVector<int>> m_childIndices;

    void flatten(const VisualizedNode& node, int parentIndex, bool flattenThisNode = false);
    int nodeIndexForModelIndex(const QModelIndex& index) const;
};

}  // namespace breco

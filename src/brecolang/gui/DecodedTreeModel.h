#pragma once

#include <QAbstractItemModel>

#include <memory>

#include "brecolang/runtime/DecodedData.h"

namespace breco::lang {

class DecodedTreeModel final : public QAbstractItemModel {
public:
    explicit DecodedTreeModel(QObject* parent = nullptr);

    void setDocument(std::shared_ptr<const BrecoProgram> program,
                     std::shared_ptr<const DecodedTree> tree);
    void clear();

    QModelIndex index(int row, int column,
                      const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index,
                  int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const DecodedNode* nodeForIndex(const QModelIndex& index) const;
    std::shared_ptr<const DecodedTree> tree() const { return m_tree; }

private:
    DecodedNodeId childAt(DecodedNodeId parent, int row) const;
    int rootCount() const;
    DecodedNodeId rootAt(int row) const;
    QString typeName(TypeId type) const;

    std::shared_ptr<const BrecoProgram> m_program;
    std::shared_ptr<const DecodedTree> m_tree;
};

}  // namespace breco::lang

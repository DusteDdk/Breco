#pragma once

#include <QVector>

#include <memory>

#include "brecolang/runtime/ByteSource.h"
#include "brecolang/runtime/DecodedData.h"

namespace breco::lang {

class RenderStore {
public:
    RenderStore(std::shared_ptr<const BrecoProgram> program,
                std::shared_ptr<const DecodedTree> tree,
                QVector<std::shared_ptr<ByteSource>> inputs,
                DecodedValueId rootValue = kInvalidId);

    const BrecoProgram& program() const { return *m_program; }
    const DecodedTree& tree() const { return *m_tree; }
    std::shared_ptr<const BrecoProgram> programPtr() const { return m_program; }
    std::shared_ptr<const DecodedTree> treePtr() const { return m_tree; }
    DecodedValueId rootValue() const { return m_rootValue; }

    ByteSource* input(InputId id) const;
    QString inputRole(InputId id) const;
    QString inputPath(InputId id) const;
    quint64 logicalOffset(const ByteSpanValue& span) const;

private:
    std::shared_ptr<const BrecoProgram> m_program;
    std::shared_ptr<const DecodedTree> m_tree;
    QVector<std::shared_ptr<ByteSource>> m_inputs;
    DecodedValueId m_rootValue = kInvalidId;
};

}  // namespace breco::lang

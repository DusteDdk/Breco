#pragma once

#include <QAbstractItemModel>
#include <QColor>
#include <QHash>

#include <memory>
#include <vector>

#include "brecolang/runtime/DecodeDocument.h"

namespace breco::lang {

struct ReferencePageRequest {
    ReferenceHandle handle;
    QVector<MaterializationLocator> expansionPath;
};

class DecodedTreeModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    explicit DecodedTreeModel(QObject* parent = nullptr);

    void setDocument(std::shared_ptr<const BrecoProgram> program,
                     std::shared_ptr<const DecodedTree> tree);
    void setDocument(std::shared_ptr<const BrecoProgram> program,
                     DecodeDocumentHandle document,
                     std::shared_ptr<const ResolvedShapeSnapshot> shape);
    void copyDocumentFrom(const DecodedTreeModel& source);
    void applyPage(const DisplayPageResult& page);
    void failPage(const SequenceWindow& window, const QString& error);
    void failReference(const ReferencePageRequest& request,
                       const QString& error);
    void clear();

    QModelIndex index(int row, int column,
                      const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    bool hasChildren(const QModelIndex& parent = {}) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    QVariant data(const QModelIndex& index,
                  int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const DecodedNode* nodeForIndex(const QModelIndex& index) const;
    std::shared_ptr<const DecodedTree> treeForIndex(
        const QModelIndex& index) const;
    MaterializationLocator locatorForIndex(const QModelIndex& index) const;
    QVector<MaterializationLocator> expansionPathForIndex(
        const QModelIndex& index) const;
    QModelIndex indexForLocator(
        const MaterializationLocator& locator,
        const QVector<MaterializationLocator>& expansionPath = {}) const;
    bool isContinuationRow(const QModelIndex& index) const;
    bool isReferenceRow(const QModelIndex& index) const;
    bool requestMore(const QModelIndex& index);
    QVector<SequenceWindow> takeUnshownSequenceWindows(quint64 count);
    std::shared_ptr<const DecodedTree> tree() const { return m_legacyTree; }

    enum class SourceSpanCoverage { None, Partial, Full };
    struct SourceSpanHit {
        QModelIndex index;
        SourceSpanCoverage coverage = SourceSpanCoverage::None;
        int depth = 0;
    };
    QVector<SourceSpanHit> sourceSpansOverlapping(InputId input, quint64 offset,
                                                 quint64 length) const;
    void setSourceSpanHighlights(const QHash<quintptr, QColor>& colors);
    void clearSourceSpanHighlights();

    static constexpr quint32 kAutoExpandedListItems = 5;
    DecodeDocumentHandle document() const { return m_document; }
    std::shared_ptr<const ResolvedShapeSnapshot> shape() const { return m_shape; }

signals:
    void pageRequested(const SequenceWindow& window);
    void referenceRequested(const ReferencePageRequest& request);

private:
    using ModelNodeId = quint64;

    struct ModelNode {
        std::shared_ptr<const DecodedTree> page;
        DecodedNodeId pageNode = kInvalidId;
        MaterializationLocator locator;
        QVector<MaterializationLocator> expansionPath;
        QVector<ReferenceTargetIdentity> targetAncestry;
        ModelNodeId parent = 0;
        QVector<ModelNodeId> children;
        int row = 0;
    };

    struct SequenceState {
        struct Segment {
            MaterializationLocator locator;
            quint64 total = 0;
            quint64 shown = 0;
            SequenceIndexKind indexKind = SequenceIndexKind::LegacyEager;
            std::optional<SequenceContinuation> successor;
        };

        quint64 total = 0;
        quint64 shown = 0;
        quint64 pendingEnd = 0;
        SequenceIndexKind indexKind = SequenceIndexKind::LegacyEager;
        MaterializationLocator requestLocator;
        QVector<Segment> segments;
        std::optional<SequenceContinuation> successor;
        bool footerVisible = true;
        bool pending = false;
        QString error;
    };

    struct ReferenceState {
        ReferenceHandle handle;
        bool pending = false;
        bool loaded = false;
        bool cycle = false;
        QString error;
    };

    static constexpr quintptr footerMask() {
        return quintptr{1} << (sizeof(quintptr) * 8 - 1);
    }
    bool isFooterInternalId(quintptr id) const {
        return (id & footerMask()) != 0;
    }
    ModelNodeId nodeId(const QModelIndex& index) const;
    ModelNode* modelNode(ModelNodeId id);
    const ModelNode* modelNode(ModelNodeId id) const;
    QModelIndex indexForNode(ModelNodeId id, int column = 0) const;
    ModelNodeId mergeTree(
        const std::shared_ptr<const DecodedTree>& tree,
        const QVector<MaterializationLocator>& expansionPath = {},
        ModelNodeId attachParent = 0);
    ModelNodeId occurrence(
        const MaterializationLocator& locator,
        const QVector<MaterializationLocator>& expansionPath) const;
    quint64 aggregateLogicalCount(ModelNodeId id) const;
    quint64 contiguousSequenceItems(ModelNodeId id) const;
    void registerSequences(
        const std::shared_ptr<const ResolvedShapeSnapshot>& shape,
        const QVector<MaterializationLocator>& expansionPath);
    void rebuildRows(ModelNodeId parent);
    QString typeName(TypeId type) const;
    quint64 nextPageAmount(const SequenceState& state) const;
    void rebuildSpanIndex();

    struct SpanRecord {
        ModelNodeId id = 0;
        InputId input = kInvalidId;
        quint64 offset = 0;
        quint64 length = 0;
        bool hasBitSlice = false;
        quint8 highBit = 0;
        quint8 lowBit = 0;
        int depth = 0;
    };

    std::shared_ptr<const BrecoProgram> m_program;
    DecodeDocumentHandle m_document;
    std::shared_ptr<const ResolvedShapeSnapshot> m_shape;
    std::shared_ptr<const DecodedTree> m_legacyTree;
    std::vector<std::unique_ptr<ModelNode>> m_nodes;
    QVector<ModelNodeId> m_roots;
    QHash<MaterializationLocator, QVector<ModelNodeId>> m_locatorNodes;
    QHash<ModelNodeId, SequenceState> m_sequences;
    QHash<ModelNodeId, ReferenceState> m_references;
    QVector<SpanRecord> m_spanIndex;
    QHash<quintptr, QColor> m_spanHighlights;
};

}  // namespace breco::lang

Q_DECLARE_METATYPE(breco::lang::ReferencePageRequest)

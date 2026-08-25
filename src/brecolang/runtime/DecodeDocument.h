#pragma once

#include <QHash>
#include <QThread>
#include <QVector>

#include <memory>

#include "brecolang/runtime/Interpreter.h"

namespace breco::lang {

enum class DecodeIntent {
    ResolveShape,
    StreamingJson,
    LegacyMaterializeAll,
};

struct MaterializedPageDelta {
    MaterializationLocator root;
    QVector<MaterializationLocator> expansionPath;
    QVector<SequenceWindow> windows;
    std::shared_ptr<const ResolvedShapeSnapshot> shape;
    std::shared_ptr<const DecodedTree> tree;
    bool legacyFullTree = false;
};

struct DisplayPageRequest {
    DecodeDocumentHandle document;
    MaterializationLocator root;
    QVector<MaterializationLocator> expansionPath;
    quint32 maxDepth = 1;
    quint32 defaultSequenceItems = 64;
    QVector<SequenceWindow> sequenceWindows;
    quint64 maxNewNodes = 100000;
    quint64 maxNewBytes = 64 * 1024 * 1024;
    WorkBudget budget;
    CancellationToken cancellation;
};

struct DisplayPageResult {
    DecodeStatus status = DecodeStatus::Error;
    quint64 documentGeneration = 0;
    QVector<MaterializedPageDelta> deltas;
    QVector<RuntimeDiagnostic> diagnostics;
    MaterializationMetrics metrics;

    bool success() const { return status == DecodeStatus::Success; }
};

struct ResolvedSpanPlan {
    QVector<ByteSpanValue> spans;
    StorageLayout layout;
};

struct ExportSpanRequest {
    DecodeDocumentHandle document;
    InstanceLocator target;
    WorkBudget budget;
    CancellationToken cancellation;
};

struct ExportSpanResult {
    DecodeStatus status = DecodeStatus::Error;
    quint64 documentGeneration = 0;
    ResolvedSpanPlan spans;
    QVector<RuntimeDiagnostic> diagnostics;

    bool success() const { return status == DecodeStatus::Success; }
};

class DecodeDocument final {
public:
    DecodeDocument(DecodeDocumentHandle handle, quint64 generation);
    ~DecodeDocument();

    DecodeDocument(const DecodeDocument&) = delete;
    DecodeDocument& operator=(const DecodeDocument&) = delete;

    DecodeResult resolve(DecodeRequest request);
    DisplayPageResult requestDisplayPage(const DisplayPageRequest& request);
    ExportSpanResult requestExportSpans(const ExportSpanRequest& request);
    bool ensureLegacyMaterialization(quint64 maxLogicalNodes,
                                     const CancellationToken& cancellation,
                                     QString* error = nullptr);

    DecodeDocumentHandle handle() const { return m_handle; }
    quint64 generation() const { return m_generation; }
    std::shared_ptr<const BrecoProgram> program() const { return m_program; }
    const QVector<std::shared_ptr<ByteSource>>& inputs() const { return m_inputs; }
    std::shared_ptr<const DecodedTree> legacyTree() const { return m_legacyTree; }
    DecodedValueId legacyRootValue() const { return m_legacyRootValue; }
    const QString& entryName() const { return m_entryName; }
    quint64 startOffset() const { return m_startOffset; }
    MaterializationLocator rootLocator() const {
        return m_shape ? m_shape->root : MaterializationLocator{};
    }

private:
    void assertThreadAffinity() const;
    RuntimeDiagnostic diagnostic(QString code, QString message) const;
    bool copyStoredLayout(const InstanceLocator& target,
                          ResolvedSpanPlan* result) const;
    bool arithmeticLayout(const InstanceLocator& target,
                          ResolvedSpanPlan* result) const;
    bool sourcesAreCurrent() const;
    void cacheTree(const std::shared_ptr<const DecodedTree>& tree);

    struct CachedNode {
        std::shared_ptr<const DecodedTree> tree;
        DecodedNodeId node = kInvalidId;
    };

    DecodeDocumentHandle m_handle;
    quint64 m_generation = 0;
    Qt::HANDLE m_ownerThread = nullptr;
    std::shared_ptr<const BrecoProgram> m_program;
    QString m_entryName;
    QVector<std::shared_ptr<ByteSource>> m_inputs;
    QVector<ByteSourceIdentity> m_sourceIdentities;
    quint64 m_startOffset = 0;
    std::shared_ptr<const ResolvedShapeSnapshot> m_shape;
    std::shared_ptr<const DecodedTree> m_legacyTree;
    DecodedValueId m_legacyRootValue = kInvalidId;
    QVector<std::shared_ptr<const DecodedTree>> m_pages;
    QHash<MaterializationLocator, CachedNode> m_materializedNodes;
    QHash<ReferenceTargetKey, std::shared_ptr<const ResolvedShapeSnapshot>>
        m_targetShapes;
    QHash<ReferenceTargetKey, std::shared_ptr<const DecodedTree>>
        m_targetInitialPages;
    bool m_valid = true;
};

}  // namespace breco::lang

Q_DECLARE_METATYPE(breco::lang::DisplayPageRequest)
Q_DECLARE_METATYPE(breco::lang::DisplayPageResult)
Q_DECLARE_METATYPE(breco::lang::ExportSpanRequest)
Q_DECLARE_METATYPE(breco::lang::ExportSpanResult)
Q_DECLARE_METATYPE(breco::lang::DecodeResult)

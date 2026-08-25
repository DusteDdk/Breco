#include "brecolang/runtime/DecodeDocument.h"

#include <QElapsedTimer>

#include <algorithm>
#include <limits>

namespace breco::lang {

namespace {

bool startsWith(const QVector<StatementId>& value,
                const QVector<StatementId>& prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.cbegin(), prefix.cend(), value.cbegin());
}

bool checkedMultiply(quint64 left, quint64 right, quint64* result) {
    if (left != 0 && right > std::numeric_limits<quint64>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

}  // namespace

DecodeDocument::DecodeDocument(DecodeDocumentHandle handle, quint64 generation)
    : m_handle(handle), m_generation(generation),
      m_ownerThread(QThread::currentThreadId()) {}

DecodeDocument::~DecodeDocument() { assertThreadAffinity(); }

void DecodeDocument::assertThreadAffinity() const {
    Q_ASSERT(m_ownerThread == QThread::currentThreadId());
}

RuntimeDiagnostic DecodeDocument::diagnostic(QString code,
                                             QString message) const {
    return {DiagnosticSeverity::Error, std::move(code), std::move(message),
            {}, {}, false};
}

DecodeResult DecodeDocument::resolve(DecodeRequest request) {
    assertThreadAffinity();
    request.mode = DecodeMode::ResolveShape;
    request.documentGeneration = m_generation;
    m_program = request.program;
    m_entryName = request.entryName;
    m_inputs = request.inputs;
    m_sourceIdentities.clear();
    m_sourceIdentities.reserve(m_inputs.size());
    for (const std::shared_ptr<ByteSource>& input : std::as_const(m_inputs)) {
        m_sourceIdentities.push_back(input ? input->identity()
                                           : ByteSourceIdentity{});
    }
    m_startOffset = request.startOffset;

    DecodeResult core = decodeBrecoProgram(request);
    core.document = m_handle;
    m_shape = core.shape;
    m_materializedNodes.clear();
    m_targetShapes.clear();
    m_targetInitialPages.clear();
    cacheTree(core.tree);
    m_legacyRootValue = core.rootValue;
    if (core.shape && core.shape->sequences.isEmpty()) {
        m_legacyTree = core.tree;
    }
    m_valid = core.success();

    // Mutable document/source state remains here. The public resolve result is
    // the immutable shape/outline contract, never an authoritative eager tree.
    core.tree.reset();
    core.rootValue = kInvalidId;
    return core;
}

DisplayPageResult DecodeDocument::requestDisplayPage(
    const DisplayPageRequest& request) {
    assertThreadAffinity();
    QElapsedTimer timer;
    timer.start();
    DisplayPageResult result;
    result.documentGeneration = m_generation;
    if (!m_valid || request.document != m_handle || !m_shape ||
        !sourcesAreCurrent()) {
        m_valid = false;
        result.status = DecodeStatus::Invalidated;
        result.diagnostics.push_back(diagnostic(
            QStringLiteral("BRR0700"),
            QStringLiteral("Decode document is invalid or no longer available")));
        return result;
    }
    if (request.cancellation && request.cancellation->isCancelled()) {
        result.status = DecodeStatus::Cancelled;
        return result;
    }

    MaterializationLocator requestedRoot = request.root.isValid()
                                               ? request.root
                                               : m_shape->root;
    std::shared_ptr<const ResolvedShapeSnapshot> pageShape = m_shape;
    const bool targetRequest = requestedRoot.isReferenceTarget();
    if (targetRequest) {
        if (!requestedRoot.referenceTarget.has_value() ||
            requestedRoot.documentGeneration != m_generation) {
            result.status = DecodeStatus::Error;
            result.diagnostics.push_back(diagnostic(
                QStringLiteral("BRR0703"),
                QStringLiteral("Invalid reference target request")));
            return result;
        }
        const ReferenceTargetKey key = *requestedRoot.referenceTarget;
        pageShape = m_targetShapes.value(key);
        if (!pageShape) {
            DecodeRequest resolveTarget;
            resolveTarget.program = m_program;
            resolveTarget.entryName = m_entryName;
            resolveTarget.inputs = m_inputs;
            resolveTarget.mode = DecodeMode::ResolveShape;
            resolveTarget.startOffset = key.logicalOffset;
            resolveTarget.entryRootOffset = m_startOffset;
            resolveTarget.root = requestedRoot;
            resolveTarget.documentGeneration = m_generation;
            resolveTarget.workBudget = request.budget;
            resolveTarget.cancellation = request.cancellation;
            resolveTarget.shapeOptions.maxShapeNodes = request.maxNewNodes;
            DecodeResult resolvedTarget = decodeBrecoProgram(resolveTarget);
            if (!resolvedTarget.success() || !resolvedTarget.shape) {
                result.status = resolvedTarget.status;
                result.diagnostics = std::move(resolvedTarget.diagnostics);
                result.metrics.elapsedNanoseconds = timer.nsecsElapsed();
                return result;
            }
            pageShape = resolvedTarget.shape;
            m_targetShapes.insert(key, pageShape);
            m_targetInitialPages.insert(key, pageShape->outline);
            cacheTree(pageShape->outline);
        } else {
            ++result.metrics.cacheHits;
        }
    } else if (requestedRoot != m_shape->root) {
        result.status = DecodeStatus::Error;
        result.diagnostics.push_back(diagnostic(
            QStringLiteral("BRR0704"),
            QStringLiteral("Structural display requests must use the resolved entry root")));
        return result;
    }

    if (pageShape->sequences.isEmpty()) {
        MaterializedPageDelta delta;
        delta.root = pageShape->root;
        delta.expansionPath = request.expansionPath;
        delta.shape = pageShape;
        delta.tree = pageShape->outline;
        delta.legacyFullTree = !targetRequest;
        result.deltas.push_back(std::move(delta));
        result.status = DecodeStatus::Success;
        ++result.metrics.cacheHits;
        result.metrics.elapsedNanoseconds = timer.nsecsElapsed();
        return result;
    }

    QVector<SequenceWindow> windows = request.sequenceWindows;
    if (windows.isEmpty()) {
        windows.reserve(pageShape->sequences.size());
        for (const ResolvedSequenceShape& sequence : pageShape->sequences) {
            if (sequence.indexKind == SequenceIndexKind::LegacyEager) {
                continue;
            }
            SequenceWindow window{
                sequence.locator, 0,
                qMin<quint64>(sequence.displayCount,
                              request.defaultSequenceItems)};
            window.expansionPath = request.expansionPath;
            windows.push_back(std::move(window));
        }
    }

    DecodeRequest materialize;
    materialize.program = m_program;
    materialize.entryName = m_entryName;
    materialize.inputs = m_inputs;
    materialize.mode = DecodeMode::MaterializePage;
    materialize.startOffset = targetRequest
                                  ? requestedRoot.referenceTarget->logicalOffset
                                  : m_startOffset;
    materialize.entryRootOffset = m_startOffset;
    materialize.root = requestedRoot;
    materialize.documentGeneration = m_generation;
    materialize.sequenceWindows = windows;
    materialize.resolvedShape = pageShape;
    materialize.maxMaterializedNodes = request.maxNewNodes;
    materialize.workBudget = request.budget;
    materialize.cancellation = request.cancellation;
    DecodeResult page = decodeBrecoProgram(materialize);
    result.status = page.status;
    result.diagnostics = page.diagnostics;
    result.metrics.materializedNodes = page.constructedNodes;
    result.metrics.materializedValues = page.metrics.materializedValues;
    result.metrics.materializedLayouts = page.metrics.materializedLayouts;
    result.metrics.replayedItems = page.metrics.scannedItems;
    result.metrics.resumedItems = page.metrics.resumedItems;
    result.metrics.coldReplayedItems = page.metrics.coldReplayedItems;
    result.metrics.coldCursorOpens = page.metrics.coldCursorOpens;
    result.metrics.elapsedNanoseconds = timer.nsecsElapsed();
    if (page.status == DecodeStatus::Error &&
        std::any_of(page.diagnostics.cbegin(), page.diagnostics.cend(),
                    [](const RuntimeDiagnostic& diagnostic) {
                        return diagnostic.code == QStringLiteral("BRR0725") ||
                               diagnostic.code == QStringLiteral("BRR0726");
                    })) {
        m_valid = false;
        result.status = DecodeStatus::Invalidated;
    }
    if ((page.status != DecodeStatus::Success &&
         page.status != DecodeStatus::Paused) ||
        !page.tree) {
        return result;
    }
    quint64 retainedBytes =
        static_cast<quint64>(page.tree->nodes.size()) * sizeof(DecodedNode) +
        static_cast<quint64>(page.tree->values.size()) * sizeof(DecodedValue) +
        static_cast<quint64>(page.tree->fieldValues.size()) *
            sizeof(DecodedFieldValue) +
        static_cast<quint64>(page.tree->valueRefs.size()) *
            sizeof(DecodedValueId) +
        static_cast<quint64>(page.tree->spans.size()) * sizeof(ByteSpanValue) +
        static_cast<quint64>(page.tree->storageLayouts.size()) *
            sizeof(StorageLayout) +
        static_cast<quint64>(page.tree->references.size()) *
            sizeof(ReferenceHandle);
    for (const QString& value : page.tree->names) {
        retainedBytes += static_cast<quint64>(value.size()) * sizeof(QChar);
    }
    for (const QString& value : page.tree->valueStrings) {
        retainedBytes += static_cast<quint64>(value.size()) * sizeof(QChar);
    }
    for (const QByteArray& value : page.tree->ownedBytes) {
        retainedBytes += static_cast<quint64>(value.size());
    }
    if (retainedBytes > request.maxNewBytes) {
        result.status = DecodeStatus::Error;
        result.diagnostics.push_back(diagnostic(
            QStringLiteral("BRR0702"),
            QStringLiteral("Display-page byte budget exceeded")));
        return result;
    }
    const bool stoppedAtVariableBoundary = std::any_of(
        page.appliedSequenceWindows.cbegin(),
        page.appliedSequenceWindows.cend(),
        [pageShape](const SequenceWindow& applied) {
            for (const ResolvedSequenceShape& sequence :
                 pageShape->sequences) {
                if (sequence.locator == applied.sequence) {
                    return sequence.indexKind ==
                           SequenceIndexKind::ForwardReplay;
                }
            }
            return false;
        });
    if (!stoppedAtVariableBoundary &&
        page.endOffset != pageShape->endOffset) {
        m_valid = false;
        result.status = DecodeStatus::Invalidated;
        result.diagnostics.push_back(diagnostic(
            QStringLiteral("BRR0701"),
            QStringLiteral("Materialization no longer matches resolved shape")));
        return result;
    }
    m_pages.push_back(page.tree);
    cacheTree(page.tree);
    MaterializedPageDelta delta;
    delta.root = pageShape->root;
    delta.expansionPath = request.expansionPath;
    delta.shape = pageShape;
    delta.windows = page.appliedSequenceWindows.isEmpty()
                        ? windows
                        : page.appliedSequenceWindows;
    delta.tree = std::move(page.tree);
    result.deltas.push_back(std::move(delta));
    return result;
}

bool DecodeDocument::copyStoredLayout(const InstanceLocator& target,
                                      ResolvedSpanPlan* result) const {
    const auto found =
        m_materializedNodes.constFind(MaterializationLocator{target});
    if (found == m_materializedNodes.cend() || !found->tree ||
        found->node >=
            static_cast<DecodedNodeId>(found->tree->nodes.size())) {
        return false;
    }
    const DecodedNode& node = found->tree->nodes.at(found->node);
    if (node.storageLayout >=
        static_cast<quint32>(found->tree->storageLayouts.size())) {
        return false;
    }
    result->layout = found->tree->storageLayouts.at(node.storageLayout);
    result->layout.spans = {0, 0};
    const StorageLayout& sourceLayout =
        found->tree->storageLayouts.at(node.storageLayout);
    for (quint32 span = 0; span < sourceLayout.spans.count; ++span) {
        result->spans.push_back(
            found->tree->spans.at(sourceLayout.spans.first + span));
    }
    result->layout.spans.count = static_cast<quint32>(result->spans.size());
    return true;
}

void DecodeDocument::cacheTree(
    const std::shared_ptr<const DecodedTree>& tree) {
    if (!tree) return;
    for (DecodedNodeId node = 0;
         node < static_cast<DecodedNodeId>(tree->nodes.size()) &&
         node < static_cast<DecodedNodeId>(tree->locators.size()); ++node) {
        const MaterializationLocator& locator = tree->locators.at(node);
        if (!m_materializedNodes.contains(locator)) {
            m_materializedNodes.insert(locator, {tree, node});
        }
    }
}

bool DecodeDocument::arithmeticLayout(const InstanceLocator& target,
                                      ResolvedSpanPlan* result) const {
    if (!m_shape || !m_program) {
        return false;
    }
    for (const ResolvedSequenceShape& sequence : m_shape->sequences) {
        if (sequence.locator.isReferenceTarget() ||
            sequence.indexKind != SequenceIndexKind::Arithmetic ||
            sequence.statement >=
                static_cast<StatementId>(m_program->statements.size()) ||
            !startsWith(target.templatePath, sequence.locator.templatePath) ||
            target.sequenceIndexes.size() <
                sequence.locator.sequenceIndexes.size()) {
            continue;
        }
        quint64 offset = sequence.arithmetic.start;
        quint64 length = sequence.totalPrimaryBytes;
        StorageLayoutKind kind = StorageLayoutKind::Composite;
        TypeId declaredType =
            m_program->statements.at(sequence.statement).type;
        if (target != static_cast<InstanceLocator>(sequence.locator)) {
            const qsizetype indexPosition =
                sequence.locator.sequenceIndexes.size();
            if (target.sequenceIndexes.size() <= indexPosition) {
                continue;
            }
            const quint64 itemIndex = target.sequenceIndexes.at(indexPosition);
            if (itemIndex >= sequence.arithmetic.count) {
                return false;
            }
            quint64 itemBytes = 0;
            if (!checkedMultiply(itemIndex, sequence.arithmetic.stride,
                                 &itemBytes)) {
                return false;
            }
            offset += itemBytes;
            length = sequence.arithmetic.stride;
            declaredType = sequence.itemType;

            if (target.templatePath.size() >
                sequence.locator.templatePath.size()) {
                const StatementId wanted = target.templatePath.at(
                    sequence.locator.templatePath.size());
                const Statement& loop =
                    m_program->statements.at(sequence.statement);
                quint64 relative = 0;
                bool found = false;
                for (quint32 i = 0; i < loop.statements.count; ++i) {
                    const StatementId childId = m_program->statementRefs.at(
                        loop.statements.first + i);
                    const Statement& child = m_program->statements.at(childId);
                    if (childId == wanted) {
                        declaredType = child.type;
                        if (child.extent <
                                static_cast<quint32>(m_program->extents.size()) &&
                            m_program->extents.at(child.extent)
                                .exactBytes.has_value()) {
                            length = *m_program->extents.at(child.extent)
                                          .exactBytes;
                        }
                        offset += relative;
                        found = true;
                        kind = StorageLayoutKind::SourceSlice;
                        break;
                    }
                    if (child.extent >=
                            static_cast<quint32>(m_program->extents.size()) ||
                        !m_program->extents.at(child.extent)
                             .exactBytes.has_value()) {
                        return false;
                    }
                    relative +=
                        *m_program->extents.at(child.extent).exactBytes;
                }
                if (!found) {
                    return false;
                }
            }
        }
        result->spans = {
            {sequence.arithmetic.input,
             m_inputs.at(sequence.arithmetic.input)->absoluteOffset(offset),
             length}};
        result->layout.kind = kind;
        result->layout.spans = {0, 1};
        result->layout.declaredType = declaredType;
        if (declaredType < static_cast<TypeId>(m_program->types.size())) {
            const TypeDesc& descriptor = m_program->types.at(declaredType);
            result->layout.endianness = descriptor.endianness;
            result->layout.bitWidth = descriptor.bitWidth;
        }
        return true;
    }
    return false;
}

ExportSpanResult DecodeDocument::requestExportSpans(
    const ExportSpanRequest& request) {
    assertThreadAffinity();
    ExportSpanResult result;
    result.documentGeneration = m_generation;
    if (!m_valid || request.document != m_handle || !sourcesAreCurrent()) {
        m_valid = false;
        result.status = DecodeStatus::Invalidated;
        result.diagnostics.push_back(diagnostic(
            QStringLiteral("BRR0710"),
            QStringLiteral("Decode document is invalid or no longer available")));
        return result;
    }
    if (request.cancellation && request.cancellation->isCancelled()) {
        result.status = DecodeStatus::Cancelled;
        return result;
    }
    if (!copyStoredLayout(request.target, &result.spans) &&
        !arithmeticLayout(request.target, &result.spans)) {
        result.status = DecodeStatus::Error;
        result.diagnostics.push_back(diagnostic(
            QStringLiteral("BRR0711"),
            QStringLiteral("No storage layout is available for the locator")));
        return result;
    }
    result.status = DecodeStatus::Success;
    return result;
}

bool DecodeDocument::ensureLegacyMaterialization(
    quint64 maxLogicalNodes, const CancellationToken& cancellation,
    QString* error) {
    assertThreadAffinity();
    if (m_legacyTree) return true;
    if (!m_valid || !m_shape || !sourcesAreCurrent()) {
        if (error) *error = QStringLiteral("Decode document was invalidated");
        return false;
    }
    if (m_shape->logicalNodeCount > maxLogicalNodes) {
        if (error) {
            *error = QStringLiteral(
                "Outform compatibility materialization is limited to %1 logical nodes; this document has %2")
                         .arg(maxLogicalNodes)
                         .arg(m_shape->logicalNodeCount);
        }
        return false;
    }
    DecodeRequest request;
    request.program = m_program;
    request.entryName = m_entryName;
    request.inputs = m_inputs;
    request.mode = DecodeMode::Tree;
    request.startOffset = m_startOffset;
    request.documentGeneration = m_generation;
    request.cancellation = cancellation;
    DecodeResult decoded = decodeBrecoProgram(request);
    if (!decoded.success() || !decoded.tree) {
        if (error) {
            QStringList messages;
            for (const RuntimeDiagnostic& diagnostic : decoded.diagnostics) {
                messages.push_back(QStringLiteral("%1: %2")
                                       .arg(diagnostic.code,
                                            diagnostic.message));
            }
            *error = messages.join(QLatin1Char('\n'));
        }
        return false;
    }
    m_legacyTree = decoded.tree;
    m_legacyRootValue = decoded.rootValue;
    cacheTree(m_legacyTree);
    return true;
}

bool DecodeDocument::sourcesAreCurrent() const {
    assertThreadAffinity();
    if (m_sourceIdentities.size() != m_inputs.size()) {
        return false;
    }
    for (qsizetype i = 0; i < m_inputs.size(); ++i) {
        if (m_inputs.at(i) &&
            m_inputs.at(i)->identity() != m_sourceIdentities.at(i)) {
            return false;
        }
    }
    return true;
}

}  // namespace breco::lang

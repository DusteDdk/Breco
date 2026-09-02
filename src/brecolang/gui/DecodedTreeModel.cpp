#include "brecolang/gui/DecodedTreeModel.h"

#include <QBrush>
#include <QColor>
#include <QVector>

#include <algorithm>
#include <limits>
#include <utility>

namespace breco::lang {

namespace {

std::optional<quint64> sequenceItemIndex(QStringView name) {
    if (name.size() < 3 || name.front() != QLatin1Char('[') ||
        name.back() != QLatin1Char(']')) {
        return std::nullopt;
    }
    bool ok = false;
    const quint64 index = name.mid(1, name.size() - 2).toULongLong(&ok);
    return ok ? std::optional<quint64>(index) : std::nullopt;
}

}  // namespace

DecodedTreeModel::DecodedTreeModel(QObject* parent) : QAbstractItemModel(parent) {}

void DecodedTreeModel::setDocument(
    std::shared_ptr<const BrecoProgram> program,
    std::shared_ptr<const DecodedTree> tree) {
    beginResetModel();
    m_program = std::move(program);
    m_document = {};
    m_shape.reset();
    m_legacyTree = std::move(tree);
    m_nodes.clear();
    m_roots.clear();
    m_locatorNodes.clear();
    m_sequences.clear();
    m_references.clear();
    mergeTree(m_legacyTree);
    rebuildSpanIndex();
    endResetModel();
}

void DecodedTreeModel::setDocument(
    std::shared_ptr<const BrecoProgram> program, DecodeDocumentHandle document,
    std::shared_ptr<const ResolvedShapeSnapshot> shape) {
    beginResetModel();
    m_program = std::move(program);
    m_document = document;
    m_shape = std::move(shape);
    m_legacyTree.reset();
    m_nodes.clear();
    m_roots.clear();
    m_locatorNodes.clear();
    m_sequences.clear();
    m_references.clear();
    if (m_shape) {
        mergeTree(m_shape->outline);
        if (m_shape->sequences.isEmpty()) {
            m_legacyTree = m_shape->outline;
        }
        registerSequences(m_shape, {});
    }
    rebuildSpanIndex();
    endResetModel();
}

void DecodedTreeModel::copyDocumentFrom(const DecodedTreeModel& source) {
    beginResetModel();
    m_program = source.m_program;
    m_document = source.m_document;
    m_shape = source.m_shape;
    m_legacyTree = source.m_legacyTree;
    m_nodes.clear();
    m_nodes.reserve(source.m_nodes.size());
    for (const std::unique_ptr<ModelNode>& node : source.m_nodes) {
        m_nodes.push_back(node ? std::make_unique<ModelNode>(*node) : nullptr);
    }
    m_roots = source.m_roots;
    m_locatorNodes = source.m_locatorNodes;
    m_sequences = source.m_sequences;
    for (SequenceState& state : m_sequences) {
        state.pending = false;
        state.pendingEnd = 0;
    }
    m_references = source.m_references;
    for (ReferenceState& state : m_references) {
        state.pending = false;
    }
    rebuildSpanIndex();
    endResetModel();
}

void DecodedTreeModel::clear() { setDocument({}, {}); }

DecodedTreeModel::ModelNode* DecodedTreeModel::modelNode(ModelNodeId id) {
    return id > 0 && id <= static_cast<ModelNodeId>(m_nodes.size())
               ? m_nodes[static_cast<qsizetype>(id - 1)].get()
               : nullptr;
}

const DecodedTreeModel::ModelNode* DecodedTreeModel::modelNode(
    ModelNodeId id) const {
    return id > 0 && id <= static_cast<ModelNodeId>(m_nodes.size())
               ? m_nodes.at(static_cast<qsizetype>(id - 1)).get()
               : nullptr;
}

void DecodedTreeModel::rebuildRows(ModelNodeId parentId) {
    QVector<ModelNodeId>* children =
        parentId == 0 ? &m_roots : &modelNode(parentId)->children;
    const ModelNode* parent = modelNode(parentId);
    if (parent != nullptr && parent->page != nullptr &&
        parent->pageNode <
            static_cast<DecodedNodeId>(parent->page->nodes.size())) {
        const DecodedNode& decoded =
            parent->page->nodes.at(parent->pageNode);
        if (m_program != nullptr &&
            decoded.type < static_cast<TypeId>(m_program->types.size()) &&
            m_program->types.at(decoded.type).kind == TypeKind::Aggregate) {
            std::sort(children->begin(), children->end(),
                      [&](ModelNodeId left, ModelNodeId right) {
                          const ModelNode* a = modelNode(left);
                          const ModelNode* b = modelNode(right);
                          const auto aIndex =
                              a != nullptr && a->page != nullptr
                                  ? sequenceItemIndex(a->page->name(
                                        a->page->nodes.at(a->pageNode).name))
                                  : std::nullopt;
                          const auto bIndex =
                              b != nullptr && b->page != nullptr
                                  ? sequenceItemIndex(b->page->name(
                                        b->page->nodes.at(b->pageNode).name))
                                  : std::nullopt;
                          return aIndex.value_or(
                                     std::numeric_limits<quint64>::max()) <
                                 bIndex.value_or(
                                     std::numeric_limits<quint64>::max());
                      });
        }
    }
    for (qsizetype row = 0; row < children->size(); ++row) {
        if (ModelNode* child = modelNode(children->at(row)); child != nullptr) {
            child->row = static_cast<int>(row);
        }
    }
}

quint64 DecodedTreeModel::aggregateLogicalCount(ModelNodeId id) const {
    const ModelNode* node = modelNode(id);
    if (node == nullptr || node->page == nullptr ||
        node->pageNode >=
            static_cast<DecodedNodeId>(node->page->nodes.size())) {
        return 0;
    }
    const DecodedNode& decoded = node->page->nodes.at(node->pageNode);
    if (decoded.value >=
        static_cast<DecodedValueId>(node->page->values.size())) {
        return 0;
    }
    const DecodedValue& value = node->page->values.at(decoded.value);
    return value.kind == DecodedValueKind::Aggregate
               ? value.logicalCount
               : 0;
}

quint64 DecodedTreeModel::contiguousSequenceItems(ModelNodeId id) const {
    const ModelNode* node = modelNode(id);
    if (node == nullptr) {
        return 0;
    }
    quint64 expected = 0;
    for (ModelNodeId childId : node->children) {
        const ModelNode* child = modelNode(childId);
        if (child == nullptr || child->page == nullptr ||
            child->pageNode >=
                static_cast<DecodedNodeId>(child->page->nodes.size())) {
            break;
        }
        const auto index = sequenceItemIndex(child->page->name(
            child->page->nodes.at(child->pageNode).name));
        if (!index.has_value() || *index != expected) {
            break;
        }
        ++expected;
    }
    return expected;
}

DecodedTreeModel::ModelNodeId DecodedTreeModel::occurrence(
    const MaterializationLocator& locator,
    const QVector<MaterializationLocator>& expansionPath) const {
    const auto found = m_locatorNodes.constFind(locator);
    if (found == m_locatorNodes.cend()) {
        return 0;
    }
    for (ModelNodeId id : found.value()) {
        const ModelNode* node = modelNode(id);
        if (node != nullptr && node->expansionPath == expansionPath) {
            return id;
        }
    }
    return 0;
}

void DecodedTreeModel::registerSequences(
    const std::shared_ptr<const ResolvedShapeSnapshot>& shape,
    const QVector<MaterializationLocator>& expansionPath) {
    if (!shape) {
        return;
    }
    for (const ResolvedSequenceShape& sequence : shape->sequences) {
        const ModelNodeId id = occurrence(sequence.locator, expansionPath);
        if (id == 0) {
            continue;
        }
        const ModelNode* node = modelNode(id);
        const bool aggregateAlias =
            node != nullptr && node->locator != sequence.locator;
        auto existing = m_sequences.find(id);
        if (existing != m_sequences.end()) {
            if (aggregateAlias &&
                std::none_of(existing->segments.cbegin(),
                             existing->segments.cend(),
                             [&](const SequenceState::Segment& segment) {
                                 return segment.locator == sequence.locator;
                             })) {
                existing->segments.push_back(
                    {sequence.locator, sequence.displayCount, 0,
                     sequence.indexKind, {}});
                const quint64 logical = aggregateLogicalCount(id);
                existing->total =
                    logical != 0 ? logical
                                 : existing->total + sequence.displayCount;
                rebuildRows(id);
                existing->shown = contiguousSequenceItems(id);
            }
            continue;
        }
        SequenceState state;
        state.total = sequence.displayCount;
        state.indexKind = sequence.indexKind;
        state.requestLocator = sequence.locator;
        if (aggregateAlias) {
            state.segments.push_back(
                {sequence.locator, sequence.displayCount, 0,
                 sequence.indexKind, {}});
            const quint64 logical = aggregateLogicalCount(id);
            if (logical != 0) {
                state.total = logical;
            }
            rebuildRows(id);
            state.shown = contiguousSequenceItems(id);
        } else if (node != nullptr) {
            state.shown = qMin(
                state.total, static_cast<quint64>(node->children.size()));
        }
        state.footerVisible = state.shown < state.total;
        m_sequences.insert(id, std::move(state));
    }
}

DecodedTreeModel::ModelNodeId DecodedTreeModel::mergeTree(
    const std::shared_ptr<const DecodedTree>& tree,
    const QVector<MaterializationLocator>& expansionPath,
    ModelNodeId attachParent) {
    if (!tree) {
        return 0;
    }
    QVector<ReferenceTargetIdentity> targetAncestry;
    if (const ModelNode* attachment = modelNode(attachParent);
        attachment != nullptr) {
        targetAncestry = attachment->targetAncestry;
        const auto reference = m_references.constFind(attachParent);
        if (reference != m_references.cend() &&
            !reference->handle.isNull) {
            targetAncestry.push_back(
                reference->handle.target.identity);
        }
    }
    QVector<ModelNodeId> pageIds(tree->nodes.size(), 0);
    ModelNodeId firstRoot = 0;
    for (DecodedNodeId pageId = 0;
         pageId < static_cast<DecodedNodeId>(tree->nodes.size()); ++pageId) {
        if (tree->nodes.at(pageId).hidden) {
            continue;
        }
        MaterializationLocator locator =
            pageId < static_cast<DecodedNodeId>(tree->locators.size())
                ? tree->locators.at(pageId)
                : MaterializationLocator{};
        if (!locator.isValid()) {
            locator.documentGeneration =
                m_shape ? m_shape->documentGeneration : 1;
            locator.templatePath = {pageId};
        }
        ModelNodeId id = occurrence(locator, expansionPath);
        if (id == 0) {
            auto node = std::make_unique<ModelNode>();
            node->page = tree;
            node->pageNode = pageId;
            node->locator = locator;
            node->expansionPath = expansionPath;
            node->targetAncestry = targetAncestry;
            id = static_cast<ModelNodeId>(m_nodes.size()) + 1;
            m_nodes.push_back(std::move(node));
            m_locatorNodes[locator].push_back(id);
            const DecodedNode& decoded = tree->nodes.at(pageId);
            if (decoded.kind == DecodedNodeKind::Reference &&
                decoded.value <
                    static_cast<DecodedValueId>(tree->values.size())) {
                const DecodedValue& value = tree->values.at(decoded.value);
                if (value.kind == DecodedValueKind::Reference &&
                    value.payload <
                        static_cast<quint32>(tree->references.size())) {
                    ReferenceState state;
                    state.handle = tree->references.at(value.payload);
                    state.cycle = !state.handle.isNull &&
                                  targetAncestry.contains(
                                      state.handle.target.identity);
                    m_references.insert(id, std::move(state));
                }
            }
        }
        pageIds[pageId] = id;
    }
    for (DecodedNodeId pageId = 0;
         pageId < static_cast<DecodedNodeId>(tree->nodes.size()); ++pageId) {
        const DecodedNode& decoded = tree->nodes.at(pageId);
        if (!decoded.hidden ||
            decoded.replacement >=
                static_cast<DecodedNodeId>(pageIds.size())) {
            continue;
        }
        const ModelNodeId replacementId =
            pageIds.at(decoded.replacement);
        if (replacementId == 0) {
            continue;
        }
        MaterializationLocator locator =
            pageId < static_cast<DecodedNodeId>(tree->locators.size())
                ? tree->locators.at(pageId)
                : MaterializationLocator{};
        if (!locator.isValid()) {
            locator.documentGeneration =
                m_shape ? m_shape->documentGeneration : 1;
            locator.templatePath = {pageId};
        }
        QVector<ModelNodeId>& occurrences = m_locatorNodes[locator];
        if (!occurrences.contains(replacementId)) {
            occurrences.push_back(replacementId);
        }
    }
    for (DecodedNodeId pageId = 0;
         pageId < static_cast<DecodedNodeId>(tree->nodes.size()); ++pageId) {
        const DecodedNode& decoded = tree->nodes.at(pageId);
        const ModelNodeId id = pageIds.at(pageId);
        if (id == 0) {
            continue;
        }
        ModelNode* node = modelNode(id);
        const ModelNodeId parentId =
            decoded.parent < static_cast<DecodedNodeId>(pageIds.size())
                ? pageIds.at(decoded.parent)
                : attachParent;
        if (node->parent == 0 && parentId != 0) {
            node->parent = parentId;
        }
        QVector<ModelNodeId>* siblings =
            parentId == 0 ? &m_roots : &modelNode(parentId)->children;
        if (!siblings->contains(id)) {
            siblings->push_back(id);
        }
        if (decoded.parent >= static_cast<DecodedNodeId>(pageIds.size()) &&
            firstRoot == 0) {
            firstRoot = id;
        }
    }
    for (DecodedNodeId pageId = 0;
         pageId < static_cast<DecodedNodeId>(tree->nodes.size()); ++pageId) {
        const DecodedNode& decoded = tree->nodes.at(pageId);
        if (decoded.hidden) {
            continue;
        }
        const ModelNodeId parentId =
            decoded.parent < static_cast<DecodedNodeId>(pageIds.size())
                ? pageIds.at(decoded.parent)
                : attachParent;
        rebuildRows(parentId);
    }
    return firstRoot;
}

void DecodedTreeModel::applyPage(const DisplayPageResult& page) {
    if ((page.status != DecodeStatus::Success &&
         page.status != DecodeStatus::Paused) ||
        page.documentGeneration == 0 || !m_shape ||
        page.documentGeneration != m_shape->documentGeneration) {
        return;
    }
    const auto applyWindows = [this](const MaterializedPageDelta& delta) {
        for (const SequenceWindow& window : delta.windows) {
            const ModelNodeId sequenceId =
                occurrence(window.sequence, delta.expansionPath);
            auto found = m_sequences.find(sequenceId);
            if (found == m_sequences.end()) {
                continue;
            }
            if (!found->segments.isEmpty()) {
                auto segment = std::find_if(
                    found->segments.begin(), found->segments.end(),
                    [&](const SequenceState::Segment& value) {
                        return value.locator == window.sequence;
                    });
                if (segment == found->segments.end() ||
                    window.firstItem > segment->shown) {
                    continue;
                }
                segment->shown = qMax(
                    segment->shown,
                    qMin(segment->total,
                         window.firstItem + window.itemCount));
                if (segment->indexKind ==
                        SequenceIndexKind::ForwardReplay &&
                    window.successor.has_value()) {
                    segment->successor = window.successor;
                }
                rebuildRows(sequenceId);
                found->shown = contiguousSequenceItems(sequenceId);
            } else {
                if (window.firstItem > found->shown) {
                    continue;
                }
                found->shown = qMax(
                    found->shown,
                    qMin(found->total,
                         window.firstItem + window.itemCount));
            }
            found->footerVisible = found->shown < found->total;
            if (found->segments.isEmpty() &&
                found->indexKind == SequenceIndexKind::ForwardReplay &&
                window.successor.has_value()) {
                found->successor = window.successor;
            }
            found->pending = false;
            found->pendingEnd = 0;
        }
    };
    QVector<SequenceWindow> resumeRequests;
    for (const MaterializedPageDelta& delta : page.deltas) {
        if (!delta.tree) {
            continue;
        }
        ModelNodeId attachParent = 0;
        if (!delta.expansionPath.isEmpty()) {
            QVector<MaterializationLocator> parentPath =
                delta.expansionPath;
            const MaterializationLocator edge = parentPath.takeLast();
            attachParent = occurrence(edge, parentPath);
            auto reference = m_references.find(attachParent);
            if (reference != m_references.end()) {
                reference->pending = false;
                reference->error.clear();
                if (!reference->loaded) {
                    reference->loaded = true;
                    const QModelIndex referenceIndex =
                        indexForNode(attachParent);
                    const int firstRow = rowCount(referenceIndex);
                    const int rootCount = static_cast<int>(std::count_if(
                        delta.tree->nodes.cbegin(), delta.tree->nodes.cend(),
                        [nodeCount = delta.tree->nodes.size()](
                            const DecodedNode& node) {
                            return node.parent >=
                                   static_cast<DecodedNodeId>(nodeCount);
                        }));
                    if (rootCount > 0) {
                        beginInsertRows(referenceIndex, firstRow,
                                        firstRow + rootCount - 1);
                    }
                    mergeTree(delta.tree, delta.expansionPath, attachParent);
                    registerSequences(delta.shape, delta.expansionPath);
                    applyWindows(delta);
                    if (rootCount > 0) {
                        endInsertRows();
                    }
                    const QModelIndex value =
                        referenceIndex.siblingAtColumn(2);
                    emit dataChanged(value, value, {Qt::DisplayRole});
                    continue;
                }
            }
        }
        if (delta.legacyFullTree || delta.windows.size() != 1) {
            beginResetModel();
            mergeTree(delta.tree, delta.expansionPath, attachParent);
            registerSequences(delta.shape, delta.expansionPath);
            applyWindows(delta);
            endResetModel();
            continue;
        }

        const SequenceWindow window = delta.windows.first();
        const ModelNodeId sequenceId =
            occurrence(window.sequence, delta.expansionPath);
        auto found = m_sequences.find(sequenceId);
        if (sequenceId == 0 || found == m_sequences.end()) {
            beginResetModel();
            mergeTree(delta.tree, delta.expansionPath, attachParent);
            registerSequences(delta.shape, delta.expansionPath);
            endResetModel();
            continue;
        }
        SequenceState& state = found.value();
        const quint64 pendingEnd = state.pendingEnd;
        if (!state.segments.isEmpty()) {
            beginResetModel();
            mergeTree(delta.tree, delta.expansionPath, attachParent);
            registerSequences(delta.shape, delta.expansionPath);
            SequenceState& updated = m_sequences[sequenceId];
            auto updatedSegment = std::find_if(
                updated.segments.begin(), updated.segments.end(),
                [&](const SequenceState::Segment& value) {
                    return value.locator == window.sequence;
                });
            if (updatedSegment != updated.segments.end() &&
                window.firstItem <= updatedSegment->shown) {
                updatedSegment->shown =
                    qMax(updatedSegment->shown,
                         qMin(updatedSegment->total,
                              window.firstItem + window.itemCount));
                if (updatedSegment->indexKind ==
                        SequenceIndexKind::ForwardReplay &&
                    window.successor.has_value()) {
                    updatedSegment->successor = window.successor;
                }
            }
            rebuildRows(sequenceId);
            updated.shown = contiguousSequenceItems(sequenceId);
            updated.footerVisible = updated.shown < updated.total;
            updated.error.clear();
            if (page.status == DecodeStatus::Paused &&
                pendingEnd > updated.shown &&
                updatedSegment != updated.segments.end() &&
                updatedSegment->indexKind ==
                    SequenceIndexKind::ForwardReplay &&
                updatedSegment->successor.has_value()) {
                updated.pending = true;
                SequenceWindow resume{
                    updatedSegment->locator, updatedSegment->shown,
                    pendingEnd - updated.shown};
                resume.successor = updatedSegment->successor;
                resume.expansionPath = delta.expansionPath;
                resumeRequests.push_back(std::move(resume));
            } else {
                updated.pending = false;
                updated.pendingEnd = 0;
            }
            endResetModel();
            continue;
        }
        const quint64 oldShown = state.shown;
        auto segment = std::find_if(
            state.segments.begin(), state.segments.end(),
            [&](const SequenceState::Segment& value) {
                return value.locator == window.sequence;
            });
        quint64 newShown = oldShown;
        quint64 newSegmentShown = 0;
        if (segment != state.segments.end() &&
            window.firstItem <= segment->shown) {
            newSegmentShown =
                qMax(segment->shown,
                     qMin(segment->total,
                          window.firstItem + window.itemCount));
            newShown = 0;
            for (auto value = state.segments.cbegin();
                 value != state.segments.cend(); ++value) {
                const quint64 shown =
                    value == segment ? newSegmentShown : value->shown;
                newShown += shown;
                if (shown < value->total) {
                    break;
                }
            }
        } else if (state.segments.isEmpty() &&
                   window.firstItem <= oldShown) {
            newShown =
                qMax(oldShown,
                     qMin(state.total,
                          window.firstItem + window.itemCount));
        }
        const QModelIndex sequenceIndex = indexForNode(sequenceId);
        if (state.footerVisible) {
            beginRemoveRows(sequenceIndex, static_cast<int>(oldShown),
                            static_cast<int>(oldShown));
            state.footerVisible = false;
            endRemoveRows();
        }
        if (newShown > oldShown) {
            beginInsertRows(sequenceIndex, static_cast<int>(oldShown),
                            static_cast<int>(newShown - 1));
            mergeTree(delta.tree, delta.expansionPath, attachParent);
            registerSequences(delta.shape, delta.expansionPath);
            state.shown = newShown;
            if (segment != state.segments.end()) {
                segment->shown = newSegmentShown;
            }
            state.error.clear();
            endInsertRows();
        } else {
            mergeTree(delta.tree, delta.expansionPath, attachParent);
            registerSequences(delta.shape, delta.expansionPath);
        }
        if (window.successor.has_value()) {
            if (segment != state.segments.end() &&
                segment->indexKind ==
                    SequenceIndexKind::ForwardReplay) {
                segment->successor = window.successor;
            } else if (state.segments.isEmpty() &&
                       state.indexKind ==
                           SequenceIndexKind::ForwardReplay) {
                state.successor = window.successor;
            }
        }
        state.error.clear();
        const SequenceIndexKind activeIndexKind =
            segment != state.segments.end() ? segment->indexKind
                                            : state.indexKind;
        const std::optional<SequenceContinuation>& activeSuccessor =
            segment != state.segments.end() ? segment->successor
                                            : state.successor;
        if (page.status == DecodeStatus::Paused &&
            pendingEnd > state.shown &&
            activeIndexKind == SequenceIndexKind::ForwardReplay &&
            activeSuccessor.has_value()) {
            state.pending = true;
            const quint64 resumeFirst =
                segment != state.segments.end() ? segment->shown
                                                : state.shown;
            SequenceWindow resume{window.sequence, resumeFirst,
                                  pendingEnd - state.shown};
            resume.successor = activeSuccessor;
            resume.expansionPath = delta.expansionPath;
            resumeRequests.push_back(std::move(resume));
        } else {
            state.pending = false;
            state.pendingEnd = 0;
        }
        if (state.shown < state.total) {
            beginInsertRows(sequenceIndex, static_cast<int>(state.shown),
                            static_cast<int>(state.shown));
            state.footerVisible = true;
            endInsertRows();
        }
    }
    for (const SequenceWindow& resume : std::as_const(resumeRequests)) {
        emit pageRequested(resume);
    }
    rebuildSpanIndex();
}

void DecodedTreeModel::failPage(const SequenceWindow& window,
                                const QString& error) {
    const ModelNodeId sequenceId =
        occurrence(window.sequence, window.expansionPath);
    auto state = m_sequences.find(sequenceId);
    if (state == m_sequences.end()) {
        return;
    }
    state->pending = false;
    state->pendingEnd = 0;
    state->error = error;
    if (state->footerVisible) {
        const QModelIndex footer = index(static_cast<int>(state->shown), 0,
                                         indexForNode(sequenceId));
        emit dataChanged(footer, footer, {Qt::DisplayRole});
    }
}

void DecodedTreeModel::failReference(const ReferencePageRequest& request,
                                     const QString& error) {
    if (request.expansionPath.isEmpty()) {
        return;
    }
    QVector<MaterializationLocator> parentPath = request.expansionPath;
    const MaterializationLocator edge = parentPath.takeLast();
    const ModelNodeId referenceId = occurrence(edge, parentPath);
    auto state = m_references.find(referenceId);
    if (state == m_references.end()) {
        return;
    }
    state->pending = false;
    state->error = error;
    const QModelIndex changed = indexForNode(referenceId);
    emit dataChanged(changed, changed.siblingAtColumn(columnCount() - 1),
                     {Qt::DisplayRole});
}

DecodedTreeModel::ModelNodeId DecodedTreeModel::nodeId(
    const QModelIndex& index) const {
    if (!index.isValid() || isFooterInternalId(index.internalId())) {
        return 0;
    }
    return static_cast<ModelNodeId>(index.internalId());
}

QModelIndex DecodedTreeModel::indexForNode(ModelNodeId id, int column) const {
    const ModelNode* node = modelNode(id);
    return node ? createIndex(node->row, column, quintptr{id}) : QModelIndex{};
}

QModelIndex DecodedTreeModel::index(int row, int column,
                                    const QModelIndex& parentIndex) const {
    if (row < 0 || column < 0 || column >= columnCount() ||
        parentIndex.column() > 0) {
        return {};
    }
    if (!parentIndex.isValid()) {
        return row < m_roots.size()
                   ? createIndex(row, column, quintptr{m_roots.at(row)})
                   : QModelIndex{};
    }
    const ModelNodeId parentId = nodeId(parentIndex);
    const ModelNode* parentNode = modelNode(parentId);
    if (!parentNode) {
        return {};
    }
    const auto sequence = m_sequences.constFind(parentId);
    if (sequence != m_sequences.cend()) {
        if (row < static_cast<int>(sequence->shown) &&
            row < parentNode->children.size()) {
            return createIndex(row, column,
                               quintptr{parentNode->children.at(row)});
        }
        if (sequence->footerVisible &&
            row == static_cast<int>(sequence->shown)) {
            return createIndex(row, column,
                               footerMask() | quintptr{parentId});
        }
        return {};
    }
    return row < parentNode->children.size()
               ? createIndex(row, column,
                             quintptr{parentNode->children.at(row)})
               : QModelIndex{};
}

QModelIndex DecodedTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    if (isFooterInternalId(child.internalId())) {
        const ModelNodeId sequenceId =
            static_cast<ModelNodeId>(child.internalId() & ~footerMask());
        return indexForNode(sequenceId);
    }
    const ModelNode* node = modelNode(nodeId(child));
    return node && node->parent != 0 ? indexForNode(node->parent)
                                     : QModelIndex{};
}

int DecodedTreeModel::rowCount(const QModelIndex& parentIndex) const {
    if (parentIndex.column() > 0) {
        return 0;
    }
    if (!parentIndex.isValid()) {
        return m_roots.size();
    }
    const ModelNodeId parentId = nodeId(parentIndex);
    const ModelNode* node = modelNode(parentId);
    if (!node) {
        return 0;
    }
    const auto sequence = m_sequences.constFind(parentId);
    if (sequence != m_sequences.cend()) {
        const quint64 rows =
            sequence->shown + (sequence->footerVisible ? 1 : 0);
        return static_cast<int>(qMin<quint64>(
            rows, static_cast<quint64>(std::numeric_limits<int>::max())));
    }
    return node->children.size();
}

int DecodedTreeModel::columnCount(const QModelIndex&) const { return 6; }

bool DecodedTreeModel::hasChildren(const QModelIndex& parentIndex) const {
    if (QAbstractItemModel::hasChildren(parentIndex)) {
        return true;
    }
    const auto reference = m_references.constFind(nodeId(parentIndex));
    return reference != m_references.cend() && !reference->loaded &&
           !reference->cycle && !reference->handle.isNull;
}

bool DecodedTreeModel::canFetchMore(const QModelIndex& parentIndex) const {
    const auto found = m_references.constFind(nodeId(parentIndex));
    return found != m_references.cend() && !found->pending &&
           !found->loaded && !found->cycle && !found->handle.isNull;
}

void DecodedTreeModel::fetchMore(const QModelIndex& parentIndex) {
    const ModelNodeId id = nodeId(parentIndex);
    auto found = m_references.find(id);
    ModelNode* node = modelNode(id);
    if (found == m_references.end() || node == nullptr || found->pending ||
        found->loaded || found->cycle || found->handle.isNull) {
        return;
    }
    found->pending = true;
    found->error.clear();
    ReferencePageRequest request;
    request.handle = found->handle;
    request.expansionPath = node->expansionPath;
    request.expansionPath.push_back(node->locator);
    emit referenceRequested(request);
}

QString DecodedTreeModel::typeName(TypeId type) const {
    if (!m_program || type >= static_cast<TypeId>(m_program->types.size())) {
        return {};
    }
    const TypeDesc& descriptor = m_program->types.at(type);
    if (descriptor.name != kInvalidId) {
        return m_program->symbol(descriptor.name);
    }
    if (descriptor.kind == TypeKind::Sequence ||
        descriptor.kind == TypeKind::Aggregate) {
        return QStringLiteral("%1<%2>")
            .arg(descriptor.kind == TypeKind::Aggregate
                     ? QStringLiteral("aggregate")
                     : QStringLiteral("sequence"),
                 typeName(descriptor.elementType));
    }
    if (descriptor.kind == TypeKind::Optional) {
        return QStringLiteral("optional<%1>").arg(typeName(descriptor.elementType));
    }
    if (descriptor.kind == TypeKind::Variant) {
        return QStringLiteral("variant");
    }
    if (descriptor.kind == TypeKind::Reference) {
        return QStringLiteral("ref<%1>")
            .arg(typeName(descriptor.elementType));
    }
    return QStringLiteral("type#%1").arg(type);
}

quint64 DecodedTreeModel::nextPageAmount(const SequenceState& state) const {
    const quint64 remaining = state.total - state.shown;
    const quint64 doubled =
        state.shown > std::numeric_limits<quint64>::max() / 2
            ? std::numeric_limits<quint64>::max()
            : state.shown * 2;
    return qMin(remaining, qMax<quint64>(64, doubled));
}

QVariant DecodedTreeModel::data(const QModelIndex& modelIndex, int role) const {
    if (isContinuationRow(modelIndex)) {
        if (role != Qt::DisplayRole || modelIndex.column() != 0) {
            return {};
        }
        const ModelNodeId sequenceId = static_cast<ModelNodeId>(
            modelIndex.internalId() & ~footerMask());
        const SequenceState state = m_sequences.value(sequenceId);
        if (state.pending) {
            return QStringLiteral("Loading…");
        }
        if (!state.error.isEmpty()) {
            return QStringLiteral("Retry loading items — %1").arg(state.error);
        }
        return QStringLiteral("Show next %1 items (%2 remaining)")
            .arg(nextPageAmount(state))
            .arg(state.total - state.shown);
    }
    const ModelNode* model = modelNode(nodeId(modelIndex));
    const DecodedNode* node = nodeForIndex(modelIndex);
    if (!model || !node || !m_program || !model->page) {
        return {};
    }
    if (role == Qt::ForegroundRole && !node->valid) {
        return QBrush(QColor(190, 40, 40));
    }
    if (role == Qt::BackgroundRole) {
        const auto found = m_spanHighlights.constFind(nodeId(modelIndex));
        if (found != m_spanHighlights.cend()) {
            return QBrush(found.value());
        }
        return {};
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    switch (modelIndex.column()) {
        case 0: return model->page->name(node->name);
        case 1: return typeName(node->type);
        case 2: {
            const auto reference = m_references.constFind(nodeId(modelIndex));
            if (reference != m_references.cend()) {
                if (reference->cycle) {
                    return QStringLiteral("back-reference (cycle)");
                }
                if (reference->pending) {
                    return QStringLiteral("loading target…");
                }
                if (!reference->error.isEmpty()) {
                    return QStringLiteral("target error: %1")
                        .arg(reference->error);
                }
            }
            return model->page->displayValue(node->value, *m_program);
        }
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
        default: return {};
    }
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
    const QModelIndex& index) const {
    const ModelNode* node = modelNode(nodeId(index));
    return node && node->page &&
                   node->pageNode <
                       static_cast<DecodedNodeId>(node->page->nodes.size())
               ? &node->page->nodes.at(node->pageNode)
               : nullptr;
}

std::shared_ptr<const DecodedTree> DecodedTreeModel::treeForIndex(
    const QModelIndex& index) const {
    const ModelNode* node = modelNode(nodeId(index));
    return node ? node->page : nullptr;
}

MaterializationLocator DecodedTreeModel::locatorForIndex(
    const QModelIndex& index) const {
    if (isContinuationRow(index)) {
        const ModelNodeId sequenceId = static_cast<ModelNodeId>(
            index.internalId() & ~footerMask());
        const ModelNode* sequence = modelNode(sequenceId);
        return sequence ? sequence->locator : MaterializationLocator{};
    }
    const ModelNode* node = modelNode(nodeId(index));
    return node ? node->locator : MaterializationLocator{};
}

QVector<MaterializationLocator> DecodedTreeModel::expansionPathForIndex(
    const QModelIndex& index) const {
    const ModelNode* node = modelNode(nodeId(index));
    return node ? node->expansionPath : QVector<MaterializationLocator>{};
}

QModelIndex DecodedTreeModel::indexForLocator(
    const MaterializationLocator& locator,
    const QVector<MaterializationLocator>& expansionPath) const {
    return indexForNode(occurrence(locator, expansionPath));
}

bool DecodedTreeModel::isContinuationRow(const QModelIndex& index) const {
    return index.isValid() && isFooterInternalId(index.internalId());
}

bool DecodedTreeModel::isReferenceRow(const QModelIndex& index) const {
    return m_references.contains(nodeId(index));
}

bool DecodedTreeModel::requestMore(const QModelIndex& index) {
    ModelNodeId sequenceId = 0;
    if (isContinuationRow(index)) {
        sequenceId = static_cast<ModelNodeId>(index.internalId() & ~footerMask());
    } else {
        sequenceId = nodeId(index);
    }
    auto state = m_sequences.find(sequenceId);
    ModelNode* sequence = modelNode(sequenceId);
    if (state == m_sequences.end() || !sequence || state->pending ||
        state->shown >= state->total) {
        return false;
    }
    auto segment = std::find_if(
        state->segments.begin(), state->segments.end(),
        [](const SequenceState::Segment& value) {
            return value.shown < value.total;
        });
    const quint64 amount =
        segment != state->segments.end()
            ? qMin(nextPageAmount(*state), segment->total - segment->shown)
            : nextPageAmount(*state);
    state->pending = true;
    state->pendingEnd = state->shown + amount;
    state->error.clear();
    const QModelIndex footer = this->index(static_cast<int>(state->shown), 0,
                                            indexForNode(sequenceId));
    emit dataChanged(footer, footer, {Qt::DisplayRole});
    SequenceWindow window{
        segment != state->segments.end()
            ? segment->locator
            : (state->requestLocator.isValid() ? state->requestLocator
                                               : sequence->locator),
        segment != state->segments.end() ? segment->shown : state->shown,
        amount};
    window.expansionPath = sequence->expansionPath;
    if (segment != state->segments.end() &&
        segment->indexKind == SequenceIndexKind::ForwardReplay) {
        window.successor = segment->successor;
    } else if (state->segments.isEmpty() &&
               state->indexKind == SequenceIndexKind::ForwardReplay) {
        window.successor = state->successor;
    }
    emit pageRequested(window);
    return true;
}

QVector<SequenceWindow> DecodedTreeModel::takeUnshownSequenceWindows(
    quint64 count) {
    QVector<SequenceWindow> windows;
    if (count == 0) {
        return windows;
    }
    for (auto it = m_sequences.begin(); it != m_sequences.end(); ++it) {
        SequenceState& state = it.value();
        ModelNode* sequence = modelNode(it.key());
        auto segment = std::find_if(
            state.segments.begin(), state.segments.end(),
            [](const SequenceState::Segment& value) {
                return value.shown < value.total;
            });
        const SequenceIndexKind indexKind =
            segment != state.segments.end() ? segment->indexKind
                                            : state.indexKind;
        const bool alreadyShown =
            state.segments.isEmpty() ? state.shown > 0
                                     : state.shown >= count;
        if (sequence == nullptr || state.pending || alreadyShown ||
            state.total == 0 ||
            indexKind == SequenceIndexKind::LegacyEager) {
            continue;
        }
        if (!state.segments.isEmpty()) {
            const quint64 desired = count - state.shown;
            const quint64 amount =
                segment != state.segments.end()
                    ? qMin(desired, segment->total - segment->shown)
                    : 0;
            if (amount > 0) {
                SequenceWindow window{segment->locator, segment->shown,
                                      amount};
                window.expansionPath = sequence->expansionPath;
                windows.push_back(std::move(window));
                state.pending = true;
                state.pendingEnd = state.shown + amount;
            }
            continue;
        }
        const quint64 amount =
            segment != state.segments.end()
                ? qMin(count, segment->total - segment->shown)
                : qMin(count, state.total);
        state.pending = true;
        state.pendingEnd = amount;
        SequenceWindow window{
            segment != state.segments.end()
                ? segment->locator
                : (state.requestLocator.isValid()
                       ? state.requestLocator
                       : sequence->locator),
            segment != state.segments.end() ? segment->shown : 0, amount};
        window.expansionPath = sequence->expansionPath;
        windows.push_back(std::move(window));
    }
    return windows;
}

void DecodedTreeModel::rebuildSpanIndex() {
    m_spanIndex.clear();
    m_spanHighlights.clear();
    for (ModelNodeId id = 1; id <= static_cast<ModelNodeId>(m_nodes.size()); ++id) {
        const ModelNode* model = modelNode(id);
        if (model == nullptr || !model->page ||
            model->pageNode >=
                static_cast<DecodedNodeId>(model->page->nodes.size())) {
            continue;
        }
        const DecodedNode& node = model->page->nodes.at(model->pageNode);
        if (!node.hasSourceSpan || node.length == 0) {
            continue;
        }
        int depth = 0;
        for (ModelNodeId parent = model->parent; parent != 0;) {
            ++depth;
            const ModelNode* parentNode = modelNode(parent);
            if (parentNode == nullptr) {
                break;
            }
            parent = parentNode->parent;
        }
        SpanRecord record;
        record.id = id;
        record.input = node.input;
        record.offset = node.offset;
        record.length = node.length;
        record.depth = depth;
        if (model->page != nullptr &&
            node.storageLayout <
                static_cast<quint32>(model->page->storageLayouts.size())) {
            const StorageLayout& layout =
                model->page->storageLayouts.at(node.storageLayout);
            if (layout.kind == StorageLayoutKind::BitSlice) {
                record.hasBitSlice = true;
                record.highBit = layout.highBit;
                record.lowBit = layout.lowBit;
            }
        }
        m_spanIndex.push_back(record);
    }
}

QVector<DecodedTreeModel::SourceSpanHit> DecodedTreeModel::sourceSpansOverlapping(
    InputId input, quint64 offset, quint64 length) const {
    QVector<SourceSpanHit> hits;
    if (length == 0) {
        return hits;
    }
    const quint64 queryEnd =
        offset > std::numeric_limits<quint64>::max() - length
            ? std::numeric_limits<quint64>::max()
            : offset + length;
    for (const SpanRecord& record : m_spanIndex) {
        if (input != kInvalidId && record.input != input) {
            continue;
        }
        const quint64 recordEnd =
            record.offset > std::numeric_limits<quint64>::max() - record.length
                ? std::numeric_limits<quint64>::max()
                : record.offset + record.length;
        if (recordEnd <= offset || record.offset >= queryEnd) {
            continue;
        }
        SourceSpanHit hit;
        hit.index = indexForNode(record.id);
        hit.depth = record.depth;
        const bool queryCoversField = offset <= record.offset && queryEnd >= recordEnd;
        const bool fieldCoversQuery = record.offset <= offset && recordEnd >= queryEnd;
        hit.coverage = queryCoversField ? SourceSpanCoverage::Full
                                        : SourceSpanCoverage::Partial;
        if (record.hasBitSlice) {
            const quint8 lo = qMin(record.lowBit, record.highBit);
            const quint8 hi = qMax(record.lowBit, record.highBit);
            const quint64 bitStart = record.offset + static_cast<quint64>(lo / 8);
            const quint64 bitEnd = record.offset + static_cast<quint64>(hi / 8) + 1ULL;
            if (offset <= bitStart && queryEnd >= bitEnd) {
                hit.coverage = SourceSpanCoverage::Full;
            }
        } else if (length == 1 && record.length == 1 && record.offset == offset) {
            hit.coverage = SourceSpanCoverage::Full;
        } else if (length == 1 && fieldCoversQuery && record.length == 1) {
            hit.coverage = SourceSpanCoverage::Full;
        }
        hits.push_back(hit);
    }
    return hits;
}

void DecodedTreeModel::setSourceSpanHighlights(const QHash<quintptr, QColor>& colors) {
    m_spanHighlights = colors;
    if (m_nodes.empty()) {
        return;
    }
    if (rowCount() > 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
                         {Qt::BackgroundRole});
    }
    for (auto it = m_spanHighlights.cbegin(); it != m_spanHighlights.cend(); ++it) {
        const QModelIndex highlighted = indexForNode(static_cast<ModelNodeId>(it.key()));
        if (!highlighted.isValid()) {
            continue;
        }
        emit dataChanged(highlighted,
                         highlighted.sibling(highlighted.row(), columnCount() - 1),
                         {Qt::BackgroundRole});
    }
}

void DecodedTreeModel::clearSourceSpanHighlights() {
    if (m_spanHighlights.isEmpty()) {
        return;
    }
    m_spanHighlights.clear();
    if (!m_nodes.empty() && rowCount() > 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
                         {Qt::BackgroundRole});
    }
}

}  // namespace breco::lang

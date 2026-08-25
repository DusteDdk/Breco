#include "brecolang/gui/DecodedTreeModel.h"

#include <QBrush>
#include <QColor>

#include <algorithm>
#include <limits>
#include <utility>

namespace breco::lang {

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
    for (qsizetype row = 0; row < children->size(); ++row) {
        if (ModelNode* child = modelNode(children->at(row)); child != nullptr) {
            child->row = static_cast<int>(row);
        }
    }
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
        if (id == 0 || m_sequences.contains(id)) {
            continue;
        }
        SequenceState state;
        state.total = sequence.displayCount;
        state.indexKind = sequence.indexKind;
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
        const ModelNodeId id = pageIds.at(pageId);
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
                reference->loaded = true;
                reference->error.clear();
            }
        }
        if (delta.legacyFullTree || delta.windows.size() != 1) {
            beginResetModel();
            mergeTree(delta.tree, delta.expansionPath, attachParent);
            registerSequences(delta.shape, delta.expansionPath);
            for (const SequenceWindow& window : delta.windows) {
                const ModelNodeId sequenceId =
                    occurrence(window.sequence, delta.expansionPath);
                auto found = m_sequences.find(sequenceId);
                if (found != m_sequences.end() &&
                    window.firstItem <= found->shown) {
                    found->shown = qMax(
                        found->shown,
                        qMin(found->total,
                             window.firstItem + window.itemCount));
                    found->footerVisible = found->shown < found->total;
                    if (found->indexKind ==
                            SequenceIndexKind::ForwardReplay &&
                        window.successor.has_value()) {
                        found->successor = window.successor;
                    }
                    found->pending = false;
                    found->pendingEnd = 0;
                }
            }
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
        const quint64 oldShown = state.shown;
        const quint64 newShown =
            window.firstItem <= oldShown
                ? qMax(oldShown,
                       qMin(state.total,
                            window.firstItem + window.itemCount))
                : oldShown;
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
            state.shown = newShown;
            state.error.clear();
            endInsertRows();
        } else {
            mergeTree(delta.tree, delta.expansionPath, attachParent);
        }
        if (state.indexKind == SequenceIndexKind::ForwardReplay &&
            window.successor.has_value()) {
            state.successor = window.successor;
        }
        state.error.clear();
        if (page.status == DecodeStatus::Paused &&
            pendingEnd > state.shown &&
            state.indexKind == SequenceIndexKind::ForwardReplay &&
            state.successor.has_value()) {
            state.pending = true;
            SequenceWindow resume{window.sequence, state.shown,
                                  pendingEnd - state.shown};
            resume.successor = state.successor;
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
    if (descriptor.kind == TypeKind::Sequence) {
        return QStringLiteral("sequence<%1>").arg(typeName(descriptor.elementType));
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
    state->pending = true;
    state->pendingEnd = state->shown + nextPageAmount(*state);
    state->error.clear();
    const QModelIndex footer = this->index(static_cast<int>(state->shown), 0,
                                            indexForNode(sequenceId));
    emit dataChanged(footer, footer, {Qt::DisplayRole});
    SequenceWindow window{sequence->locator, state->shown,
                          state->pendingEnd - state->shown};
    window.expansionPath = sequence->expansionPath;
    if (state->indexKind == SequenceIndexKind::ForwardReplay) {
        window.successor = state->successor;
    }
    emit pageRequested(window);
    return true;
}

}  // namespace breco::lang

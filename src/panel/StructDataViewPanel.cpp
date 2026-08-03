#include "panel/StructDataViewPanel.h"

#include "struct/StructExport.h"
#include "struct/StructVisualizedTreeModel.h"
#include "ui_StructDataView.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QColor>
#include <QClipboard>
#include <QHeaderView>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QStyledItemDelegate>
#include <QStringList>
#include <QTreeView>

namespace breco {

namespace {

class StructTreeItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option,
                         const QModelIndex& index) const override {
        const bool oddVisualRow =
            option->features.testFlag(QStyleOptionViewItem::Alternate);
        QStyledItemDelegate::initStyleOption(option, index);
        const QColor background =
            index
                .data(oddVisualRow
                          ? StructVisualizedTreeModel::OddRowBackgroundRole
                          : StructVisualizedTreeModel::EvenRowBackgroundRole)
                .value<QColor>();
        if (background.isValid()) {
            option->backgroundBrush = QBrush(background);
        }
    }
};

}  // namespace

StructDataViewPanel::StructDataViewPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::StructDataView>()) {
    m_ui->setupUi(this);
    m_model = new StructVisualizedTreeModel(this);
    m_ui->structDataTreeView->setModel(m_model);
    m_ui->structDataTreeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ui->structDataTreeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_ui->structDataTreeView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_ui->structDataTreeView->setItemDelegate(
        new StructTreeItemDelegate(m_ui->structDataTreeView));
    connect(m_ui->structDataTreeView, &QTreeView::customContextMenuRequested,
            this, &StructDataViewPanel::showTreeContextMenu);
    auto* copyShortcut =
        new QShortcut(QKeySequence::Copy, m_ui->structDataTreeView);
    connect(copyShortcut, &QShortcut::activated, this,
            &StructDataViewPanel::copySelectedScalarValuesToClipboard);
    connect(m_ui->structDataTreeView, &QTreeView::clicked, this,
            [this](const QModelIndex& index) {
                const VisualizedNode* node =
                    m_model->nodeForIndex(index.siblingAtColumn(0));
                if (node != nullptr &&
                    node->declarationRange.end > node->declarationRange.start) {
                    emit declarationLocationActivated(
                        node->declarationRange.start,
                        node->declarationRange.end);
                }
                const QVariant offset =
                    index.data(StructVisualizedTreeModel::SourceOffsetRole);
                if (!offset.isValid()) {
                    return;
                }
                const quint64 byteLength =
                    index
                        .data(StructVisualizedTreeModel::SourceLengthRole)
                        .toULongLong();
                emit sourceLocationActivated(
                    index
                        .data(StructVisualizedTreeModel::SourceFilePathRole)
                        .toString(),
                    offset.toULongLong(), byteLength);
            });
    QHeaderView* header = m_ui->structDataTreeView->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(StructVisualizedTreeModel::Name,
                                 QHeaderView::ResizeToContents);
    header->setSectionResizeMode(StructVisualizedTreeModel::Type,
                                 QHeaderView::ResizeToContents);
    header->setSectionResizeMode(StructVisualizedTreeModel::Value,
                                 QHeaderView::Stretch);
    header->setSectionResizeMode(StructVisualizedTreeModel::Bytes,
                                 QHeaderView::ResizeToContents);
    header->setSectionResizeMode(StructVisualizedTreeModel::Valid,
                                 QHeaderView::ResizeToContents);
    connect(m_ui->expandCollapseAllButton, &QPushButton::clicked, this,
            [this]() { toggleExpandCollapseAll(); });
}

StructDataViewPanel::~StructDataViewPanel() = default;

QTreeView* StructDataViewPanel::structDataTreeView() const { return m_ui->structDataTreeView; }

QPushButton* StructDataViewPanel::expandCollapseAllButton() const {
    return m_ui->expandCollapseAllButton;
}

QVBoxLayout* StructDataViewPanel::structDataViewLayout() const {
    return m_ui->structDataViewLayout;
}

void StructDataViewPanel::setVisualization(const VisualizedNode& root) {
    m_model->setRoot(root);
    m_ui->structDataTreeView->expandAll();
    m_allExpanded = true;
    m_ui->expandCollapseAllButton->setText(QStringLiteral("Collapse All"));
}

void StructDataViewPanel::clearVisualization() {
    VisualizedNode emptyRoot;
    emptyRoot.name = QStringLiteral("root");
    m_model->setRoot(emptyRoot);
    m_allExpanded = false;
    m_ui->expandCollapseAllButton->setText(QStringLiteral("Expand All"));
}

void StructDataViewPanel::setSourceEndianness(Endianness endianness) {
    m_sourceEndianness = endianness;
}

void StructDataViewPanel::setOutforms(const QVector<OutformNode>& outforms) {
    m_outforms = outforms;
}

bool StructDataViewPanel::saveBytesToFile(const QString& filePath,
                                          const QByteArray& bytes) {
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

void StructDataViewPanel::copySelectedScalarValuesToClipboard() const {
    QStringList lines;
    for (const VisualizedNode* node : selectedTreeNodes()) {
        if (node != nullptr && isScalarCopyNode(*node)) {
            lines.push_back(formatScalarValue(*node, StructScalarFormat::Default));
        }
    }
    if (!lines.isEmpty()) {
        QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    }
}

void StructDataViewPanel::toggleExpandCollapseAll() {
    if (m_allExpanded) {
        m_ui->structDataTreeView->collapseAll();
        m_allExpanded = false;
        m_ui->expandCollapseAllButton->setText(QStringLiteral("Expand All"));
    } else {
        m_ui->structDataTreeView->expandAll();
        m_allExpanded = true;
        m_ui->expandCollapseAllButton->setText(QStringLiteral("Collapse All"));
    }
}

void StructDataViewPanel::showTreeContextMenu(const QPoint& pos) {
    const QModelIndex clickedIndex =
        m_ui->structDataTreeView->indexAt(pos).siblingAtColumn(0);
    const VisualizedNode* clickedNode = m_model->nodeForIndex(clickedIndex);

    QMenu menu(this);
    if (clickedNode != nullptr && isScalarCopyNode(*clickedNode)) {
        menu.addAction(QStringLiteral("Copy value"), this,
                       [this, clickedNode]() {
                           copyScalarToClipboard(*clickedNode,
                                                 StructScalarFormat::Default);
                       });
        menu.addAction(QStringLiteral("Copy value (HEX)"), this,
                       [this, clickedNode]() {
                           copyScalarToClipboard(*clickedNode,
                                                 StructScalarFormat::Hex);
                       });
        menu.addAction(QStringLiteral("Copy value (Decimal)"), this,
                       [this, clickedNode]() {
                           copyScalarToClipboard(*clickedNode,
                                                 StructScalarFormat::Decimal);
                       });

        QMenu* prefixed =
            menu.addMenu(QStringLiteral("Copy file:offset:Name:type: value"));
        prefixed->addAction(QStringLiteral("HEX"), this,
                            [this, clickedNode]() {
                                copyPrefixedScalarToClipboard(
                                    *clickedNode, StructScalarFormat::Hex);
                            });
        prefixed->addAction(QStringLiteral("Decimal"), this,
                            [this, clickedNode]() {
                                copyPrefixedScalarToClipboard(
                                    *clickedNode, StructScalarFormat::Decimal);
                            });
        QMenu* stringMenu = prefixed->addMenu(QStringLiteral("String"));
        stringMenu->addAction(QStringLiteral("ASCII"), this,
                              [this, clickedNode]() {
                                  copyPrefixedScalarToClipboard(
                                      *clickedNode, StructScalarFormat::Ascii);
                              });
        stringMenu->addAction(QStringLiteral("UTF-8"), this,
                              [this, clickedNode]() {
                                  copyPrefixedScalarToClipboard(
                                      *clickedNode, StructScalarFormat::Utf8);
                              });
        stringMenu->addAction(QStringLiteral("UTF-16"), this,
                              [this, clickedNode]() {
                                  copyPrefixedScalarToClipboard(
                                      *clickedNode, StructScalarFormat::Utf16);
                              });
        menu.addSeparator();
    }

    QVector<const VisualizedNode*> thisItem;
    if (clickedNode != nullptr) {
        thisItem.push_back(clickedNode);
    }
    const QVector<const VisualizedNode*> selected = selectedTreeNodes();
    const QVector<const VisualizedNode*> allItems = m_model->topLevelNodes();

    addScopeMenu(&menu, QStringLiteral("This item"), thisItem, true,
                 clickedNode != nullptr);
    addScopeMenu(&menu, QStringLiteral("Selected items"), selected, false,
                 selected.size() > 1);
    addScopeMenu(&menu, QStringLiteral("All Items"), allItems, false,
                 !allItems.isEmpty());

    menu.exec(m_ui->structDataTreeView->viewport()->mapToGlobal(pos));
}

void StructDataViewPanel::addScopeMenu(
    QMenu* parent, const QString& label,
    const QVector<const VisualizedNode*>& nodes, bool writeSingleJsonObject,
    bool enabled) {
    QMenu* scopeMenu = parent->addMenu(label);
    scopeMenu->setEnabled(enabled);
    scopeMenu->addAction(QStringLiteral("Copy as JSON"), this,
                         [this, nodes, writeSingleJsonObject]() {
                             copyJsonToClipboard(nodes, writeSingleJsonObject);
                         });
    scopeMenu->addAction(QStringLiteral("Save as JSON"), this,
                         [this, nodes, writeSingleJsonObject]() {
                             saveJson(nodes, writeSingleJsonObject);
                         });
    addOutformMenu(scopeMenu, nodes);
    scopeMenu->addAction(
        QStringLiteral("Save as binary struct (source endianness)"), this,
        [this, nodes]() {
            saveBinary(nodes, StructBinaryExportMode::SourceEndianness);
        });
    scopeMenu->addAction(
        QStringLiteral("Save as binary (declared endianness)"), this,
        [this, nodes]() {
            saveBinary(nodes, StructBinaryExportMode::DeclaredEndianness);
        });
}

void StructDataViewPanel::addOutformMenu(
    QMenu* parent, const QVector<const VisualizedNode*>& nodes) {
    QMenu* outformMenu = parent->addMenu(QStringLiteral("outform..."));
    if (m_outforms.isEmpty()) {
        QAction* emptyAction =
            outformMenu->addAction(QStringLiteral("no outforms declared"));
        emptyAction->setEnabled(false);
        return;
    }
    for (const OutformNode& outform : m_outforms) {
        const QString fileName = outform.sourceFilePath.isEmpty()
                                     ? QStringLiteral("inline script")
                                     : QFileInfo(outform.sourceFilePath).fileName();
        outformMenu->addAction(
            QStringLiteral("%1: %2").arg(fileName, outform.name), this,
            [this, outform, nodes]() { saveUsingOutform(outform, nodes); });
    }
}

void StructDataViewPanel::saveUsingOutform(
    const OutformNode& outform, const QVector<const VisualizedNode*>& nodes) {
    QString rendered;
    for (const VisualizedNode* node : nodes) {
        if (node == nullptr) {
            continue;
        }
        QString error;
        rendered += renderStructureTemplate(outform.templateText, *node, &error);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Template error"), error);
            return;
        }
    }
    const QString outputPath = m_outformSavePathForTests.isEmpty()
                                   ? QFileDialog::getSaveFileName(
                                         this,
                                         QStringLiteral("Save using outform '%1'")
                                             .arg(outform.name),
                                         QString(),
                                         QStringLiteral("Text files (*.txt);;All files (*)"))
                                   : m_outformSavePathForTests;
    if (!outputPath.isEmpty() && !saveBytesToFile(outputPath, rendered.toUtf8())) {
        QMessageBox::warning(this, QStringLiteral("Save failed"),
                             QStringLiteral("Could not write '%1'.").arg(outputPath));
    }
}

QVector<const VisualizedNode*> StructDataViewPanel::selectedTreeNodes() const {
    if (m_ui->structDataTreeView->selectionModel() == nullptr) {
        return {};
    }
    return m_model->nodesForIndexes(
        m_ui->structDataTreeView->selectionModel()->selectedRows(0));
}

QByteArray StructDataViewPanel::jsonForNodes(
    const QVector<const VisualizedNode*>& nodes, bool writeSingleObject) const {
    if (writeSingleObject && nodes.size() == 1 && nodes.first() != nullptr) {
        return serializeVisualizedNode(*nodes.first());
    }
    return serializeVisualizedNodes(nodes);
}

void StructDataViewPanel::saveJson(const QVector<const VisualizedNode*>& nodes,
                                   bool writeSingleObject) {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save File"), QString(),
        QStringLiteral("JSON files (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (!saveBytesToFile(path, jsonForNodes(nodes, writeSingleObject))) {
        QMessageBox::warning(this, QStringLiteral("Save failed"),
                             QStringLiteral("Could not write '%1'.").arg(path));
    }
}

void StructDataViewPanel::saveBinary(
    const QVector<const VisualizedNode*>& nodes, StructBinaryExportMode mode) {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save File"), QString(),
        QStringLiteral("Binary files (*.bin);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (!saveBytesToFile(path,
                         exportVisualizedBytes(nodes, mode, m_sourceEndianness))) {
        QMessageBox::warning(this, QStringLiteral("Save failed"),
                             QStringLiteral("Could not write '%1'.").arg(path));
    }
}

void StructDataViewPanel::copyJsonToClipboard(
    const QVector<const VisualizedNode*>& nodes, bool writeSingleObject) const {
    QGuiApplication::clipboard()->setText(
        QString::fromUtf8(jsonForNodes(nodes, writeSingleObject)));
}

void StructDataViewPanel::copyScalarToClipboard(
    const VisualizedNode& node, StructScalarFormat format) const {
    QGuiApplication::clipboard()->setText(formatScalarValue(node, format));
}

void StructDataViewPanel::copyPrefixedScalarToClipboard(
    const VisualizedNode& node, StructScalarFormat format) const {
    QGuiApplication::clipboard()->setText(
        formatPrefixedScalarValue(node, format));
}

}  // namespace breco

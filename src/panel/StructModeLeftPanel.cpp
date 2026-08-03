#include "panel/StructModeLeftPanel.h"

#include "settings/AppSettings.h"
#include "struct/StructDeclarationParser.h"
#include "struct/StructureLibrary.h"
#include "ui_StructModeLeftPanel.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDrag>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFormat>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHBoxLayout>

#include <algorithm>

namespace breco {

namespace {

const QString kLanguageSnippetMime =
    QStringLiteral("application/x-breco-struct-language-snippet");
constexpr int kViewIdRole = Qt::UserRole;
constexpr int kOffsetRole = Qt::UserRole + 1;
constexpr int kFilePathRole = Qt::UserRole + 2;
constexpr int kByteLengthRole = Qt::UserRole + 3;

QString offsetText(quint64 offset) {
    return QStringLiteral("0x%1").arg(offset, 0, 16).toUpper();
}

const QString kStructDeclarationFileFilter =
    QStringLiteral("BrecoStruct (*.brecostruct);;BrecoScript (*.brecoscript);;Text (*.txt);;All files (*)");

QString structDeclarationDialogStartPath() {
    const QString lastPath = AppSettings::lastStructDefinitionFilePath();
    if (!lastPath.isEmpty()) {
        return lastPath;
    }
    return AppSettings::lastStructDefinitionDialogDirectory();
}

}  // namespace

StructModeLeftPanel::StructModeLeftPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::StructModeLeftPanel>()) {
    m_ui->setupUi(this);

    // Designer owns the three substantial sections. Move them into a splitter
    // and add the dynamically built library as its first pane.
    m_sectionSplitter = new QSplitter(Qt::Vertical, this);
    m_sectionSplitter->setObjectName(QStringLiteral("structSectionsSplitter"));
    m_sectionSplitter->setChildrenCollapsible(false);
    m_ui->structModeLeftPanelLayout->insertWidget(1, m_sectionSplitter, 1);
    setupStructureLibrary();
    m_sectionSplitter->addWidget(m_ui->structEditorWidgetPlaceholder);
    m_sectionSplitter->addWidget(m_ui->structViewEntriesWidget);
    m_sectionSplitter->addWidget(m_ui->languageReferenceWidget);
    m_sectionSplitter->setStretchFactor(0, 1);
    m_sectionSplitter->setStretchFactor(1, 2);
    m_sectionSplitter->setStretchFactor(2, 1);
    m_sectionSplitter->setStretchFactor(3, 1);

    // Keep the toggle/action strip at its natural one-row height; the splitter
    // receives all surplus vertical space.
    m_ui->sectionControlsLayout->setContentsMargins(0, 0, 0, 0);
    m_ui->sectionControlsLayout->setVerticalSpacing(0);

    m_ui->previewEnabledCheckBox->setChecked(
        AppSettings::structPreviewEnabled());
    m_ui->viewsCheckBox->setChecked(AppSettings::structViewsVisible());
    m_ui->languageCheckBox->setChecked(
        AppSettings::structLanguageVisible());
    m_ui->structEditorWidgetPlaceholder->setVisible(m_ui->editorCheckBox->isChecked());
    m_ui->structViewEntriesWidget->setVisible(m_ui->viewsCheckBox->isChecked());
    m_ui->languageReferenceWidget->setVisible(m_ui->languageCheckBox->isChecked());
    m_libraryWidget->setVisible(m_ui->libraryCheckBox->isChecked());
    connect(m_ui->libraryCheckBox, &QCheckBox::toggled,
            m_libraryWidget, &QWidget::setVisible);
    connect(m_ui->editorCheckBox, &QCheckBox::toggled,
            m_ui->structEditorWidgetPlaceholder, &QWidget::setVisible);
    connect(m_ui->viewsCheckBox, &QCheckBox::toggled,
            m_ui->structViewEntriesWidget, &QWidget::setVisible);
    connect(m_ui->viewsCheckBox, &QCheckBox::toggled,
            this, [](bool checked) {
                AppSettings::setStructViewsVisible(checked);
            });
    connect(m_ui->languageCheckBox, &QCheckBox::toggled,
            m_ui->languageReferenceWidget, &QWidget::setVisible);
    connect(m_ui->languageCheckBox, &QCheckBox::toggled,
            this, [](bool checked) {
                AppSettings::setStructLanguageVisible(checked);
            });

    connect(m_ui->loadStructDefinitionButton, &QToolButton::clicked,
            this, &StructModeLeftPanel::handleLoadRequested);
    connect(m_ui->saveStructDefinitionButton, &QToolButton::clicked,
            this, &StructModeLeftPanel::handleSaveRequested);
    connect(m_ui->structDeclarationEdit, &QPlainTextEdit::textChanged, this,
            [this]() { reparseDeclaration(); });
    connect(m_ui->entryComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
                highlightSelectedEntry();
                updatePreviewControls();
            });
    connect(m_ui->scanStructureButton, &QToolButton::clicked, this, [this]() {
        if (m_structureScanRunning) {
            emit structureScanStopRequested();
        } else if (!m_scanRunning && m_ui->scanStructureButton->isEnabled()) {
            emit structureScanRequested();
        }
    });
    connect(m_ui->previewEnabledCheckBox, &QCheckBox::toggled, this,
            [this](bool checked) {
                AppSettings::setStructPreviewEnabled(checked);
                updatePreviewControls();
                if (checked && canPreview()) {
                    emit previewRequested();
                } else if (!checked) {
                    emit previewClearRequested();
                }
            });
    connect(m_ui->addViewButton, &QToolButton::clicked, this,
            &StructModeLeftPanel::addViewRequested);
    connect(m_ui->removeViewButton, &QToolButton::clicked, this,
            &StructModeLeftPanel::removeSelectedViews);

    m_ui->currentViewsTableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ui->currentViewsTableWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_ui->currentViewsTableWidget->setEditTriggers(
        QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
        QAbstractItemView::SelectedClicked);
    m_ui->currentViewsTableWidget->horizontalHeader()->setStretchLastSection(true);
    connect(m_ui->currentViewsTableWidget, &QTableWidget::itemSelectionChanged,
            this, &StructModeLeftPanel::updateRemoveEnabled);
    connect(m_ui->currentViewsTableWidget, &QTableWidget::cellClicked, this,
            [this](int row, int) {
                const CurrentStructView view = viewForRow(row);
                emit sourceLocationActivated(view.filePath, view.offset,
                                             view.byteLength);
            });
    connect(m_ui->currentViewsTableWidget, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem* item) {
                if (m_updatingTable || item == nullptr) {
                    return;
                }
                const int row = item->row();
                if (item->column() == 3) {
                    quint64 offset = 0;
                    if (!parseAbsoluteOffset(item->text(), &offset)) {
                        m_updatingTable = true;
                        item->setText(offsetText(item->data(kOffsetRole).toULongLong()));
                        item->setToolTip(QStringLiteral("Enter a decimal or 0x hexadecimal offset"));
                        m_updatingTable = false;
                        return;
                    }
                    m_updatingTable = true;
                    item->setText(offsetText(offset));
                    item->setData(kOffsetRole, QVariant::fromValue<qulonglong>(offset));
                    item->setToolTip(QString());
                    m_updatingTable = false;
                }
                emitViewChangedForRow(row);
            });

    setupLanguageReference();
    updateStatusAndErrorHighlight();
    updatePreviewControls();
    updateRemoveEnabled();
}

StructModeLeftPanel::~StructModeLeftPanel() = default;

QPlainTextEdit* StructModeLeftPanel::structDeclarationEdit() const {
    return m_ui->structDeclarationEdit;
}

QComboBox* StructModeLeftPanel::entryComboBox() const { return m_ui->entryComboBox; }

QSpinBox* StructModeLeftPanel::entryCountSpinBox() const {
    return m_ui->entryCountSpinBox;
}

QCheckBox* StructModeLeftPanel::previewEnabledCheckBox() const {
    return m_ui->previewEnabledCheckBox;
}

QTableWidget* StructModeLeftPanel::currentViewsTableWidget() const {
    return m_ui->currentViewsTableWidget;
}

QToolButton* StructModeLeftPanel::addViewButton() const { return m_ui->addViewButton; }

QToolButton* StructModeLeftPanel::removeViewButton() const {
    return m_ui->removeViewButton;
}

QToolButton* StructModeLeftPanel::scanStructureButton() const {
    return m_ui->scanStructureButton;
}

QVBoxLayout* StructModeLeftPanel::structDeclarationLayout() const {
    return m_ui->structDeclarationLayout;
}

bool StructModeLeftPanel::canPreview() const {
    return m_parseValid && !m_graph.entryNames().isEmpty() &&
           m_ui->entryComboBox->currentIndex() >= 0;
}

bool StructModeLeftPanel::previewEnabled() const {
    return m_ui->previewEnabledCheckBox->isChecked();
}

QString StructModeLeftPanel::declarationText() const {
    return m_ui->structDeclarationEdit->toPlainText();
}

QVector<CurrentStructView> StructModeLeftPanel::currentViews() const {
    QVector<CurrentStructView> views;
    views.reserve(m_ui->currentViewsTableWidget->rowCount());
    for (int row = 0; row < m_ui->currentViewsTableWidget->rowCount(); ++row) {
        views.push_back(viewForRow(row));
    }
    return views;
}

void StructModeLeftPanel::reparseDeclaration() {
    const QString text = m_ui->structDeclarationEdit->toPlainText();
    m_isEmpty = text.trimmed().isEmpty();
    const ParseResult result = parseStructDeclaration(text);
    m_parseValid = result.valid;
    m_parseError = result.errorMessage;
    m_parseErrorRange = result.errorRange;
    if (result.valid) {
        m_graph = result.graph;
    } else {
        m_graph.clear();
    }
    updateEntryCombo();
    updateStatusAndErrorHighlight();
    highlightSelectedEntry();
    updatePreviewControls();
    emit parseStateChanged();
}

void StructModeLeftPanel::updateEntryCombo() {
    const QString previous = m_ui->entryComboBox->currentText();
    m_ui->entryComboBox->blockSignals(true);
    m_ui->entryComboBox->clear();
    if (m_parseValid) {
        m_ui->entryComboBox->addItems(m_graph.entryNames());
        const QString selectedEntry =
            m_graph.defaultEntryName().isEmpty()
                ? previous
                : m_graph.defaultEntryName();
        const int idx = m_ui->entryComboBox->findText(selectedEntry);
        if (idx >= 0) {
            m_ui->entryComboBox->setCurrentIndex(idx);
        }
    }
    m_ui->entryComboBox->blockSignals(false);
}

void StructModeLeftPanel::setPreviewActive(bool active) {
    if (m_previewActive == active) {
        updatePreviewControls();
        return;
    }
    m_previewActive = active;
    updatePreviewControls();
}

void StructModeLeftPanel::updatePreviewControls() {
    m_ui->previewEnabledCheckBox->setEnabled(canPreview());
    m_ui->addViewButton->setEnabled(previewEnabled() && canPreview());
    m_ui->scanStructureButton->setEnabled(
        m_structureScanRunning ||
        (!m_scanRunning && m_parseValid &&
         m_graph.entryHasEffectiveScanConstraint(
             m_ui->entryComboBox->currentText())));
}

void StructModeLeftPanel::setScanState(bool running, bool structureScan) {
    m_scanRunning = running;
    m_structureScanRunning = running && structureScan;
    m_ui->scanStructureButton->setText(
        m_structureScanRunning ? QStringLiteral("Stop") : QStringLiteral("Scan"));
    updatePreviewControls();
}

void StructModeLeftPanel::updateRemoveEnabled() {
    m_ui->removeViewButton->setEnabled(
        m_ui->currentViewsTableWidget->rowCount() > 0 &&
        m_ui->currentViewsTableWidget->selectionModel() != nullptr &&
        m_ui->currentViewsTableWidget->selectionModel()->hasSelection());
}

void StructModeLeftPanel::addCurrentView(const CurrentStructView& view) {
    m_updatingTable = true;
    const int row = m_ui->currentViewsTableWidget->rowCount();
    m_ui->currentViewsTableWidget->insertRow(row);

    auto* nameItem = new QTableWidgetItem(view.name);
    nameItem->setData(kViewIdRole, QVariant::fromValue<qulonglong>(view.id));
    nameItem->setData(kFilePathRole, view.filePath);
    nameItem->setData(kByteLengthRole,
                      QVariant::fromValue<qulonglong>(view.byteLength));
    m_ui->currentViewsTableWidget->setItem(row, 0, nameItem);

    auto* typeItem = new QTableWidgetItem(view.type);
    typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
    m_ui->currentViewsTableWidget->setItem(row, 1, typeItem);

    auto* repeatSpin = new QSpinBox(m_ui->currentViewsTableWidget);
    repeatSpin->setRange(m_ui->entryCountSpinBox->minimum(),
                         m_ui->entryCountSpinBox->maximum());
    repeatSpin->setValue(view.repeat);
    m_ui->currentViewsTableWidget->setCellWidget(row, 2, repeatSpin);
    connect(repeatSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, id = view.id](int) {
                const int changedRow = rowForViewId(id);
                if (changedRow >= 0) {
                    emitViewChangedForRow(changedRow);
                }
            });

    auto* offsetItem = new QTableWidgetItem(offsetText(view.offset));
    offsetItem->setData(kOffsetRole, QVariant::fromValue<qulonglong>(view.offset));
    m_ui->currentViewsTableWidget->setItem(row, 3, offsetItem);
    m_updatingTable = false;
    updateRemoveEnabled();
}

void StructModeLeftPanel::setCurrentViewByteLength(quint64 id,
                                                   quint64 byteLength) {
    const int row = rowForViewId(id);
    QTableWidgetItem* nameItem =
        row >= 0 ? m_ui->currentViewsTableWidget->item(row, 0) : nullptr;
    if (nameItem == nullptr) {
        return;
    }
    m_updatingTable = true;
    nameItem->setData(kByteLengthRole,
                      QVariant::fromValue<qulonglong>(byteLength));
    m_updatingTable = false;
}

void StructModeLeftPanel::clearCurrentViews() {
    m_updatingTable = true;
    m_ui->currentViewsTableWidget->setRowCount(0);
    m_updatingTable = false;
    updateRemoveEnabled();
}

int StructModeLeftPanel::rowForViewId(quint64 id) const {
    for (int row = 0; row < m_ui->currentViewsTableWidget->rowCount(); ++row) {
        const QTableWidgetItem* item = m_ui->currentViewsTableWidget->item(row, 0);
        if (item != nullptr && item->data(kViewIdRole).toULongLong() == id) {
            return row;
        }
    }
    return -1;
}

CurrentStructView StructModeLeftPanel::viewForRow(int row) const {
    CurrentStructView view;
    const QTableWidgetItem* nameItem = m_ui->currentViewsTableWidget->item(row, 0);
    const QTableWidgetItem* typeItem = m_ui->currentViewsTableWidget->item(row, 1);
    const auto* repeatSpin =
        qobject_cast<QSpinBox*>(m_ui->currentViewsTableWidget->cellWidget(row, 2));
    const QTableWidgetItem* offsetItem = m_ui->currentViewsTableWidget->item(row, 3);
    if (nameItem != nullptr) {
        view.id = nameItem->data(kViewIdRole).toULongLong();
        view.name = nameItem->text();
        view.filePath = nameItem->data(kFilePathRole).toString();
        view.byteLength = nameItem->data(kByteLengthRole).toULongLong();
    }
    if (typeItem != nullptr) {
        view.type = typeItem->text();
    }
    if (repeatSpin != nullptr) {
        view.repeat = repeatSpin->value();
    }
    if (offsetItem != nullptr) {
        view.offset = offsetItem->data(kOffsetRole).toULongLong();
    }
    return view;
}

void StructModeLeftPanel::emitViewChangedForRow(int row) {
    if (row < 0 || row >= m_ui->currentViewsTableWidget->rowCount()) {
        return;
    }
    const CurrentStructView view = viewForRow(row);
    emit currentViewChanged(view.id, view.name, view.repeat, view.offset);
}

void StructModeLeftPanel::removeSelectedViews() {
    const QModelIndexList selected =
        m_ui->currentViewsTableWidget->selectionModel()->selectedRows();
    QVector<int> rows;
    QVector<quint64> ids;
    rows.reserve(selected.size());
    ids.reserve(selected.size());
    for (const QModelIndex& index : selected) {
        rows.push_back(index.row());
        const QTableWidgetItem* item =
            m_ui->currentViewsTableWidget->item(index.row(), 0);
        if (item != nullptr) {
            ids.push_back(item->data(kViewIdRole).toULongLong());
        }
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    m_updatingTable = true;
    for (int row : rows) {
        m_ui->currentViewsTableWidget->removeRow(row);
    }
    m_updatingTable = false;
    updateRemoveEnabled();
    if (!ids.isEmpty()) {
        emit currentViewsRemoved(ids);
    }
}

bool StructModeLeftPanel::loadDeclarationFromFile(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return false;
    }
    m_ui->structDeclarationEdit->setPlainText(QString::fromUtf8(bytes));
    const ParseResult parsed = parseStructDeclarationFile(filePath);
    if (parsed.valid) {
        m_graph = parsed.graph;
        m_parseValid = true;
        m_parseError.clear();
        updateEntryCombo();
        updateStatusAndErrorHighlight();
        highlightSelectedEntry();
        updatePreviewControls();
        emit parseStateChanged();
    }
    emit declarationFileLoaded(QFileInfo(filePath).absoluteFilePath());
    return true;
}

void StructModeLeftPanel::setupStructureLibrary() {
    auto* container = new QWidget(m_sectionSplitter);
    container->setObjectName(QStringLiteral("structureLibraryWidget"));
    m_libraryWidget = container;
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* controls = new QHBoxLayout();
    auto* label = new QLabel(QStringLiteral("Structure library"), container);
    auto* directoryButton = new QToolButton(container);
    directoryButton->setText(QStringLiteral("Directory..."));
    auto* refreshButton = new QToolButton(container);
    refreshButton->setText(QStringLiteral("Refresh"));
    controls->addWidget(label);
    controls->addStretch();
    controls->addWidget(directoryButton);
    controls->addWidget(refreshButton);
    m_libraryTree = new QTreeWidget(container);
    m_libraryTree->setHeaderLabels({QStringLiteral("File / structure")});
    m_libraryTree->setRootIsDecorated(true);
    layout->addLayout(controls);
    layout->addWidget(m_libraryTree);
    m_sectionSplitter->addWidget(container);
    connect(directoryButton, &QToolButton::clicked, this,
            &StructModeLeftPanel::chooseStructureLibraryDirectory);
    connect(refreshButton, &QToolButton::clicked, this,
            &StructModeLeftPanel::refreshStructureLibrary);
    connect(m_libraryTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (item == nullptr) {
                    return;
                }
                const QString path = item->data(0, Qt::UserRole).toString();
                if (!path.isEmpty()) {
                    loadDeclarationFromFile(path);
                }
            });
    refreshStructureLibrary();
}

void StructModeLeftPanel::refreshStructureLibrary() {
    QString directory = AppSettings::structureLibraryDirectory();
    if (directory.isEmpty()) {
        directory = StructureLibrary::defaultDirectory();
    }
    StructureLibrary::ensureDirectory(directory);
    StructureLibrary library(directory);
    m_libraryTree->clear();
    for (const StructureLibraryFile& file : library.scan()) {
        auto* fileItem = new QTreeWidgetItem(m_libraryTree, {file.relativePath});
        fileItem->setData(0, Qt::UserRole, file.filePath);
        if (!file.errorMessage.isEmpty()) {
            fileItem->setToolTip(0, file.errorMessage);
        }
        for (const QString& entry : file.entries) {
            auto* entryItem = new QTreeWidgetItem(fileItem, {entry});
            entryItem->setData(0, Qt::UserRole, file.filePath);
        }
    }
}

void StructModeLeftPanel::chooseStructureLibraryDirectory() {
    const QString current = AppSettings::structureLibraryDirectory().isEmpty()
                                ? StructureLibrary::defaultDirectory()
                                : AppSettings::structureLibraryDirectory();
    const QString selected = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select structure library directory"), current);
    if (selected.isEmpty()) {
        return;
    }
    AppSettings::setStructureLibraryDirectory(QFileInfo(selected).absoluteFilePath());
    refreshStructureLibrary();
}

bool StructModeLeftPanel::saveDeclarationToFile(const QString& filePath) const {
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray text = declarationText().toUtf8();
    if (file.write(text) != text.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

void StructModeLeftPanel::handleLoadRequested() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load struct declaration"),
        AppSettings::lastStructDefinitionDialogDirectory(), kStructDeclarationFileFilter);
    if (path.isEmpty()) {
        return;
    }
    if (!loadDeclarationFromFile(path)) {
        QMessageBox::warning(this, QStringLiteral("Load failed"),
                             QStringLiteral("Could not read '%1'.").arg(path));
    }
}

void StructModeLeftPanel::handleSaveRequested() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save struct declaration"), structDeclarationDialogStartPath(),
        kStructDeclarationFileFilter);
    if (path.isEmpty()) {
        return;
    }
    if (!saveDeclarationToFile(path)) {
        QMessageBox::warning(this, QStringLiteral("Save failed"),
                             QStringLiteral("Could not write '%1'.").arg(path));
        return;
    }
    AppSettings::setLastStructDefinitionFilePath(QFileInfo(path).absoluteFilePath());
}

bool StructModeLeftPanel::parseAbsoluteOffset(const QString& text, quint64* offset) {
    if (offset == nullptr) {
        return false;
    }
    QString value = text.trimmed();
    int base = 10;
    if (value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        value.remove(0, 2);
        base = 16;
    }
    if (value.isEmpty()) {
        return false;
    }
    bool ok = false;
    const quint64 parsed = value.toULongLong(&ok, base);
    if (!ok) {
        return false;
    }
    *offset = parsed;
    return true;
}

QString StructModeLeftPanel::languageSnippetForObjectName(const QString& objectName) {
    static const QHash<QString, QString> snippets = {
        {QStringLiteral("structKeywordLabel"),
         QStringLiteral("struct Name { uint8 value; }")},
        {QStringLiteral("typedefKeywordLabel"),
         QStringLiteral("typedef uint8 Alias;")},
        {QStringLiteral("bigEndianModifierLabel"),
         QStringLiteral("uint16<be> value;")},
        {QStringLiteral("littleEndianModifierLabel"),
         QStringLiteral("uint16<le> value;")},
        {QStringLiteral("uint8TypeLabel"), QStringLiteral("uint8 value;")},
        {QStringLiteral("uint16TypeLabel"), QStringLiteral("uint16 value;")},
        {QStringLiteral("uint32TypeLabel"), QStringLiteral("uint32 value;")},
        {QStringLiteral("uint64TypeLabel"), QStringLiteral("uint64 value;")},
        {QStringLiteral("int8TypeLabel"), QStringLiteral("int8 value;")},
        {QStringLiteral("int16TypeLabel"), QStringLiteral("int16 value;")},
        {QStringLiteral("int32TypeLabel"), QStringLiteral("int32 value;")},
        {QStringLiteral("int64TypeLabel"), QStringLiteral("int64 value;")},
        {QStringLiteral("asciiStringTypeLabel"), QStringLiteral("asciistr value;")},
        {QStringLiteral("utf8StringTypeLabel"), QStringLiteral("utf8str value;")},
        {QStringLiteral("utf16StringTypeLabel"), QStringLiteral("utf16str value;")},
        {QStringLiteral("byteTypeLabel"), QStringLiteral("byte value;")},
        {QStringLiteral("maxLengthModifierLabel"),
         QStringLiteral("asciistr<max:1> value;")},
        {QStringLiteral("fixedLengthModifierLabel"),
         QStringLiteral("byte<len:1> value;")},
        {QStringLiteral("untilExpressionModifierLabel"),
         QStringLiteral("byte<until:=0> value;")},
        {QStringLiteral("variableDirectiveLabel"),
         QStringLiteral("/var(value) uint8 field;")},
        {QStringLiteral("repeatDirectiveLabel"),
         QStringLiteral("/repeat(1) uint8 value;")},
        {QStringLiteral("conditionDirectiveLabel"),
         QStringLiteral("/cond(=0) uint8 value;")},
        {QStringLiteral("whenDirectiveLabel"),
         QStringLiteral("struct Maybe { /var(flag) uint8 flag; /when($flag = 1) uint8 value; }")},
        {QStringLiteral("assertDirectiveLabel"),
         QStringLiteral("struct Checked { /var(length) uint8 length; /assert($length = 1); }")},
        {QStringLiteral("defaultDirectiveLabel"),
         QStringLiteral("/default Packet\nstruct Packet { uint8 value; }")},
        {QStringLiteral("bitfieldBlockLabel"),
         QStringLiteral("struct Flags { uint8 flags { bit 0 enabled; } }")},
    };
    return snippets.value(objectName);
}

void StructModeLeftPanel::setupLanguageReference() {
    m_ui->languageReferenceWidget->setStyleSheet(QStringLiteral(
        "QLabel {"
        " background: palette(alternate-base);"
        " border: 1px solid palette(mid);"
        " border-radius: 5px;"
        " padding: 3px 6px;"
        "}"));
    const QList<QLabel*> labels =
        m_ui->languageReferenceWidget->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        const QString snippet = languageSnippetForObjectName(label->objectName());
        if (snippet.isEmpty()) {
            continue;
        }
        m_languageSnippets.insert(label, snippet);
        label->setCursor(Qt::OpenHandCursor);
        label->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        label->installEventFilter(this);
    }
    m_ui->structDeclarationEdit->setAcceptDrops(true);
    m_ui->structDeclarationEdit->viewport()->setAcceptDrops(true);
    m_ui->structDeclarationEdit->viewport()->installEventFilter(this);
}

bool StructModeLeftPanel::eventFilter(QObject* watched, QEvent* event) {
    if (m_languageSnippets.contains(watched)) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                m_dragStartPosition = mouse->position().toPoint();
                m_dragLabel = qobject_cast<QLabel*>(watched);
                return true;
            }
        } else if (event->type() == QEvent::MouseMove && m_dragLabel != nullptr) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if ((mouse->buttons() & Qt::LeftButton) != 0 &&
                (mouse->position().toPoint() - m_dragStartPosition).manhattanLength() >=
                    QApplication::startDragDistance()) {
                auto* drag = new QDrag(m_dragLabel);
                auto* mime = new QMimeData();
                const QString snippet = m_languageSnippets.value(m_dragLabel);
                mime->setData(kLanguageSnippetMime, snippet.toUtf8());
                mime->setText(snippet);
                drag->setMimeData(mime);
                drag->exec(Qt::CopyAction);
                m_dragLabel = nullptr;
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            m_dragLabel = nullptr;
        }
    }

    if (watched == m_ui->structDeclarationEdit->viewport()) {
        if (event->type() == QEvent::DragEnter) {
            auto* drag = static_cast<QDragEnterEvent*>(event);
            if (drag->mimeData()->hasFormat(kLanguageSnippetMime)) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* drop = static_cast<QDropEvent*>(event);
            if (drop->mimeData()->hasFormat(kLanguageSnippetMime)) {
                insertLanguageSnippet(
                    QString::fromUtf8(drop->mimeData()->data(kLanguageSnippetMime)));
                drop->acceptProposedAction();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void StructModeLeftPanel::insertLanguageSnippet(const QString& snippet) {
    if (snippet.isEmpty()) {
        return;
    }
    QTextCursor cursor = m_ui->structDeclarationEdit->textCursor();
    if (m_ui->structDeclarationEdit->toPlainText().isEmpty()) {
        cursor.movePosition(QTextCursor::Start);
        cursor.insertText(snippet);
    } else {
        cursor.movePosition(QTextCursor::EndOfBlock);
        cursor.insertText(QLatin1Char('\n') + snippet);
    }
    m_ui->structDeclarationEdit->setTextCursor(cursor);
    m_ui->structDeclarationEdit->setFocus();
}

void StructModeLeftPanel::updateStatusAndErrorHighlight() {
    QList<QTextEdit::ExtraSelection> selections;
    if (m_isEmpty || m_parseValid) {
        const QString text = m_ui->structDeclarationEdit->toPlainText();
        const int lineCount =
            text.isEmpty() ? 0 : text.count(QLatin1Char('\n')) + 1;
        m_ui->structDeclarationStatusLabel->setText(
            QStringLiteral("%1 lines, no errors").arg(lineCount));
        m_ui->structDeclarationStatusLabel->setVisible(true);
    } else {
        int errorPosition = m_parseErrorRange.start;
        if (errorPosition < 0) {
            errorPosition = 0;
        }
        const int maxPosition = m_ui->structDeclarationEdit->toPlainText().size();
        if (errorPosition > maxPosition) {
            errorPosition = maxPosition;
        }

        QTextCursor errorCursor(m_ui->structDeclarationEdit->document());
        errorCursor.setPosition(errorPosition);
        const QString errorMessage =
            m_parseError.isEmpty()
                ? QStringLiteral("Invalid struct declaration")
                : m_parseError;
        m_ui->structDeclarationStatusLabel->setText(
            QStringLiteral("Line %1: %2")
                .arg(errorCursor.blockNumber() + 1)
                .arg(errorMessage));
        m_ui->structDeclarationStatusLabel->setVisible(true);

        QTextEdit::ExtraSelection errorLine;
        errorLine.cursor = errorCursor;
        errorLine.cursor.select(QTextCursor::LineUnderCursor);
        errorLine.format.setBackground(QColor(255, 80, 80, 90));
        selections.push_back(errorLine);
    }
    if (!m_parseValid) {
        m_ui->structDeclarationEdit->setExtraSelections(selections);
    }
}

void StructModeLeftPanel::highlightSelectedEntry() {
    QList<QTextEdit::ExtraSelection> selections;
    if (!m_parseValid) {
        updateStatusAndErrorHighlight();
        return;
    }
    if (m_ui->entryComboBox->currentIndex() < 0) {
        m_ui->structDeclarationEdit->setExtraSelections(selections);
        return;
    }
    const QString entryName = m_ui->entryComboBox->currentText();
    const TextRange range = m_graph.nameRangeForEntry(entryName);
    if (range.start < 0 || range.end <= range.start) {
        m_ui->structDeclarationEdit->setExtraSelections(selections);
        return;
    }
    QTextEdit::ExtraSelection selection;
    selection.cursor = m_ui->structDeclarationEdit->textCursor();
    selection.cursor.setPosition(range.start);
    selection.cursor.setPosition(range.end, QTextCursor::KeepAnchor);
    selection.format.setBackground(QColor(255, 255, 0, 80));
    selections.push_back(selection);
    m_ui->structDeclarationEdit->setExtraSelections(selections);
}

void StructModeLeftPanel::focusDeclarationRange(int start, int end) {
    if (!m_parseValid || start < 0 || end <= start) {
        return;
    }
    const int maxPosition = m_ui->structDeclarationEdit->toPlainText().size();
    if (start > maxPosition) {
        return;
    }

    QTextCursor focusCursor(m_ui->structDeclarationEdit->document());
    focusCursor.setPosition(qBound(0, start, maxPosition));
    QTextCursor lineCursor = focusCursor;
    lineCursor.select(QTextCursor::LineUnderCursor);

    QTextEdit::ExtraSelection selection;
    selection.cursor = lineCursor;
    selection.format.setBackground(QColor(255, 255, 0, 80));
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    m_ui->structDeclarationEdit->setExtraSelections({selection});
    m_ui->structDeclarationEdit->setTextCursor(focusCursor);
    m_ui->structDeclarationEdit->centerCursor();
}

}  // namespace breco

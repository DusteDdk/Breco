#include "brecolang/gui/BrecoLangPanel.h"

#include "brecolang/compiler/Compiler.h"
#include "brecolang/gui/BrecoLangLibrary.h"
#include "brecolang/gui/DecodedTreeModel.h"
#include "brecolang/render/OutformRenderer.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace breco::lang {

namespace {

QString compileMessages(const QVector<Diagnostic>& diagnostics) {
    QStringList messages;
    for (const Diagnostic& diagnostic : diagnostics) {
        messages.push_back(QStringLiteral("%1: %2 (source offset %3)")
                               .arg(diagnostic.code, diagnostic.message)
                               .arg(diagnostic.span.start));
    }
    return messages.join(QLatin1Char('\n'));
}

QString runtimeMessages(const QVector<RuntimeDiagnostic>& diagnostics) {
    QStringList messages;
    for (const RuntimeDiagnostic& diagnostic : diagnostics) {
        messages.push_back(
            QStringLiteral("%1: %2").arg(diagnostic.code, diagnostic.message));
    }
    return messages.join(QLatin1Char('\n'));
}

}  // namespace

BrecoLangPanel::BrecoLangPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("brecoLangPanel"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);

    auto* schemaRow = new QHBoxLayout();
    schemaRow->addWidget(new QLabel(QStringLiteral("Schema"), this));
    m_schemaPath = new QLineEdit(this);
    m_schemaPath->setObjectName(QStringLiteral("brecoLangSchemaPath"));
    m_schemaPath->setPlaceholderText(QStringLiteral("Select a .breco schema"));
    schemaRow->addWidget(m_schemaPath, 1);
    auto* browseSchema = new QPushButton(QStringLiteral("Browse…"), this);
    auto* loadSchema = new QPushButton(QStringLiteral("Load"), this);
    schemaRow->addWidget(browseSchema);
    schemaRow->addWidget(loadSchema);
    outer->addLayout(schemaRow);

    auto* libraryRow = new QHBoxLayout();
    libraryRow->addWidget(new QLabel(QStringLiteral("Library"), this));
    m_libraryPath = new QLineEdit(this);
    m_libraryPath->setObjectName(QStringLiteral("brecoLangLibraryPath"));
    libraryRow->addWidget(m_libraryPath, 1);
    auto* chooseLibrary = new QPushButton(QStringLiteral("Directory…"), this);
    auto* refreshLibraryButton = new QPushButton(QStringLiteral("Refresh"), this);
    m_libraryCombo = new QComboBox(this);
    m_libraryCombo->setObjectName(QStringLiteral("brecoLangLibraryCombo"));
    m_libraryCombo->setMinimumWidth(220);
    libraryRow->addWidget(chooseLibrary);
    libraryRow->addWidget(refreshLibraryButton);
    libraryRow->addWidget(m_libraryCombo);
    outer->addLayout(libraryRow);

    m_migrationNotice = new QLabel(this);
    m_migrationNotice->setObjectName(QStringLiteral("brecoLangMigrationNotice"));
    m_migrationNotice->setWordWrap(true);
    m_migrationNotice->setStyleSheet(QStringLiteral("QLabel { color: #9a6700; }"));
    m_migrationNotice->hide();
    outer->addWidget(m_migrationNotice);

    auto* entryRow = new QHBoxLayout();
    entryRow->addWidget(new QLabel(QStringLiteral("Entry"), this));
    m_entryCombo = new QComboBox(this);
    m_entryCombo->setObjectName(QStringLiteral("brecoLangEntryCombo"));
    entryRow->addWidget(m_entryCombo, 1);
    entryRow->addWidget(new QLabel(QStringLiteral("Offset"), this));
    m_offsetEdit = new QLineEdit(QStringLiteral("0"), this);
    m_offsetEdit->setObjectName(QStringLiteral("brecoLangOffset"));
    m_offsetEdit->setMaximumWidth(150);
    entryRow->addWidget(m_offsetEdit);
    auto* browseInput =
        new QPushButton(QStringLiteral("Bind selected input…"), this);
    auto* decodeButton = new QPushButton(QStringLiteral("Decode"), this);
    decodeButton->setObjectName(QStringLiteral("brecoLangDecodeButton"));
    m_pinViewButton = new QPushButton(QStringLiteral("Pin View"), this);
    m_pinViewButton->setObjectName(QStringLiteral("brecoLangPinView"));
    m_scanButton = new QPushButton(QStringLiteral("Scan for Entry"), this);
    m_scanButton->setObjectName(QStringLiteral("brecoLangScanButton"));
    entryRow->addWidget(browseInput);
    entryRow->addWidget(decodeButton);
    entryRow->addWidget(m_pinViewButton);
    entryRow->addWidget(m_scanButton);
    outer->addLayout(entryRow);

    m_inputTable = new QTableWidget(this);
    m_inputTable->setObjectName(QStringLiteral("brecoLangInputTable"));
    m_inputTable->setColumnCount(2);
    m_inputTable->setHorizontalHeaderLabels(
        {QStringLiteral("Input role"), QStringLiteral("File")});
    m_inputTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    m_inputTable->horizontalHeader()->setSectionResizeMode(1,
                                                           QHeaderView::Stretch);
    m_inputTable->verticalHeader()->setVisible(false);
    m_inputTable->setMaximumHeight(135);
    outer->addWidget(m_inputTable);

    auto* workspace = new QSplitter(Qt::Horizontal, this);
    workspace->setObjectName(QStringLiteral("brecoLangWorkspace"));
    m_schemaEditor = new QPlainTextEdit(workspace);
    m_schemaEditor->setObjectName(QStringLiteral("brecoLangSchemaEditor"));
    m_schemaEditor->setPlaceholderText(
        QStringLiteral("BrecoLang 0.1 schema source"));

    auto* resultPane = new QWidget(workspace);
    auto* resultLayout = new QVBoxLayout(resultPane);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    auto* treeTools = new QHBoxLayout();
    m_outformCombo = new QComboBox(resultPane);
    m_outformCombo->setObjectName(QStringLiteral("brecoLangOutformCombo"));
    auto* saveOutformButton =
        new QPushButton(QStringLiteral("Save Outform…"), resultPane);
    auto* saveJsonButton = new QPushButton(QStringLiteral("Save JSON…"), resultPane);
    auto* saveBinaryButton =
        new QPushButton(QStringLiteral("Save Binary…"), resultPane);
    m_expandAllButton = new QPushButton(QStringLiteral("Expand All"), resultPane);
    m_expandAllButton->setObjectName(QStringLiteral("brecoLangExpandAll"));
    treeTools->addWidget(new QLabel(QStringLiteral("Outform"), resultPane));
    treeTools->addWidget(m_outformCombo, 1);
    treeTools->addWidget(saveOutformButton);
    treeTools->addWidget(saveJsonButton);
    treeTools->addWidget(saveBinaryButton);
    treeTools->addWidget(m_expandAllButton);
    resultLayout->addLayout(treeTools);

    m_viewTabs = new QTabWidget(resultPane);
    m_viewTabs->setObjectName(QStringLiteral("brecoLangViewTabs"));
    m_viewTabs->setTabsClosable(true);
    ViewState live = createView(QStringLiteral("Live"));
    m_views.push_back(live);
    resultLayout->addWidget(m_viewTabs, 1);
    workspace->addWidget(m_schemaEditor);
    workspace->addWidget(resultPane);
    workspace->setStretchFactor(0, 2);
    workspace->setStretchFactor(1, 3);
    outer->addWidget(workspace, 1);

    m_status = new QLabel(QStringLiteral("Load a schema to begin."), this);
    m_status->setObjectName(QStringLiteral("brecoLangStatus"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setWordWrap(true);
    outer->addWidget(m_status);

    m_compileTimer = new QTimer(this);
    m_compileTimer->setSingleShot(true);
    m_compileTimer->setInterval(300);
    connect(m_compileTimer, &QTimer::timeout, this,
            &BrecoLangPanel::compileEditor);
    connect(m_schemaEditor, &QPlainTextEdit::textChanged, this, [this]() {
        if (!m_updatingEditor) {
            m_compileTimer->start();
        }
    });
    connect(browseSchema, &QPushButton::clicked, this,
            &BrecoLangPanel::chooseSchema);
    connect(loadSchema, &QPushButton::clicked, this,
            [this]() { loadSchemaFile(m_schemaPath->text()); });
    connect(chooseLibrary, &QPushButton::clicked, this,
            &BrecoLangPanel::chooseLibraryDirectory);
    connect(refreshLibraryButton, &QPushButton::clicked, this,
            &BrecoLangPanel::refreshLibrary);
    connect(m_libraryCombo, qOverload<int>(&QComboBox::activated), this,
            [this](int index) {
                const QString path = m_libraryCombo->itemData(index).toString();
                if (!path.isEmpty()) {
                    loadSchemaFile(path);
                }
            });
    connect(browseInput, &QPushButton::clicked, this,
            &BrecoLangPanel::chooseInput);
    connect(decodeButton, &QPushButton::clicked, this,
            &BrecoLangPanel::decodeSelected);
    connect(m_offsetEdit, &QLineEdit::returnPressed, this,
            &BrecoLangPanel::decodeSelected);
    connect(m_pinViewButton, &QPushButton::clicked, this,
            &BrecoLangPanel::pinCurrentView);
    connect(m_scanButton, &QPushButton::clicked, this, [this]() {
        if (m_scanRunning) {
            emit scanStopRequested();
        } else {
            emit scanRequested();
        }
    });
    connect(m_expandAllButton, &QPushButton::clicked, this, [this]() {
        if (ViewState* view = activeView(); view != nullptr) {
            view->tree->expandAll();
        }
    });
    connect(saveJsonButton, &QPushButton::clicked, this,
            &BrecoLangPanel::saveJson);
    connect(saveBinaryButton, &QPushButton::clicked, this,
            &BrecoLangPanel::saveBinary);
    connect(saveOutformButton, &QPushButton::clicked, this,
            &BrecoLangPanel::saveOutform);
    connect(m_viewTabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
        if (index <= 0) {
            return;
        }
        QWidget* page = m_viewTabs->widget(index);
        const auto found = std::find_if(
            m_views.begin(), m_views.end(),
            [page](const ViewState& view) { return view.page == page; });
        if (found != m_views.end()) {
            m_views.erase(found);
        }
        m_viewTabs->removeTab(index);
        page->deleteLater();
    });

    setLibraryDirectory(BrecoLangLibrary::defaultDirectory());
}

BrecoLangPanel::ViewState BrecoLangPanel::createView(const QString& title) {
    ViewState view;
    view.page = new QWidget(m_viewTabs);
    auto* layout = new QVBoxLayout(view.page);
    layout->setContentsMargins(0, 0, 0, 0);
    view.model = new DecodedTreeModel(view.page);
    view.tree = new QTreeView(view.page);
    view.tree->setObjectName(QStringLiteral("brecoLangTreeView"));
    view.tree->setModel(view.model);
    view.tree->setAlternatingRowColors(true);
    view.tree->setUniformRowHeights(true);
    view.tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    view.tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    view.tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    view.tree->header()->setStretchLastSection(false);
    layout->addWidget(view.tree);
    m_viewTabs->addTab(view.page, title);
    installViewNavigation(view);
    return view;
}

void BrecoLangPanel::installViewNavigation(ViewState& view) {
    QTreeView* treeView = view.tree;
    connect(treeView, &QTreeView::doubleClicked, this,
            [this, treeView](const QModelIndex& index) {
                const auto found = std::find_if(
                    m_views.cbegin(), m_views.cend(),
                    [treeView](const ViewState& candidate) {
                        return candidate.tree == treeView;
                    });
                if (found == m_views.cend() || !found->program) {
                    return;
                }
                const DecodedNode* node = found->model->nodeForIndex(index);
                if (node == nullptr || !node->hasSourceSpan ||
                    node->input >= static_cast<InputId>(found->sources.size()) ||
                    !found->sources.at(node->input)) {
                    return;
                }
                emit sourceLocationActivated(found->sources.at(node->input)->path(),
                                             node->offset, node->length);
            });
}

bool BrecoLangPanel::loadSchemaFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("Could not open schema '%1': %2")
                      .arg(path, file.errorString()),
                  true);
        return false;
    }
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    m_schemaPath->setText(absolutePath);
    const bool loaded =
        loadSchemaText(QString::fromUtf8(file.readAll()), absolutePath);
    if (loaded) {
        emit schemaFileLoaded(absolutePath);
    }
    return loaded;
}

bool BrecoLangPanel::loadSchemaText(const QString& source,
                                    const QString& sourcePath) {
    m_sourcePath = sourcePath;
    m_updatingEditor = true;
    m_schemaEditor->setPlainText(source);
    m_updatingEditor = false;
    m_compileTimer->stop();
    compileEditor();
    return m_program != nullptr;
}

void BrecoLangPanel::compileEditor() {
    const QHash<QString, QString> paths = currentInputPaths();
    const QString entry = m_entryCombo->currentText();
    const bool shouldRedecode = m_liveDecoded;
    const CompileResult compiled =
        compileBrecoLang(m_schemaEditor->toPlainText(), m_sourcePath);
    if (!compiled.success()) {
        m_program.reset();
        if (ViewState* view = liveView(); view != nullptr) {
            view->program.reset();
            view->sources.clear();
            view->rootValue = kInvalidId;
            view->model->clear();
        }
        populateProgramControls();
        setStatus(compileMessages(compiled.diagnostics), true);
        return;
    }
    m_program = compiled.program;
    populateProgramControls(paths, entry);
    const QString warnings = compileMessages(compiled.diagnostics);
    setStatus(warnings.isEmpty() ? QStringLiteral("Schema compiled successfully.")
                                 : warnings,
              false);
    if (shouldRedecode) {
        decodeSelected();
    }
}

void BrecoLangPanel::populateProgramControls(
    const QHash<QString, QString>& preservedPaths,
    const QString& preservedEntry) {
    const QSignalBlocker entryBlocker(m_entryCombo);
    m_entryCombo->clear();
    m_outformCombo->clear();
    m_inputTable->setRowCount(0);
    if (!m_program) {
        return;
    }
    int defaultEntryIndex = -1;
    for (const EntryDesc& entry : m_program->entries) {
        const QString name = m_program->symbol(entry.name);
        m_entryCombo->addItem(name);
        if (entry.name == m_program->defaultEntry) {
            defaultEntryIndex = m_entryCombo->count() - 1;
        }
    }
    const int preservedIndex = m_entryCombo->findText(preservedEntry);
    m_entryCombo->setCurrentIndex(preservedIndex >= 0 ? preservedIndex
                                                      : defaultEntryIndex);
    for (const OutformDesc& outform : m_program->outforms) {
        m_outformCombo->addItem(m_program->symbol(outform.name));
    }
    m_inputTable->setRowCount(m_program->inputs.size());
    for (InputId input = 0; input < static_cast<InputId>(m_program->inputs.size());
         ++input) {
        const InputDesc& descriptor = m_program->inputs.at(input);
        const QString roleName = m_program->symbol(descriptor.name);
        auto* role = new QTableWidgetItem(roleName);
        role->setFlags(role->flags() & ~Qt::ItemIsEditable);
        role->setData(Qt::UserRole, input);
        m_inputTable->setItem(static_cast<int>(input), 0, role);
        auto* path = new QTableWidgetItem(preservedPaths.value(roleName));
        if (path->text().isEmpty() && descriptor.isDefault &&
            !m_suggestedInputPath.isEmpty()) {
            path->setText(m_suggestedInputPath);
        }
        m_inputTable->setItem(static_cast<int>(input), 1, path);
    }
    if (m_inputTable->rowCount() > 0) {
        m_inputTable->selectRow(0);
    }
}

void BrecoLangPanel::setSuggestedInputPath(const QString& path) {
    m_suggestedInputPath = path;
    if (!m_program) {
        return;
    }
    for (InputId input = 0; input < static_cast<InputId>(m_program->inputs.size());
         ++input) {
        if (m_program->inputs.at(input).isDefault) {
            QTableWidgetItem* item = m_inputTable->item(static_cast<int>(input), 1);
            if (item != nullptr && item->text().isEmpty()) {
                item->setText(path);
            }
        }
    }
}

bool BrecoLangPanel::setInputPath(QStringView role, const QString& path) {
    for (int row = 0; row < m_inputTable->rowCount(); ++row) {
        if (m_inputTable->item(row, 0) != nullptr &&
            m_inputTable->item(row, 0)->text() == role) {
            m_inputTable->item(row, 1)->setText(path);
            return true;
        }
    }
    return false;
}

bool BrecoLangPanel::selectEntry(QStringView entryName) {
    const int index = m_entryCombo->findText(entryName.toString());
    if (index < 0) {
        return false;
    }
    m_entryCombo->setCurrentIndex(index);
    return true;
}

void BrecoLangPanel::setDecodeOffset(quint64 offset) {
    m_offsetEdit->setText(QStringLiteral("0x%1").arg(offset, 0, 16));
}

quint64 BrecoLangPanel::decodeOffset() const {
    bool ok = false;
    const quint64 value = m_offsetEdit->text().trimmed().toULongLong(&ok, 0);
    return ok ? value : std::numeric_limits<quint64>::max();
}

QString BrecoLangPanel::inputPath(InputId input) const {
    if (input >= static_cast<InputId>(m_inputTable->rowCount())) {
        return {};
    }
    const QTableWidgetItem* item =
        m_inputTable->item(static_cast<int>(input), 1);
    return item != nullptr ? item->text().trimmed() : QString();
}

QHash<QString, QString> BrecoLangPanel::currentInputPaths() const {
    QHash<QString, QString> paths;
    for (int row = 0; row < m_inputTable->rowCount(); ++row) {
        const QTableWidgetItem* role = m_inputTable->item(row, 0);
        const QTableWidgetItem* path = m_inputTable->item(row, 1);
        if (role != nullptr && path != nullptr) {
            paths.insert(role->text(), path->text());
        }
    }
    return paths;
}

bool BrecoLangPanel::decodeSelected() {
    if (!m_program || m_entryCombo->currentText().isEmpty()) {
        setStatus(QStringLiteral("Load a schema and select an entry first."), true);
        return false;
    }
    const quint64 offset = decodeOffset();
    if (offset == std::numeric_limits<quint64>::max()) {
        setStatus(QStringLiteral("Offset must be a decimal or hexadecimal byte offset."),
                  true);
        return false;
    }

    DecodeRequest request;
    request.program = m_program;
    request.entryName = m_entryCombo->currentText();
    request.startOffset = offset;
    request.mode = DecodeMode::Tree;
    request.inputs.resize(m_program->inputs.size());
    for (InputId input = 0; input < static_cast<InputId>(m_program->inputs.size());
         ++input) {
        const QString path = inputPath(input);
        if (path.isEmpty()) {
            continue;
        }
        QString error;
        request.inputs[input] = PagedFileSource::open(path, &error);
        if (!request.inputs[input]) {
            setStatus(QStringLiteral("Could not bind input '%1': %2")
                          .arg(m_program->symbol(m_program->inputs.at(input).name),
                               error),
                      true);
            return false;
        }
    }
    const DecodeResult decoded = decodeBrecoProgram(request);
    ViewState* view = liveView();
    if (view == nullptr) {
        return false;
    }
    view->program = m_program;
    view->entryName = request.entryName;
    view->offset = offset;
    view->sources = request.inputs;
    view->rootValue = decoded.rootValue;
    if (!decoded.tree) {
        view->model->clear();
    } else {
        view->model->setDocument(m_program, decoded.tree);
        if (view->model->rowCount() > 0) {
            view->tree->expand(view->model->index(0, 0));
        }
    }
    m_viewTabs->setCurrentWidget(view->page);
    const QString messages = runtimeMessages(decoded.diagnostics);
    setStatus(decoded.success()
                  ? QStringLiteral("Decoded %1 bytes into %2 nodes.%3")
                        .arg(decoded.endOffset - decoded.startOffset)
                        .arg(decoded.constructedNodes)
                        .arg(messages.isEmpty()
                                 ? QString()
                                 : QStringLiteral("\n%1").arg(messages))
                  : (messages.isEmpty() ? QStringLiteral("Decode failed.")
                                        : messages),
              !decoded.success());
    m_liveDecoded = decoded.success();
    return decoded.success();
}

bool BrecoLangPanel::pinCurrentView() {
    const ViewState* source = activeView();
    if (source == nullptr || !source->model->tree() || !source->program) {
        setStatus(QStringLiteral("Decode an entry before pinning a view."), true);
        return false;
    }
    const QString title = QStringLiteral("%1 @ 0x%2")
                              .arg(source->entryName)
                              .arg(source->offset, 0, 16);
    ViewState pinned = createView(title);
    pinned.program = source->program;
    pinned.entryName = source->entryName;
    pinned.offset = source->offset;
    pinned.sources = source->sources;
    pinned.rootValue = source->rootValue;
    pinned.model->setDocument(source->program, source->model->tree());
    m_views.push_back(pinned);
    m_viewTabs->setCurrentWidget(pinned.page);
    if (pinned.model->rowCount() > 0) {
        pinned.tree->expand(pinned.model->index(0, 0));
    }
    setStatus(QStringLiteral("Pinned %1.").arg(title), false);
    return true;
}

BrecoLangPanel::ViewState* BrecoLangPanel::liveView() {
    return m_views.isEmpty() ? nullptr : &m_views[0];
}

const BrecoLangPanel::ViewState* BrecoLangPanel::activeView() const {
    QWidget* current = m_viewTabs->currentWidget();
    const auto found = std::find_if(
        m_views.cbegin(), m_views.cend(),
        [current](const ViewState& view) { return view.page == current; });
    return found == m_views.cend() ? nullptr : &*found;
}

BrecoLangPanel::ViewState* BrecoLangPanel::activeView() {
    QWidget* current = m_viewTabs->currentWidget();
    const auto found = std::find_if(
        m_views.begin(), m_views.end(),
        [current](const ViewState& view) { return view.page == current; });
    return found == m_views.end() ? nullptr : &*found;
}

bool BrecoLangPanel::exportJson(QIODevice* output, QString* error) const {
    const ViewState* view = activeView();
    if (view == nullptr || !view->program || !view->model->tree()) {
        if (error != nullptr) {
            *error = QStringLiteral("Decode an entry before exporting JSON");
        }
        return false;
    }
    DecodeRequest request;
    request.program = view->program;
    request.entryName = view->entryName;
    request.inputs = view->sources;
    request.startOffset = view->offset;
    request.mode = DecodeMode::Streaming;
    request.output = output;
    const DecodeResult decoded = decodeBrecoProgram(request);
    if (!decoded.success()) {
        if (error != nullptr) {
            *error = runtimeMessages(decoded.diagnostics);
        }
        return false;
    }
    if (output->write("\n", 1) != 1) {
        if (error != nullptr) {
            *error = output->errorString();
        }
        return false;
    }
    return true;
}

bool BrecoLangPanel::writeSourceSpans(const ViewState& view,
                                      const StorageLayout& layout,
                                      QIODevice* output, QString* error) const {
    const auto tree = view.model->tree();
    if (!tree || layout.spans.first > static_cast<quint32>(tree->spans.size()) ||
        layout.spans.count > static_cast<quint32>(tree->spans.size()) -
                                 layout.spans.first) {
        if (error != nullptr) {
            *error = QStringLiteral("Decoded storage layout is invalid");
        }
        return false;
    }
    constexpr qsizetype chunkSize = 1024 * 1024;
    for (quint32 index = 0; index < layout.spans.count; ++index) {
        const ByteSpanValue& span = tree->spans.at(layout.spans.first + index);
        if (span.input >= static_cast<InputId>(view.sources.size()) ||
            !view.sources.at(span.input)) {
            if (error != nullptr) {
                *error = QStringLiteral("Storage input is no longer bound");
            }
            return false;
        }
        ByteSource* source = view.sources.at(span.input).get();
        const quint64 base = source->absoluteOffset(0);
        if (span.offset < base) {
            if (error != nullptr) {
                *error = QStringLiteral("Storage span precedes its input window");
            }
            return false;
        }
        quint64 logical = span.offset - base;
        quint64 remaining = span.length;
        while (remaining > 0) {
            const qsizetype amount = static_cast<qsizetype>(
                qMin<quint64>(remaining, static_cast<quint64>(chunkSize)));
            const ByteReadResult bytes = source->read(logical, amount);
            if (!bytes.ok() || bytes.view.data() == nullptr) {
                if (error != nullptr) {
                    *error = bytes.error.isEmpty()
                                 ? QStringLiteral("Could not read decoded source span")
                                 : bytes.error;
                }
                return false;
            }
            if (output->write(bytes.view.data(), bytes.view.length) !=
                bytes.view.length) {
                if (error != nullptr) {
                    *error = output->errorString();
                }
                return false;
            }
            logical += static_cast<quint64>(amount);
            remaining -= static_cast<quint64>(amount);
        }
    }
    return true;
}

bool BrecoLangPanel::exportBinary(QIODevice* output, QString* error) const {
    const ViewState* view = activeView();
    if (view == nullptr || !view->model->tree()) {
        if (error != nullptr) {
            *error = QStringLiteral("Decode an entry before exporting binary data");
        }
        return false;
    }
    QModelIndex index = view->tree->currentIndex();
    const DecodedNode* node = view->model->nodeForIndex(index);
    if (node == nullptr && !view->model->tree()->nodes.isEmpty()) {
        node = &view->model->tree()->nodes.first();
    }
    if (node == nullptr ||
        node->storageLayout >=
            static_cast<quint32>(view->model->tree()->storageLayouts.size())) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected node has no stored byte layout");
        }
        return false;
    }
    return writeSourceSpans(
        *view, view->model->tree()->storageLayouts.at(node->storageLayout),
        output, error);
}

bool BrecoLangPanel::renderOutform(QStringView outformName, QIODevice* output,
                                   QString* error) const {
    const ViewState* view = activeView();
    if (view == nullptr || !view->program || !view->model->tree() ||
        view->rootValue == kInvalidId) {
        if (error != nullptr) {
            *error = QStringLiteral("Decode an entry before rendering an outform");
        }
        return false;
    }
    const RenderStore store(view->program, view->model->tree(), view->sources,
                            view->rootValue);
    const OutformRenderResult rendered =
        breco::lang::renderOutform(store, outformName, output);
    if (!rendered.success && error != nullptr) {
        *error = rendered.error;
    }
    return rendered.success;
}

std::optional<ProbeScanPlan> BrecoLangPanel::probeScanPlan(QString* error) const {
    if (!m_program || m_entryCombo->currentText().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("Load a schema and select an entry first");
        }
        return std::nullopt;
    }
    const EntryDesc* selected = nullptr;
    for (const EntryDesc& entry : m_program->entries) {
        if (m_program->symbol(entry.name) == m_entryCombo->currentText()) {
            selected = &entry;
            break;
        }
    }
    if (selected == nullptr ||
        selected->input >= static_cast<InputId>(m_program->inputs.size())) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected entry has no valid primary input");
        }
        return std::nullopt;
    }
    ProbeScanPlan plan;
    plan.program = m_program;
    plan.entryName = m_entryCombo->currentText();
    plan.primaryInput = selected->input;
    plan.inputPaths.resize(m_program->inputs.size());
    for (InputId input = 0; input < static_cast<InputId>(m_program->inputs.size());
         ++input) {
        plan.inputPaths[input] = inputPath(input);
        if (input != plan.primaryInput && plan.inputPaths[input].isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("Bind input '%1' before scanning")
                             .arg(m_program->symbol(m_program->inputs.at(input).name));
            }
            return std::nullopt;
        }
    }
    return plan;
}

void BrecoLangPanel::setScanRunning(bool running) {
    m_scanRunning = running;
    m_scanButton->setText(running ? QStringLiteral("Stop Scan")
                                  : QStringLiteral("Scan for Entry"));
}

bool BrecoLangPanel::saveWithCommit(
    const QString& caption, const QString& filter,
    const std::function<bool(QIODevice*, QString*)>& writer) {
    const QString path = QFileDialog::getSaveFileName(this, caption, {}, filter);
    if (path.isEmpty()) {
        return false;
    }
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        setStatus(output.errorString(), true);
        return false;
    }
    QString error;
    if (!writer(&output, &error)) {
        output.cancelWriting();
        setStatus(error, true);
        return false;
    }
    if (!output.commit()) {
        setStatus(output.errorString(), true);
        return false;
    }
    setStatus(QStringLiteral("Saved %1").arg(path), false);
    return true;
}

void BrecoLangPanel::saveJson() {
    saveWithCommit(QStringLiteral("Save decoded JSON"),
                   QStringLiteral("JSON files (*.json);;All files (*)"),
                   [this](QIODevice* output, QString* error) {
                       return exportJson(output, error);
                   });
}

void BrecoLangPanel::saveBinary() {
    saveWithCommit(QStringLiteral("Save decoded binary"),
                   QStringLiteral("Binary files (*.bin);;All files (*)"),
                   [this](QIODevice* output, QString* error) {
                       return exportBinary(output, error);
                   });
}

void BrecoLangPanel::saveOutform() {
    if (m_outformCombo->currentText().isEmpty()) {
        setStatus(QStringLiteral("The schema does not declare an outform."), true);
        return;
    }
    const QString name = m_outformCombo->currentText();
    saveWithCommit(QStringLiteral("Save outform %1").arg(name),
                   QStringLiteral("All files (*)"),
                   [this, name](QIODevice* output, QString* error) {
                       return renderOutform(name, output, error);
                   });
}

void BrecoLangPanel::chooseSchema() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open BrecoLang schema"),
        QFileInfo(m_schemaPath->text()).absolutePath(),
        QStringLiteral("BrecoLang schemas (*.breco);;All files (*)"));
    if (!path.isEmpty()) {
        loadSchemaFile(path);
    }
}

void BrecoLangPanel::chooseInput() {
    const int row = m_inputTable->currentRow();
    if (row < 0) {
        setStatus(QStringLiteral("Select an input row first."), true);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Bind BrecoLang input"),
        QFileInfo(m_inputTable->item(row, 1)->text()).absolutePath(),
        QStringLiteral("All files (*)"));
    if (!path.isEmpty()) {
        m_inputTable->item(row, 1)->setText(path);
    }
}

void BrecoLangPanel::chooseLibraryDirectory() {
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose schema library"), m_libraryPath->text());
    if (!path.isEmpty()) {
        setLibraryDirectory(path);
    }
}

void BrecoLangPanel::setLibraryDirectory(const QString& path) {
    BrecoLangLibrary library(path);
    QString error;
    if (!BrecoLangLibrary::ensureDirectory(library.directory(), &error)) {
        setStatus(error, true);
        return;
    }
    m_libraryPath->setText(library.directory());
    refreshLibrary();
    emit libraryDirectoryChanged(library.directory());
}

QString BrecoLangPanel::libraryDirectory() const {
    return m_libraryPath->text();
}

void BrecoLangPanel::refreshLibrary() {
    BrecoLangLibrary library(m_libraryPath->text());
    const BrecoLangLibraryContents contents = library.scan();
    const QSignalBlocker blocker(m_libraryCombo);
    m_libraryCombo->clear();
    m_libraryCombo->addItem(QStringLiteral("Choose schema…"));
    for (const BrecoLangLibraryFile& item : contents.schemas) {
        const QString label = item.errorMessage.isEmpty()
                                  ? item.relativePath
                                  : QStringLiteral("%1 — %2")
                                        .arg(item.relativePath, item.errorMessage);
        m_libraryCombo->addItem(label, item.filePath);
    }
    if (contents.filesNeedingMigration.isEmpty()) {
        m_migrationNotice->clear();
        m_migrationNotice->hide();
    } else {
        m_migrationNotice->setText(
            QStringLiteral("%1 older schema file(s) remain on disk and require "
                           "manual migration. They were not loaded, converted, or "
                           "deleted: %2")
                .arg(contents.filesNeedingMigration.size())
                .arg(contents.filesNeedingMigration.join(QStringLiteral(", "))));
        m_migrationNotice->show();
    }
}

QString BrecoLangPanel::migrationNoticeText() const {
    return m_migrationNotice->text();
}

void BrecoLangPanel::setStatus(const QString& text, bool error) {
    m_status->setText(text.isEmpty() ? QStringLiteral("Ready.") : text);
    m_status->setStyleSheet(error ? QStringLiteral("QLabel { color: #b00020; }")
                                  : QString());
}

QString BrecoLangPanel::statusText() const { return m_status->text(); }

QTreeView* BrecoLangPanel::treeView() const {
    const ViewState* view = activeView();
    return view != nullptr ? view->tree : nullptr;
}

DecodedTreeModel* BrecoLangPanel::treeModel() const {
    const ViewState* view = activeView();
    return view != nullptr ? view->model : nullptr;
}

std::shared_ptr<const DecodedTree> BrecoLangPanel::tree() const {
    const ViewState* view = activeView();
    return view != nullptr ? view->model->tree()
                           : std::shared_ptr<const DecodedTree>{};
}

}  // namespace breco::lang

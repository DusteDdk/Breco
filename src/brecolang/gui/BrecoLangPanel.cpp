#include "brecolang/gui/BrecoLangPanel.h"

#include "brecolang/compiler/Compiler.h"
#include "brecolang/gui/BrecoDecodeController.h"
#include "brecolang/gui/BrecoLangLibrary.h"
#include "brecolang/gui/DecodedTreeModel.h"
#include "edit/EditQueue.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QComboBox>
#include <QInputDialog>
#include <QColor>
#include <QDockWidget>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QVector>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace breco::lang {

namespace {

constexpr int kDecodeTargetKindRole = Qt::UserRole;
constexpr int kDecodeTargetNameRole = Qt::UserRole + 1;

bool alphabeticallyBefore(const QString& left, const QString& right) {
    const int insensitive = QString::compare(left, right, Qt::CaseInsensitive);
    return insensitive != 0 ? insensitive < 0 : left < right;
}

class InputPathEdit final : public QLineEdit {
public:
    explicit InputPathEdit(std::function<void()> openFile, QWidget* parent)
        : QLineEdit(parent), m_openFile(std::move(openFile)) {}

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        QLineEdit::mouseDoubleClickEvent(event);
        if (event->button() == Qt::LeftButton && m_openFile) {
            m_openFile();
        }
    }

private:
    std::function<void()> m_openFile;
};

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
    m_decodeController = new BrecoDecodeController(this);
    connect(m_decodeController, &BrecoDecodeController::resolveFinished, this,
            &BrecoLangPanel::handleResolveFinished);
    connect(m_decodeController, &BrecoDecodeController::displayPageFinished,
            this, &BrecoLangPanel::handleDisplayPageFinished);
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
    m_entryCombo->setModel(new QStandardItemModel(m_entryCombo));
    entryRow->addWidget(m_entryCombo, 1);
    entryRow->addWidget(new QLabel(QStringLiteral("Offset"), this));
    m_offsetEdit = new QLineEdit(QStringLiteral("0"), this);
    m_offsetEdit->setObjectName(QStringLiteral("brecoLangOffset"));
    m_offsetEdit->setMaximumWidth(150);
    entryRow->addWidget(m_offsetEdit);
    auto* decodeButton = new QPushButton(QStringLiteral("Decode"), this);
    decodeButton->setObjectName(QStringLiteral("brecoLangDecodeButton"));
    m_pinViewButton = new QPushButton(QStringLiteral("Pin View"), this);
    m_pinViewButton->setObjectName(QStringLiteral("brecoLangPinView"));
    m_scanButton = new QPushButton(QStringLiteral("Scan for Entry"), this);
    m_scanButton->setObjectName(QStringLiteral("brecoLangScanButton"));
    entryRow->addWidget(decodeButton);
    entryRow->addWidget(m_pinViewButton);
    entryRow->addWidget(m_scanButton);
    outer->addLayout(entryRow);

    m_inputViewGroup = new QButtonGroup(this);
    m_inputViewGroup->setExclusive(true);
    m_inputTable = new QTableWidget(this);
    m_inputTable->setObjectName(QStringLiteral("brecoLangInputTable"));
    m_inputTable->setColumnCount(3);
    m_inputTable->setHorizontalHeaderLabels(
        {QStringLiteral("View"), QStringLiteral("Input role"),
         QStringLiteral("File")});
    m_inputTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    m_inputTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    m_inputTable->horizontalHeader()->setSectionResizeMode(2,
                                                           QHeaderView::Stretch);
    m_inputTable->verticalHeader()->setVisible(false);
    m_inputTable->setMaximumHeight(135);
    m_inputTable->hide();
    outer->addWidget(m_inputTable);

    m_workspaceWindow = new QMainWindow(this);
    m_workspaceWindow->setObjectName(QStringLiteral("brecoLangWorkspace"));
    m_workspaceWindow->setWindowFlag(Qt::Window, false);
    m_workspaceWindow->setDockNestingEnabled(true);
    auto* centralPlaceholder = new QWidget(m_workspaceWindow);
    centralPlaceholder->setObjectName(
        QStringLiteral("brecoLangWorkspaceCentral"));
    centralPlaceholder->setMaximumSize(0, 0);
    m_workspaceWindow->setCentralWidget(centralPlaceholder);

    m_schemaDock =
        new QDockWidget(QStringLiteral("Schema"), m_workspaceWindow);
    m_schemaDock->setObjectName(QStringLiteral("brecoLangSchemaDock"));
    m_schemaDock->setFeatures(QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);
    m_schemaDock->setMinimumSize(120, 80);
    m_schemaEditor = new QPlainTextEdit(m_schemaDock);
    m_schemaEditor->setObjectName(QStringLiteral("brecoLangSchemaEditor"));
    m_schemaEditor->setPlaceholderText(
        QStringLiteral("BrecoLang 0.1 schema source"));
    m_schemaDock->setWidget(m_schemaEditor);

    m_decodeDock =
        new QDockWidget(QStringLiteral("Decode"), m_workspaceWindow);
    m_decodeDock->setObjectName(QStringLiteral("brecoLangDecodeDock"));
    m_decodeDock->setFeatures(QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);
    m_decodeDock->setMinimumSize(180, 80);
    auto* resultPane = new QWidget(m_decodeDock);
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
    m_expandAllButton =
        new QPushButton(QStringLiteral("Expand Loaded"), resultPane);
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
    m_decodeDock->setWidget(resultPane);

    m_workspaceWindow->addDockWidget(Qt::LeftDockWidgetArea, m_schemaDock);
    m_workspaceWindow->addDockWidget(Qt::LeftDockWidgetArea, m_decodeDock);
    m_workspaceWindow->splitDockWidget(m_schemaDock, m_decodeDock,
                                       Qt::Horizontal);
    m_workspaceWindow->installEventFilter(this);
    m_schemaDock->installEventFilter(this);
    m_decodeDock->installEventFilter(this);
    outer->addWidget(m_workspaceWindow, 1);

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
    m_hoverHighlightTimer = new QTimer(this);
    m_hoverHighlightTimer->setSingleShot(true);
    m_hoverHighlightTimer->setInterval(50);
    connect(m_hoverHighlightTimer, &QTimer::timeout, this, [this]() {
        if (!m_hasPendingHover) {
            return;
        }
        m_hasHoverHighlight = true;
        m_hoverHighlightPath = m_pendingHoverPath;
        m_hoverHighlightOffset = m_pendingHoverOffset;
        m_hoverHighlightLength = m_pendingHoverLength;
        ViewState* view = activeView();
        if (view != nullptr) {
            applySourceHighlights(*view);
        }
    });
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
    connect(m_inputViewGroup, &QButtonGroup::idToggled, this,
            [this](int input, bool checked) {
                if (!checked) {
                    return;
                }
                const QString path = inputPath(static_cast<InputId>(input));
                if (!path.isEmpty()) {
                    emit inputFileActivated(path);
                }
            });
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
            expandDefaultContainers(*view);
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
            m_decodeController->closeView(found->id);
            m_views.erase(found);
        }
        m_viewTabs->removeTab(index);
        page->deleteLater();
    });

    setLibraryDirectory(BrecoLangLibrary::defaultDirectory());
}

bool BrecoLangPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_workspaceWindow &&
        (event->type() == QEvent::Show || event->type() == QEvent::Resize)) {
        ensureWorkspaceLayout();
    }
    auto* dock = qobject_cast<QDockWidget*>(watched);
    if (dock != nullptr &&
        (dock == m_schemaDock || dock == m_decodeDock) &&
        event->type() == QEvent::Close) {
        event->ignore();
        if (dock->isFloating()) {
            dock->setFloating(false);
        }
        dock->show();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void BrecoLangPanel::ensureWorkspaceLayout() {
    if (m_workspaceLayoutApplied || m_workspaceWindow == nullptr ||
        m_schemaDock == nullptr || m_decodeDock == nullptr) {
        return;
    }
    const int width = m_workspaceWindow->width();
    if (width < 50) {
        return;
    }
    m_workspaceLayoutApplied = true;
    m_workspaceWindow->resizeDocks({m_schemaDock, m_decodeDock},
                                   {width * 2 / 5, width * 3 / 5},
                                   Qt::Horizontal);
}

BrecoLangPanel::ViewState BrecoLangPanel::createView(const QString& title) {
    ViewState view;
    view.id = m_nextViewId++;
    view.page = new QWidget(m_viewTabs);
    auto* layout = new QVBoxLayout(view.page);
    layout->setContentsMargins(0, 0, 0, 0);
    view.model = new DecodedTreeModel(view.page);
    view.tree = new QTreeView(view.page);
    view.tree->setObjectName(QStringLiteral("brecoLangTreeView"));
    view.tree->setModel(view.model);
    view.tree->setAlternatingRowColors(true);
    view.tree->setUniformRowHeights(true);
    view.tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view.tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    view.tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    view.tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    view.tree->header()->setStretchLastSection(false);
    layout->addWidget(view.tree);
    m_viewTabs->addTab(view.page, title);
    installViewNavigation(view);
    connect(view.model, &DecodedTreeModel::pageRequested, this,
            [this, viewId = view.id](const SequenceWindow& window) {
                ViewState* target = viewById(viewId);
                if (target == nullptr || !target->document.isValid() ||
                    !target->shape) {
                    return;
                }
                DisplayPageRequest request;
                request.document = target->document;
                request.root = window.sequence.isReferenceTarget() &&
                                       window.sequence.referenceTarget.has_value()
                                   ? MaterializationLocator::target(
                                         *window.sequence.referenceTarget)
                                   : target->shape->root;
                request.expansionPath = window.expansionPath;
                request.sequenceWindows = {window};
                request.defaultSequenceItems =
                    DecodedTreeModel::kAutoExpandedListItems;
                m_decodeController->requestDisplayPage(viewId,
                                                       std::move(request));
            });
    connect(view.model, &DecodedTreeModel::referenceRequested, this,
            [this, viewId = view.id](const ReferencePageRequest& expansion) {
                ViewState* target = viewById(viewId);
                if (target == nullptr || !target->document.isValid()) {
                    return;
                }
                DisplayPageRequest request;
                request.document = target->document;
                request.root = expansion.handle.targetLocator();
                request.expansionPath = expansion.expansionPath;
                request.defaultSequenceItems =
                    DecodedTreeModel::kAutoExpandedListItems;
                m_decodeController->requestDisplayPage(viewId,
                                                       std::move(request));
            });
    return view;
}

void BrecoLangPanel::installViewNavigation(ViewState& view) {
    QTreeView* treeView = view.tree;
    connect(treeView, &QTreeView::expanded, this,
            [model = view.model](const QModelIndex& index) {
                if (model->canFetchMore(index)) {
                    model->fetchMore(index);
                }
            });
    connect(treeView, &QTreeView::clicked, this,
            [this, treeView](const QModelIndex& index) {
                activateTreeIndex(treeView, index);
            });
    connect(treeView, &QTreeView::doubleClicked, this,
            [this, treeView, model = view.model](const QModelIndex& index) {
                if (model->isContinuationRow(index)) {
                    model->requestMore(index);
                    return;
                }
                if (model->isReferenceRow(index)) {
                    treeView->setExpanded(index, true);
                    if (model->canFetchMore(index)) {
                        model->fetchMore(index);
                    }
                    return;
                }
                auto found = std::find_if(
                    m_views.begin(), m_views.end(),
                    [treeView](const ViewState& candidate) {
                        return candidate.tree == treeView;
                    });
                if (found != m_views.end()) {
                    editDecodedValue(*found, index);
                }
            });
}

void BrecoLangPanel::activateTreeIndex(QTreeView* treeView,
                                       const QModelIndex& index) {
    auto found = std::find_if(
        m_views.begin(), m_views.end(),
        [treeView](const ViewState& candidate) {
            return candidate.tree == treeView;
        });
    if (found == m_views.end() || !found->program ||
        found->model->isContinuationRow(index)) {
        return;
    }
    const DecodedNode* selectedNode = found->model->nodeForIndex(index);
    if (selectedNode == nullptr || !selectedNode->hasSourceSpan ||
        selectedNode->input >=
            static_cast<InputId>(found->inputPaths.size()) ||
        found->inputPaths.at(selectedNode->input).isEmpty()) {
        return;
    }

    const DecodedNode node = *selectedNode;
    const QString inputPath = found->inputPaths.at(node.input);
    const MaterializationLocator locator =
        found->model->locatorForIndex(index);
    const QVector<MaterializationLocator> expansionPath =
        found->model->expansionPathForIndex(index);

    if (found == m_views.begin() &&
        pinView(*found, locator, expansionPath) == nullptr) {
        return;
    }

    setViewedInput(node.input);
    emit sourceLocationActivated(inputPath, node.offset, node.length);
}

void BrecoLangPanel::expandDefaultContainers(ViewState& view) {
    if (m_expandingDefaults || view.tree == nullptr || view.model == nullptr) {
        return;
    }
    m_expandingDefaults = true;
    requestUnshownSequencePages(view);
    QVector<QModelIndex> pending;
    const int roots = view.model->rowCount();
    pending.reserve(roots);
    for (int row = 0; row < roots; ++row) {
        pending.push_back(view.model->index(row, 0));
    }
    int visited = 0;
    while (!pending.isEmpty() && visited < 100000) {
        ++visited;
        const QModelIndex index = pending.takeLast();
        if (!index.isValid() || view.model->isContinuationRow(index)) {
            continue;
        }
        if (view.model->canFetchMore(index)) {
            view.tree->expand(index);
            view.model->fetchMore(index);
            continue;
        }
        const int rows = view.model->rowCount(index);
        if (rows <= 0) {
            continue;
        }
        view.tree->expand(index);
        for (int row = rows - 1; row >= 0; --row) {
            pending.push_back(view.model->index(row, 0, index));
        }
    }
    m_expandingDefaults = false;
}

void BrecoLangPanel::requestUnshownSequencePages(ViewState& view) {
    if (view.model == nullptr || !view.document.isValid()) {
        return;
    }
    const QVector<SequenceWindow> windows =
        view.model->takeUnshownSequenceWindows(
            DecodedTreeModel::kAutoExpandedListItems);
    for (const SequenceWindow& window : windows) {
        DisplayPageRequest request;
        request.document = view.document;
        request.root = window.sequence.isReferenceTarget() &&
                               window.sequence.referenceTarget.has_value()
                           ? MaterializationLocator::target(
                                 *window.sequence.referenceTarget)
                           : (view.shape ? view.shape->root
                                         : MaterializationLocator{});
        request.expansionPath = window.expansionPath;
        request.sequenceWindows = {window};
        request.defaultSequenceItems =
            DecodedTreeModel::kAutoExpandedListItems;
        m_decodeController->requestDisplayPage(view.id, std::move(request));
    }
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
    m_preservedDecodeSelection.reset();
    m_updatingEditor = true;
    m_schemaEditor->setPlainText(source);
    m_updatingEditor = false;
    m_compileTimer->stop();
    compileSource(true);
    return m_program != nullptr;
}

void BrecoLangPanel::compileEditor() {
    compileSource(false);
}

void BrecoLangPanel::compileSource(bool selectDefault) {
    const QHash<QString, QString> currentPaths = currentInputPaths();
    if (!currentPaths.isEmpty()) {
        m_preservedInputPaths = currentPaths;
    }
    if (!selectDefault) {
        if (const auto selection = currentDecodeSelection();
            selection.has_value()) {
            m_preservedDecodeSelection = selection;
        }
    }
    const bool shouldRedecode = m_liveDecoded;
    const CompileResult compiled =
        compileBrecoLang(m_schemaEditor->toPlainText(), m_sourcePath);
    if (!compiled.success()) {
        m_program.reset();
        if (ViewState* view = liveView(); view != nullptr) {
            m_decodeController->closeView(view->id);
            view->program.reset();
            view->sources.clear();
            view->inputPaths.clear();
            view->document = {};
            view->shape.reset();
            view->rootValue = kInvalidId;
            view->model->clear();
        }
        populateProgramControls();
        setStatus(compileMessages(compiled.diagnostics), true);
        return;
    }
    m_program = compiled.program;
    populateProgramControls(
        m_preservedInputPaths,
        selectDefault ? std::nullopt : m_preservedDecodeSelection,
        selectDefault);
    m_preservedDecodeSelection = currentDecodeSelection();
    m_preservedInputPaths = currentInputPaths();
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
    const std::optional<DecodeSelection>& preservedSelection,
    bool selectDefault) {
    const QSignalBlocker entryBlocker(m_entryCombo);
    m_entryCombo->clear();
    m_outformCombo->clear();
    for (QAbstractButton* button : m_inputViewGroup->buttons()) {
        m_inputViewGroup->removeButton(button);
    }
    m_inputTable->setRowCount(0);
    if (!m_program) {
        m_inputTable->hide();
        return;
    }
    auto* targetModel =
        qobject_cast<QStandardItemModel*>(m_entryCombo->model());
    Q_ASSERT(targetModel != nullptr);
    const auto appendHeader = [targetModel](const QString& text) {
        auto* item = new QStandardItem(text);
        item->setFlags(Qt::NoItemFlags);
        targetModel->appendRow(item);
    };
    const auto appendTarget =
        [targetModel](const QString& name, DecodeTargetKind kind) {
            auto* item = new QStandardItem(name);
            item->setData(static_cast<int>(kind), kDecodeTargetKindRole);
            item->setData(name, kDecodeTargetNameRole);
            targetModel->appendRow(item);
            return item->row();
        };

    QVector<const EntryDesc*> entries;
    entries.reserve(m_program->entries.size());
    for (const EntryDesc& entry : m_program->entries) {
        entries.push_back(&entry);
    }
    std::sort(entries.begin(), entries.end(),
              [this](const EntryDesc* left, const EntryDesc* right) {
                  return alphabeticallyBefore(m_program->symbol(left->name),
                                              m_program->symbol(right->name));
              });

    QVector<const RecordDesc*> records;
    records.reserve(m_program->records.size());
    for (const RecordDesc& record : m_program->records) {
        if (record.parameters.count != 0) {
            continue;
        }
        const bool duplicatesImplicitEntry =
            std::any_of(entries.cbegin(), entries.cend(),
                        [&record](const EntryDesc* entry) {
                            return entry->name == record.name &&
                                   entry->resultType == record.type;
                        });
        if (!duplicatesImplicitEntry) {
            records.push_back(&record);
        }
    }
    std::sort(records.begin(), records.end(),
              [this](const RecordDesc* left, const RecordDesc* right) {
                  const QString leftName = m_program->symbol(left->name);
                  const QString rightName = m_program->symbol(right->name);
                  const bool leftInternal = leftName.startsWith(QLatin1Char('$'));
                  const bool rightInternal =
                      rightName.startsWith(QLatin1Char('$'));
                  return leftInternal != rightInternal
                             ? !leftInternal
                             : alphabeticallyBefore(leftName, rightName);
              });

    int defaultEntryIndex = -1;
    if (!entries.isEmpty()) {
        appendHeader(QStringLiteral("-- Entries --"));
        for (const EntryDesc* entry : entries) {
            const QString name = m_program->symbol(entry->name);
            const int index = appendTarget(name, DecodeTargetKind::Entry);
            if (entry->name == m_program->defaultEntry) {
                defaultEntryIndex = index;
            }
        }
    }
    if (!records.isEmpty()) {
        appendHeader(QStringLiteral("-- Records --"));
        for (const RecordDesc* record : records) {
            appendTarget(m_program->symbol(record->name),
                         DecodeTargetKind::Record);
        }
    }
    const int preservedIndex =
        preservedSelection.has_value()
            ? findDecodeSelection(*preservedSelection)
            : -1;
    m_entryCombo->setCurrentIndex(
        preservedIndex >= 0
            ? preservedIndex
            : (selectDefault ? defaultEntryIndex : -1));
    for (const OutformDesc& outform : m_program->outforms) {
        m_outformCombo->addItem(m_program->symbol(outform.name));
    }
    const bool showBindings = m_program->inputs.size() >= 2;
    m_inputTable->setVisible(showBindings);
    m_inputTable->setRowCount(m_program->inputs.size());
    for (InputId input = 0; input < static_cast<InputId>(m_program->inputs.size());
         ++input) {
        const InputDesc& descriptor = m_program->inputs.at(input);
        const QString roleName = m_program->symbol(descriptor.name);

        auto* radioHost = new QWidget(m_inputTable);
        auto* radioLayout = new QHBoxLayout(radioHost);
        radioLayout->setContentsMargins(0, 0, 0, 0);
        auto* radio = new QRadioButton(radioHost);
        radio->setObjectName(
            QStringLiteral("brecoLangViewInput%1").arg(input));
        radioLayout->addWidget(radio, 0, Qt::AlignCenter);
        m_inputViewGroup->addButton(radio, static_cast<int>(input));
        m_inputTable->setCellWidget(static_cast<int>(input), 0, radioHost);

        auto* role = new QTableWidgetItem(roleName);
        role->setFlags(role->flags() & ~Qt::ItemIsEditable);
        role->setData(Qt::UserRole, input);
        m_inputTable->setItem(static_cast<int>(input), 1, role);

        QString path = preservedPaths.value(roleName);
        if (path.isEmpty() && descriptor.isDefault &&
            !m_suggestedInputPath.isEmpty()) {
            path = m_suggestedInputPath;
        }
        auto* pathEdit = new InputPathEdit(
            [this, input]() { chooseInput(static_cast<int>(input)); },
            m_inputTable);
        pathEdit->setObjectName(
            QStringLiteral("brecoLangInputPath%1").arg(input));
        pathEdit->setText(path);
        connect(pathEdit, &QLineEdit::editingFinished, this,
                [this, input, radio, pathEdit]() {
                    if (radio->isChecked() &&
                        !pathEdit->text().trimmed().isEmpty()) {
                        emit inputFileActivated(pathEdit->text().trimmed());
                    }
                });
        m_inputTable->setCellWidget(static_cast<int>(input), 2, pathEdit);
    }

    if (showBindings && !m_suggestedInputPath.isEmpty()) {
        for (InputId input = 0;
             input < static_cast<InputId>(m_program->inputs.size()); ++input) {
            if (QFileInfo(inputPath(input)).absoluteFilePath() ==
                QFileInfo(m_suggestedInputPath).absoluteFilePath()) {
                setViewedInput(input);
                break;
            }
        }
    }
}

void BrecoLangPanel::setSuggestedInputPath(const QString& path) {
    m_suggestedInputPath = path;
    if (!m_program) {
        return;
    }
    if (m_program->inputs.size() == 1) {
        if (QLineEdit* edit = inputPathEdit(0); edit != nullptr) {
            edit->setText(path);
        }
        return;
    }
    for (InputId input = 0; input < static_cast<InputId>(m_program->inputs.size());
         ++input) {
        QLineEdit* edit = inputPathEdit(static_cast<int>(input));
        if (m_program->inputs.at(input).isDefault && edit != nullptr &&
            edit->text().isEmpty()) {
            edit->setText(path);
        }
        if (edit != nullptr && !path.isEmpty() &&
            QFileInfo(edit->text()).absoluteFilePath() ==
                QFileInfo(path).absoluteFilePath()) {
            setViewedInput(input);
            break;
        }
    }
}

QLineEdit* BrecoLangPanel::inputPathEdit(int row) const {
    return qobject_cast<QLineEdit*>(m_inputTable->cellWidget(row, 2));
}

void BrecoLangPanel::setViewedInput(InputId input) {
    if (!m_program || m_program->inputs.size() < 2 ||
        input >= static_cast<InputId>(m_program->inputs.size())) {
        return;
    }
    if (QAbstractButton* button =
            m_inputViewGroup->button(static_cast<int>(input));
        button != nullptr) {
        const QSignalBlocker blocker(m_inputViewGroup);
        button->setChecked(true);
    }
}

bool BrecoLangPanel::setInputPath(QStringView role, const QString& path) {
    if (!m_program) {
        return false;
    }
    for (InputId input = 0; input < static_cast<InputId>(m_program->inputs.size());
         ++input) {
        if (m_program->symbol(m_program->inputs.at(input).name) == role) {
            if (m_program->inputs.size() == 1) {
                setSuggestedInputPath(path);
            } else if (QLineEdit* edit =
                           inputPathEdit(static_cast<int>(input));
                       edit != nullptr) {
                edit->setText(path);
            }
            return true;
        }
    }
    return false;
}

bool BrecoLangPanel::selectEntry(QStringView entryName) {
    const int index =
        findDecodeSelection({DecodeTargetKind::Entry, entryName.toString()});
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
    if (!m_program ||
        input >= static_cast<InputId>(m_program->inputs.size())) {
        return {};
    }
    if (m_program->inputs.size() == 1) {
        return m_suggestedInputPath.trimmed();
    }
    const QLineEdit* edit = inputPathEdit(static_cast<int>(input));
    return edit != nullptr ? edit->text().trimmed() : QString();
}

QHash<QString, QString> BrecoLangPanel::currentInputPaths() const {
    QHash<QString, QString> paths;
    if (!m_program) {
        return paths;
    }
    for (InputId input = 0; input < static_cast<InputId>(m_program->inputs.size());
         ++input) {
        paths.insert(m_program->symbol(m_program->inputs.at(input).name),
                     inputPath(input));
    }
    return paths;
}

std::optional<BrecoLangPanel::DecodeSelection>
BrecoLangPanel::currentDecodeSelection() const {
    const int index = m_entryCombo->currentIndex();
    if (index < 0) {
        return std::nullopt;
    }
    const QVariant kindValue =
        m_entryCombo->itemData(index, kDecodeTargetKindRole);
    const QString name =
        m_entryCombo->itemData(index, kDecodeTargetNameRole).toString();
    if (!kindValue.isValid() || name.isEmpty()) {
        return std::nullopt;
    }
    const int kind = kindValue.toInt();
    if (kind != static_cast<int>(DecodeTargetKind::Entry) &&
        kind != static_cast<int>(DecodeTargetKind::Record)) {
        return std::nullopt;
    }
    return DecodeSelection{static_cast<DecodeTargetKind>(kind), name};
}

int BrecoLangPanel::findDecodeSelection(
    const DecodeSelection& selection) const {
    if (!selection.isValid()) {
        return -1;
    }
    for (int index = 0; index < m_entryCombo->count(); ++index) {
        if (m_entryCombo->itemData(index, kDecodeTargetKindRole).toInt() ==
                static_cast<int>(selection.kind) &&
            m_entryCombo->itemData(index, kDecodeTargetNameRole).toString() ==
                selection.name) {
            return index;
        }
    }
    return -1;
}

ResolvedDecodeTarget BrecoLangPanel::selectedDecodeTarget(
    QString* error) const {
    const auto selection = currentDecodeSelection();
    if (!selection.has_value()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "Load a schema and select an entry or record first");
        }
        return {};
    }
    return resolveDecodeTarget(m_program, selection->kind, selection->name,
                               error);
}

bool BrecoLangPanel::decodeSelected() {
    QString targetError;
    const ResolvedDecodeTarget target = selectedDecodeTarget(&targetError);
    if (!target.isValid()) {
        setStatus(targetError, true);
        return false;
    }
    const quint64 offset = decodeOffset();
    if (offset == std::numeric_limits<quint64>::max()) {
        setStatus(QStringLiteral("Offset must be a decimal or hexadecimal byte offset."),
                  true);
        return false;
    }

    ViewState* view = liveView();
    if (view == nullptr) {
        return false;
    }
    QVector<InputBindingSpec> bindings(target.program->inputs.size());
    QVector<QString> paths(target.program->inputs.size());
    for (InputId input = 0;
         input < static_cast<InputId>(target.program->inputs.size());
         ++input) {
        const QString path = inputPath(input);
        bindings[input].path = path;
        paths[input] = path;
    }
    view->program = target.program;
    view->entryName = target.entryName;
    view->offset = offset;
    view->sources.clear();
    view->inputPaths = std::move(paths);
    view->document = {};
    view->shape.reset();
    view->rootValue = kInvalidId;
    view->model->clear();
    m_viewTabs->setCurrentWidget(view->page);
    m_decodeController->requestResolve(
        view->id, target.program, target.entryName, std::move(bindings), offset);
    setStatus(QStringLiteral("Resolving decode shape…"), false);
    m_liveDecoded = true;
    return true;
}

BrecoLangPanel::ViewState* BrecoLangPanel::viewById(quint64 id) {
    const auto found = std::find_if(
        m_views.begin(), m_views.end(),
        [id](const ViewState& view) { return view.id == id; });
    return found == m_views.end() ? nullptr : &*found;
}

const BrecoLangPanel::ViewState* BrecoLangPanel::viewById(quint64 id) const {
    const auto found = std::find_if(
        m_views.cbegin(), m_views.cend(),
        [id](const ViewState& view) { return view.id == id; });
    return found == m_views.cend() ? nullptr : &*found;
}

void BrecoLangPanel::handleResolveFinished(const ResolveResponse& response) {
    ViewState* view = viewById(response.tag.viewId);
    if (view == nullptr) {
        return;
    }
    const DecodeResult& decoded = response.result;
    const QString messages = runtimeMessages(decoded.diagnostics);
    if (!decoded.success() || !decoded.shape) {
        view->document = {};
        view->shape.reset();
        view->model->clear();
        m_liveDecoded = false;
        setStatus(messages.isEmpty() ? QStringLiteral("Decode failed.")
                                     : messages,
                  true);
        return;
    }
    view->document = decoded.document;
    view->shape = decoded.shape;
    view->model->setDocument(view->program, decoded.document, decoded.shape);
    if (decoded.shape->outline && !decoded.shape->outline->nodes.isEmpty()) {
        view->rootValue = decoded.shape->outline->nodes.first().value;
    }
    expandDefaultContainers(*view);
    const quint64 bytes = decoded.endOffset - decoded.startOffset;
    setStatus(
        QStringLiteral("Resolved %1 bytes and %2 logical nodes; materialized %3 nodes.%4")
            .arg(bytes)
            .arg(decoded.logicalNodes)
            .arg(decoded.constructedNodes)
            .arg(messages.isEmpty() ? QString()
                                    : QStringLiteral("\n%1").arg(messages)),
        false);
    m_liveDecoded = true;
}

void BrecoLangPanel::handleDisplayPageFinished(
    const DisplayPageResponse& response) {
    ViewState* view = viewById(response.tag.viewId);
    if (view == nullptr || !view->shape) {
        return;
    }
    if (response.result.status != DecodeStatus::Success &&
        response.result.status != DecodeStatus::Paused) {
        const QString messages = runtimeMessages(response.result.diagnostics);
        if (!response.expansionPath.isEmpty()) {
            ReferencePageRequest expansion;
            expansion.expansionPath = response.expansionPath;
            view->model->failReference(
                expansion,
                messages.isEmpty() ? QStringLiteral("request failed")
                                   : messages);
        }
        for (const SequenceWindow& window : response.windows) {
            view->model->failPage(
                window, messages.isEmpty() ? QStringLiteral("request failed")
                                            : messages);
        }
        setStatus(messages.isEmpty() ? QStringLiteral("Materialization failed.")
                                     : messages,
                  true);
        return;
    }
    const QPersistentModelIndex topVisible(
        view->tree->indexAt(QPoint(0, 0)));
    const int topOffset = topVisible.isValid()
                              ? view->tree->visualRect(topVisible).top()
                              : 0;
    const int horizontalScroll =
        view->tree->horizontalScrollBar()->value();
    view->model->applyPage(response.result);
    expandDefaultContainers(*view);
    if (topVisible.isValid()) {
        view->tree->scrollTo(topVisible,
                             QAbstractItemView::PositionAtTop);
        view->tree->verticalScrollBar()->setValue(
            view->tree->verticalScrollBar()->value() - topOffset);
    }
    view->tree->horizontalScrollBar()->setValue(horizontalScroll);
    setStatus(
        response.result.status == DecodeStatus::Paused
            ? QStringLiteral("Replay paused at a committed item boundary; activate the continuation row to resume.")
            : QStringLiteral("Resolved %1 bytes; materialized %2 nodes for display.")
                  .arg(view->shape->endOffset - view->shape->startOffset)
                  .arg(response.result.metrics.materializedNodes),
        false);
}

bool BrecoLangPanel::pinCurrentView() {
    const ViewState* source = activeView();
    if (source == nullptr || !source->shape || !source->program ||
        !source->document.isValid()) {
        setStatus(QStringLiteral("Decode an entry before pinning a view."), true);
        return false;
    }
    const QModelIndex current = source->tree->currentIndex();
    return pinView(*source, source->model->locatorForIndex(current),
                   source->model->expansionPathForIndex(current)) != nullptr;
}

BrecoLangPanel::ViewState* BrecoLangPanel::pinView(
    const ViewState& source, const MaterializationLocator& selected,
    const QVector<MaterializationLocator>& expansionPath) {
    const QString title = QStringLiteral("%1 @ 0x%2")
                              .arg(source.entryName)
                              .arg(source.offset, 0, 16);
    ViewState pinned = createView(title);
    pinned.program = source.program;
    pinned.entryName = source.entryName;
    pinned.offset = source.offset;
    pinned.sources = source.sources;
    pinned.inputPaths = source.inputPaths;
    pinned.document = source.document;
    pinned.shape = source.shape;
    pinned.rootValue = source.rootValue;
    if (!m_decodeController->shareDocument(source.id, pinned.id)) {
        m_viewTabs->removeTab(m_viewTabs->indexOf(pinned.page));
        pinned.page->deleteLater();
        setStatus(QStringLiteral("Could not share the decoded document."), true);
        return nullptr;
    }
    pinned.model->copyDocumentFrom(*source.model);
    m_views.push_back(pinned);
    ViewState& result = m_views.last();
    m_viewTabs->setCurrentWidget(result.page);
    expandDefaultContainers(result);
    if (selected.isValid()) {
        const QModelIndex selectedIndex =
            result.model->indexForLocator(selected, expansionPath);
        if (selectedIndex.isValid()) {
            result.tree->setCurrentIndex(selectedIndex);
            result.tree->scrollTo(selectedIndex);
        }
    }
    setStatus(QStringLiteral("Pinned %1.").arg(title), false);
    return &result;
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
    if (view == nullptr || !view->program || !view->document.isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("Decode an entry before exporting JSON");
        }
        return false;
    }
    DocumentOutputRequest request;
    request.kind = DocumentOutputKind::Json;
    return m_decodeController->renderOutputBlocking(view->id, request, output,
                                                    error);
}

bool BrecoLangPanel::exportBinary(QIODevice* output, QString* error) const {
    const ViewState* view = activeView();
    if (view == nullptr || !view->document.isValid() || !view->shape) {
        if (error != nullptr) {
            *error = QStringLiteral("Decode an entry before exporting binary data");
        }
        return false;
    }
    QModelIndex index = view->tree->currentIndex();
    DocumentOutputRequest request;
    request.kind = DocumentOutputKind::BinarySpans;
    request.target = view->model->locatorForIndex(index);
    if (!request.target.isValid()) {
        request.target = view->shape->root;
    }
    return m_decodeController->renderOutputBlocking(view->id, request, output,
                                                    error);
}

bool BrecoLangPanel::renderOutform(QStringView outformName, QIODevice* output,
                                   QString* error) const {
    const ViewState* view = activeView();
    if (view == nullptr || !view->program || !view->document.isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("Decode an entry before rendering an outform");
        }
        return false;
    }
    DocumentOutputRequest request;
    request.kind = DocumentOutputKind::Outform;
    request.outformName = outformName.toString();
    return m_decodeController->renderOutputBlocking(view->id, request, output,
                                                    error);
}

std::optional<ProbeScanPlan> BrecoLangPanel::probeScanPlan(QString* error) const {
    const ResolvedDecodeTarget target = selectedDecodeTarget(error);
    if (!target.isValid()) {
        return std::nullopt;
    }
    ProbeScanPlan plan;
    plan.program = target.program;
    plan.entryName = target.entryName;
    plan.primaryInput = target.primaryInput;
    plan.inputPaths.resize(target.program->inputs.size());
    for (InputId input = 0;
         input < static_cast<InputId>(target.program->inputs.size());
         ++input) {
        plan.inputPaths[input] = inputPath(input);
        if (input != plan.primaryInput && plan.inputPaths[input].isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("Bind input '%1' before scanning")
                             .arg(target.program->symbol(
                                 target.program->inputs.at(input).name));
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
    PathSelectActivity activity, const QString& caption,
    const QString& filter,
    const std::function<bool(QIODevice*, QString*)>& writer) {
    const QString path = promptForPath(
        this, activity, PathSelectKind::SaveFile, caption, filter);
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
    saveWithCommit(PathSelectActivity::SaveDecodedJson,
                   QStringLiteral("Save decoded JSON"),
                   QStringLiteral("JSON files (*.json);;All files (*)"),
                   [this](QIODevice* output, QString* error) {
                       return exportJson(output, error);
                   });
}

void BrecoLangPanel::saveBinary() {
    saveWithCommit(PathSelectActivity::SaveDecodedBinary,
                   QStringLiteral("Save decoded binary"),
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
    saveWithCommit(PathSelectActivity::SaveOutform,
                   QStringLiteral("Save outform %1").arg(name),
                   QStringLiteral("All files (*)"),
                   [this, name](QIODevice* output, QString* error) {
                       return renderOutform(name, output, error);
                   });
}

void BrecoLangPanel::chooseSchema() {
    const QString path = promptForPath(
        this, PathSelectActivity::OpenBrecoLangSchema,
        PathSelectKind::OpenFile, QStringLiteral("Open BrecoLang schema"),
        QStringLiteral("BrecoLang schemas (*.breco);;All files (*)"));
    if (!path.isEmpty()) {
        loadSchemaFile(path);
    }
}

void BrecoLangPanel::chooseInput(int row) {
    QLineEdit* edit = inputPathEdit(row);
    if (row < 0 || edit == nullptr) {
        return;
    }
    const QString path = promptForPath(
        this, PathSelectActivity::BindBrecoLangInput,
        PathSelectKind::OpenFile, QStringLiteral("Bind BrecoLang input"),
        QStringLiteral("All files (*)"));
    if (!path.isEmpty()) {
        edit->setText(path);
        if (QAbstractButton* button = m_inputViewGroup->button(row);
            button != nullptr && button->isChecked()) {
            emit inputFileActivated(path);
        }
    }
}

void BrecoLangPanel::chooseLibraryDirectory() {
    const QString path = promptForPath(
        this, PathSelectActivity::OpenSchemaLibraryDir,
        PathSelectKind::OpenDirectory,
        QStringLiteral("Choose schema library"));
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

void BrecoLangPanel::highlightSourceRange(const QString& inputPath, quint64 offset,
                                          quint64 length, SourceHighlightMode mode) {
    if (mode == SourceHighlightMode::Hover) {
        if (length == 0) {
            clearSourceHighlight(mode);
            return;
        }
        m_hasPendingHover = true;
        m_pendingHoverPath = inputPath;
        m_pendingHoverOffset = offset;
        m_pendingHoverLength = length;
        m_hoverHighlightTimer->start();
        return;
    }
    if (length == 0) {
        clearSourceHighlight(mode);
        return;
    }
    m_hasSelectionHighlight = true;
    m_selectionHighlightPath = inputPath;
    m_selectionHighlightOffset = offset;
    m_selectionHighlightLength = length;
    ViewState* view = activeView();
    if (view != nullptr) {
        applySourceHighlights(*view);
    }
}

void BrecoLangPanel::setFieldEditingEnabled(bool enabled) {
    m_fieldEditingEnabled = enabled;
}

void BrecoLangPanel::clearSourceHighlight(SourceHighlightMode mode) {
    if (mode == SourceHighlightMode::Hover) {
        m_hasPendingHover = false;
        m_hasHoverHighlight = false;
        if (m_hoverHighlightTimer != nullptr) {
            m_hoverHighlightTimer->stop();
        }
    } else {
        m_hasSelectionHighlight = false;
    }
    ViewState* view = activeView();
    if (view != nullptr) {
        applySourceHighlights(*view);
    }
}

void BrecoLangPanel::applySourceHighlights(ViewState& view) {
    if (view.model == nullptr || view.tree == nullptr) {
        return;
    }
    auto inputIdForPath = [&](const QString& path) -> InputId {
        for (int i = 0; i < view.inputPaths.size(); ++i) {
            if (view.inputPaths.at(i) == path) {
                return static_cast<InputId>(i);
            }
        }
        return kInvalidId;
    };

    QHash<quintptr, QColor> colors;
    QModelIndex innermost;
    int innermostDepth = -1;
    const QColor partial(255, 170, 80);
    const QColor full(120, 200, 110);

    auto addHits = [&](const QString& path, quint64 offset, quint64 length, bool scrollInnermost) {
        const auto hits = view.model->sourceSpansOverlapping(inputIdForPath(path), offset, length);
        for (const auto& hit : hits) {
            if (!hit.index.isValid()) {
                continue;
            }
            QModelIndex parent = hit.index.parent();
            while (parent.isValid()) {
                view.tree->expand(parent);
                parent = parent.parent();
            }
            const QColor color =
                hit.coverage == DecodedTreeModel::SourceSpanCoverage::Full ? full : partial;
            const quintptr id = hit.index.internalId();
            if (!colors.contains(id) || hit.coverage == DecodedTreeModel::SourceSpanCoverage::Full) {
                colors.insert(id, color);
            }
            if (scrollInnermost && hit.depth >= innermostDepth) {
                innermostDepth = hit.depth;
                innermost = hit.index;
            }
        }
    };

    if (m_hasSelectionHighlight) {
        addHits(m_selectionHighlightPath, m_selectionHighlightOffset,
                m_selectionHighlightLength, false);
    }
    if (m_hasHoverHighlight) {
        addHits(m_hoverHighlightPath, m_hoverHighlightOffset, m_hoverHighlightLength, true);
    }
    view.model->setSourceSpanHighlights(colors);
    if (innermost.isValid()) {
        view.tree->scrollTo(innermost, QAbstractItemView::PositionAtCenter);
    }
}

bool BrecoLangPanel::editDecodedValue(ViewState& view, const QModelIndex& index) {
    if (!m_fieldEditingEnabled || view.model == nullptr || index.column() != 2) {
        return false;
    }
    const DecodedNode* node = view.model->nodeForIndex(index);
    if (node == nullptr || !node->hasSourceSpan || node->length == 0) {
        return false;
    }
    if (node->kind == DecodedNodeKind::Sequence || node->kind == DecodedNodeKind::Record ||
        node->kind == DecodedNodeKind::Computed) {
        return false;
    }
    if (node->input >= static_cast<InputId>(view.inputPaths.size()) ||
        view.inputPaths.at(node->input).isEmpty()) {
        return false;
    }
    const std::shared_ptr<const DecodedTree> tree = view.model->treeForIndex(index);
    if (!tree || !view.program) {
        return false;
    }
    const QString current = index.data(Qt::DisplayRole).toString();
    bool ok = false;
    const QString typed = QInputDialog::getText(
        this, QStringLiteral("Edit value"), QStringLiteral("New value"),
        QLineEdit::Normal, current, &ok);
    if (!ok) {
        return false;
    }

    QByteArray original(static_cast<int>(node->length), '\0');
    if (node->input < static_cast<InputId>(view.sources.size()) &&
        view.sources.at(node->input)) {
        const ByteReadResult read = view.sources.at(node->input)->read(
            node->offset, static_cast<qsizetype>(node->length));
        if (read.ok() && read.view.data() != nullptr &&
            read.view.length == static_cast<qsizetype>(node->length)) {
            original = QByteArray(read.view.data(), static_cast<int>(read.view.length));
        }
    }
    TypeKind kind = TypeKind::Invalid;
    Endianness endian = Endianness::None;
    quint16 bitWidth = 0;
    bool bitSlice = false;
    quint8 highBit = 0;
    quint8 lowBit = 0;
    if (node->type < static_cast<TypeId>(view.program->types.size())) {
        const TypeDesc& type = view.program->types.at(node->type);
        kind = type.kind;
        endian = type.endianness;
        bitWidth = type.bitWidth;
    }
    if (node->storageLayout < static_cast<quint32>(tree->storageLayouts.size())) {
        const StorageLayout& layout = tree->storageLayouts.at(node->storageLayout);
        if (layout.endianness != Endianness::None) {
            endian = layout.endianness;
        }
        if (layout.bitWidth != 0) {
            bitWidth = layout.bitWidth;
        }
        if (layout.kind == StorageLayoutKind::BitSlice) {
            bitSlice = true;
            highBit = layout.highBit;
            lowBit = layout.lowBit;
        }
    }

    QByteArray encoded;
    if (kind == TypeKind::String || kind == TypeKind::Bytes) {
        encoded = typed.toUtf8();
        if (encoded.size() > original.size()) {
            encoded.truncate(original.size());
        }
        while (encoded.size() < original.size()) {
            encoded.append('\0');
        }
    } else if (kind == TypeKind::Boolean) {
        const bool value = typed.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
                           typed == QStringLiteral("1");
        encoded = QByteArray(1, value ? 1 : 0);
        if (original.size() > 1) {
            encoded.append(QByteArray(original.size() - 1, '\0'));
        }
    } else if (kind == TypeKind::SignedInteger || kind == TypeKind::UnsignedInteger ||
               kind == TypeKind::Enum) {
        const int width = bitWidth > 0 ? (bitWidth + 7) / 8 : original.size();
        const bool little = endian != Endianness::Big;
        const auto packed = ::breco::EditQueue::packInteger(
            typed, qMax(1, width), kind == TypeKind::SignedInteger, little, 10);
        if (!packed.has_value()) {
            setStatus(QStringLiteral("Could not parse value."), true);
            return false;
        }
        encoded = packed.value();
        if (bitSlice) {
            const quint8 lo = qMin(lowBit, highBit);
            const quint8 hi = qMax(lowBit, highBit);
            const int bitCount = hi - lo + 1;
            quint64 incoming = 0;
            for (int i = 0; i < encoded.size(); ++i) {
                const int shift = little ? (i * 8) : ((encoded.size() - 1 - i) * 8);
                incoming |= static_cast<quint64>(static_cast<unsigned char>(encoded.at(i)))
                            << shift;
            }
            const quint64 valueMask =
                (bitCount >= 64) ? ~0ULL : ((1ULL << bitCount) - 1ULL);
            if (kind != TypeKind::SignedInteger && incoming > valueMask) {
                setStatus(QStringLiteral("Value does not fit the bitfield."), true);
                return false;
            }
            incoming &= valueMask;
            encoded = original;
            quint64 carrier = 0;
            for (int i = 0; i < encoded.size(); ++i) {
                const int shift = little ? (i * 8) : ((encoded.size() - 1 - i) * 8);
                carrier |= static_cast<quint64>(static_cast<unsigned char>(encoded.at(i)))
                           << shift;
            }
            const quint64 fieldMask = valueMask << lo;
            carrier = (carrier & ~fieldMask) | ((incoming << lo) & fieldMask);
            for (int i = 0; i < encoded.size(); ++i) {
                const int shift = little ? (i * 8) : ((encoded.size() - 1 - i) * 8);
                encoded[i] = static_cast<char>((carrier >> shift) & 0xFFULL);
            }
        } else if (encoded.size() < original.size()) {
            encoded.append(QByteArray(original.size() - encoded.size(), '\0'));
        } else if (encoded.size() > original.size()) {
            encoded.truncate(original.size());
        }
    } else {
        setStatus(QStringLiteral("This field type cannot be edited."), true);
        return false;
    }

    emit fieldEditCommitted(view.inputPaths.at(node->input), node->offset, original,
                            encoded);
    return true;
}

}  // namespace breco::lang

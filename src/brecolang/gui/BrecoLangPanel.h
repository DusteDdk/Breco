#pragma once

#include "settings/PathSelect.h"

#include <QWidget>
#include <QHash>
#include <QByteArray>

#include <memory>
#include <optional>
#include <functional>

#include "brecolang/runtime/DecodeTarget.h"
#include "brecolang/runtime/Interpreter.h"
#include "brecolang/runtime/ProbeScan.h"

QT_BEGIN_NAMESPACE
class QButtonGroup;
class QComboBox;
class QDockWidget;
class QLabel;
class QLineEdit;
class QMainWindow;
class QIODevice;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTimer;
class QTreeView;
class QWidget;
QT_END_NAMESPACE

namespace breco::lang {

class DecodedTreeModel;
class BrecoDecodeController;
struct ResolveResponse;
struct DisplayPageResponse;

class BrecoLangPanel final : public QWidget {
    Q_OBJECT

public:
    explicit BrecoLangPanel(QWidget* parent = nullptr);

    bool loadSchemaFile(const QString& path);
    bool loadSchemaText(const QString& source, const QString& sourcePath = {});
    void setSuggestedInputPath(const QString& path);
    bool setInputPath(QStringView role, const QString& path);
    bool selectEntry(QStringView entryName);
    void setDecodeOffset(quint64 offset);
    quint64 decodeOffset() const;
    bool decodeSelected();
    bool pinCurrentView();

    bool exportJson(QIODevice* output, QString* error = nullptr) const;
    bool exportBinary(QIODevice* output, QString* error = nullptr) const;
    bool renderOutform(QStringView outformName, QIODevice* output,
                       QString* error = nullptr) const;
    std::optional<ProbeScanPlan> probeScanPlan(QString* error = nullptr) const;
    void setScanRunning(bool running);

    void setLibraryDirectory(const QString& path);
    QString libraryDirectory() const;
    void refreshLibrary();
    QString migrationNoticeText() const;

    QComboBox* entryComboBox() const { return m_entryCombo; }
    QComboBox* outformComboBox() const { return m_outformCombo; }
    QTableWidget* inputTable() const { return m_inputTable; }
    QTreeView* treeView() const;
    QPushButton* expandAllButton() const { return m_expandAllButton; }
    QPushButton* pinViewButton() const { return m_pinViewButton; }
    QPushButton* scanButton() const { return m_scanButton; }
    QTabWidget* viewTabs() const { return m_viewTabs; }
    QPlainTextEdit* schemaEditor() const { return m_schemaEditor; }
    QDockWidget* schemaDockWidget() const { return m_schemaDock; }
    QDockWidget* decodeDockWidget() const { return m_decodeDock; }
    QMainWindow* workspaceWindow() const { return m_workspaceWindow; }
    DecodedTreeModel* treeModel() const;
    QString statusText() const;
    std::shared_ptr<const BrecoProgram> program() const { return m_program; }
    std::shared_ptr<const DecodedTree> tree() const;

    enum class SourceHighlightMode { Hover, Selection };
    void highlightSourceRange(const QString& inputPath, quint64 offset, quint64 length,
                              SourceHighlightMode mode);
    void clearSourceHighlight(SourceHighlightMode mode);
    void setFieldEditingEnabled(bool enabled);
    bool fieldEditingEnabled() const { return m_fieldEditingEnabled; }

signals:
    void scanRequested();
    void scanStopRequested();
    void inputFileActivated(const QString& filePath);
    void sourceLocationActivated(const QString& filePath,
                                 quint64 absoluteOffset,
                                 quint64 byteLength);
    void fieldEditCommitted(const QString& filePath, quint64 offset,
                            const QByteArray& originalBytes,
                            const QByteArray& newBytes);
    void schemaFileLoaded(const QString& filePath);
    void libraryDirectoryChanged(const QString& directory);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct DecodeSelection {
        DecodeTargetKind kind = DecodeTargetKind::Entry;
        QString name;

        bool isValid() const { return !name.isEmpty(); }
    };

    struct ViewState {
        QWidget* page = nullptr;
        quint64 id = 0;
        QTreeView* tree = nullptr;
        DecodedTreeModel* model = nullptr;
        std::shared_ptr<const BrecoProgram> program;
        QString entryName;
        quint64 offset = 0;
        QVector<std::shared_ptr<ByteSource>> sources;
        QVector<QString> inputPaths;
        DecodeDocumentHandle document;
        std::shared_ptr<const ResolvedShapeSnapshot> shape;
        DecodedValueId rootValue = kInvalidId;
    };

    void compileEditor();
    void compileSource(bool selectDefault);
    void populateProgramControls(const QHash<QString, QString>& preservedPaths = {},
                                 const std::optional<DecodeSelection>&
                                     preservedSelection = std::nullopt,
                                 bool selectDefault = false);
    void chooseSchema();
    void chooseInput(int row);
    void chooseLibraryDirectory();
    void saveJson();
    void saveBinary();
    void saveOutform();
    void setStatus(const QString& text, bool error);
    QLineEdit* inputPathEdit(int row) const;
    QString inputPath(InputId input) const;
    QHash<QString, QString> currentInputPaths() const;
    std::optional<DecodeSelection> currentDecodeSelection() const;
    int findDecodeSelection(const DecodeSelection& selection) const;
    ResolvedDecodeTarget selectedDecodeTarget(QString* error = nullptr) const;
    void setViewedInput(InputId input);
    ViewState* liveView();
    const ViewState* activeView() const;
    ViewState* activeView();
    ViewState createView(const QString& title);
    ViewState* pinView(
        const ViewState& source,
        const MaterializationLocator& selected = MaterializationLocator{},
        const QVector<MaterializationLocator>& expansionPath = {});
    void activateTreeIndex(QTreeView* treeView, const QModelIndex& index);
    void installViewNavigation(ViewState& view);
    void expandDefaultContainers(ViewState& view);
    void applySourceHighlights(ViewState& view);
    bool editDecodedValue(ViewState& view, const QModelIndex& index);
    void requestUnshownSequencePages(ViewState& view);
    void ensureWorkspaceLayout();
    void handleResolveFinished(const ResolveResponse& response);
    void handleDisplayPageFinished(const DisplayPageResponse& response);
    ViewState* viewById(quint64 id);
    const ViewState* viewById(quint64 id) const;
    bool saveWithCommit(PathSelectActivity activity, const QString& caption,
                        const QString& filter,
                        const std::function<bool(QIODevice*, QString*)>& writer);

    QLineEdit* m_schemaPath = nullptr;
    QLineEdit* m_libraryPath = nullptr;
    QLineEdit* m_offsetEdit = nullptr;
    QComboBox* m_libraryCombo = nullptr;
    QComboBox* m_entryCombo = nullptr;
    QComboBox* m_outformCombo = nullptr;
    QButtonGroup* m_inputViewGroup = nullptr;
    QTableWidget* m_inputTable = nullptr;
    QPlainTextEdit* m_schemaEditor = nullptr;
    QMainWindow* m_workspaceWindow = nullptr;
    QDockWidget* m_schemaDock = nullptr;
    QDockWidget* m_decodeDock = nullptr;
    QTabWidget* m_viewTabs = nullptr;
    QLabel* m_migrationNotice = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_expandAllButton = nullptr;
    QPushButton* m_pinViewButton = nullptr;
    QPushButton* m_scanButton = nullptr;
    QTimer* m_compileTimer = nullptr;
    QTimer* m_hoverHighlightTimer = nullptr;
    QString m_pendingHoverPath;
    quint64 m_pendingHoverOffset = 0;
    quint64 m_pendingHoverLength = 0;
    bool m_hasPendingHover = false;
    QString m_hoverHighlightPath;
    quint64 m_hoverHighlightOffset = 0;
    quint64 m_hoverHighlightLength = 0;
    bool m_hasHoverHighlight = false;
    QString m_selectionHighlightPath;
    quint64 m_selectionHighlightOffset = 0;
    quint64 m_selectionHighlightLength = 0;
    bool m_hasSelectionHighlight = false;
    BrecoDecodeController* m_decodeController = nullptr;
    QString m_suggestedInputPath;
    QString m_sourcePath;
    std::optional<DecodeSelection> m_preservedDecodeSelection;
    QHash<QString, QString> m_preservedInputPaths;
    std::shared_ptr<const BrecoProgram> m_program;
    QVector<ViewState> m_views;
    bool m_updatingEditor = false;
    bool m_liveDecoded = false;
    bool m_scanRunning = false;
    bool m_expandingDefaults = false;
    bool m_workspaceLayoutApplied = false;
    bool m_fieldEditingEnabled = false;
    quint64 m_nextViewId = 1;
};

}  // namespace breco::lang

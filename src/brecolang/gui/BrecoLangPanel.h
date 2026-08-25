#pragma once

#include <QWidget>
#include <QHash>

#include <memory>
#include <optional>
#include <functional>

#include "brecolang/runtime/Interpreter.h"
#include "brecolang/runtime/ProbeScan.h"

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
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
    DecodedTreeModel* treeModel() const;
    QString statusText() const;
    std::shared_ptr<const BrecoProgram> program() const { return m_program; }
    std::shared_ptr<const DecodedTree> tree() const;

signals:
    void scanRequested();
    void scanStopRequested();
    void sourceLocationActivated(const QString& filePath,
                                 quint64 absoluteOffset,
                                 quint64 byteLength);
    void schemaFileLoaded(const QString& filePath);
    void libraryDirectoryChanged(const QString& directory);

private:
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
    void populateProgramControls(const QHash<QString, QString>& preservedPaths = {},
                                 const QString& preservedEntry = {});
    void chooseSchema();
    void chooseInput();
    void chooseLibraryDirectory();
    void saveJson();
    void saveBinary();
    void saveOutform();
    void setStatus(const QString& text, bool error);
    QString inputPath(InputId input) const;
    QHash<QString, QString> currentInputPaths() const;
    ViewState* liveView();
    const ViewState* activeView() const;
    ViewState* activeView();
    ViewState createView(const QString& title);
    void installViewNavigation(ViewState& view);
    void handleResolveFinished(const ResolveResponse& response);
    void handleDisplayPageFinished(const DisplayPageResponse& response);
    ViewState* viewById(quint64 id);
    const ViewState* viewById(quint64 id) const;
    bool saveWithCommit(const QString& caption, const QString& filter,
                        const std::function<bool(QIODevice*, QString*)>& writer);

    QLineEdit* m_schemaPath = nullptr;
    QLineEdit* m_libraryPath = nullptr;
    QLineEdit* m_offsetEdit = nullptr;
    QComboBox* m_libraryCombo = nullptr;
    QComboBox* m_entryCombo = nullptr;
    QComboBox* m_outformCombo = nullptr;
    QTableWidget* m_inputTable = nullptr;
    QPlainTextEdit* m_schemaEditor = nullptr;
    QTabWidget* m_viewTabs = nullptr;
    QLabel* m_migrationNotice = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_expandAllButton = nullptr;
    QPushButton* m_pinViewButton = nullptr;
    QPushButton* m_scanButton = nullptr;
    QTimer* m_compileTimer = nullptr;
    BrecoDecodeController* m_decodeController = nullptr;
    QString m_suggestedInputPath;
    QString m_sourcePath;
    std::shared_ptr<const BrecoProgram> m_program;
    QVector<ViewState> m_views;
    bool m_updatingEditor = false;
    bool m_liveDecoded = false;
    bool m_scanRunning = false;
    quint64 m_nextViewId = 1;
};

}  // namespace breco::lang

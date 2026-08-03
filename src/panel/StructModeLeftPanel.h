#pragma once

#include <QHash>
#include <QPoint>
#include <QVector>
#include <QWidget>

#include <memory>

#include "struct/StructureGraph.h"

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QEvent;
class QLabel;
class QPlainTextEdit;
class QSpinBox;
class QSplitter;
class QTableWidget;
class QTreeWidget;
class QToolButton;
class QVBoxLayout;
namespace Ui {
class StructModeLeftPanel;
}
QT_END_NAMESPACE

namespace breco {

struct CurrentStructView {
    quint64 id = 0;
    QString name;
    QString type;
    int repeat = 1;
    quint64 offset = 0;
    quint64 byteLength = 0;
    QString filePath;
};

class StructModeLeftPanel : public QWidget {
    Q_OBJECT

public:
    explicit StructModeLeftPanel(QWidget* parent = nullptr);
    ~StructModeLeftPanel() override;

    QPlainTextEdit* structDeclarationEdit() const;
    QComboBox* entryComboBox() const;
    QSpinBox* entryCountSpinBox() const;
    QCheckBox* previewEnabledCheckBox() const;
    QTableWidget* currentViewsTableWidget() const;
    QToolButton* addViewButton() const;
    QToolButton* removeViewButton() const;
    QToolButton* scanStructureButton() const;
    QTreeWidget* structureLibraryTreeWidget() const { return m_libraryTree; }
    QVBoxLayout* structDeclarationLayout() const;

    const StructureGraph& structureGraph() const { return m_graph; }
    bool isParseValid() const { return m_parseValid; }
    bool canPreview() const;
    bool previewEnabled() const;
    bool previewActive() const { return m_previewActive; }
    QString declarationText() const;
    QVector<CurrentStructView> currentViews() const;

    void reparseDeclaration();
    void highlightSelectedEntry();
    void focusDeclarationRange(int start, int end);
    void setPreviewActive(bool active);
    void setScanState(bool running, bool structureScan);
    void addCurrentView(const CurrentStructView& view);
    void setCurrentViewByteLength(quint64 id, quint64 byteLength);
    void clearCurrentViews();
    bool loadDeclarationFromFile(const QString& filePath);
    bool saveDeclarationToFile(const QString& filePath) const;
    void insertLanguageSnippet(const QString& snippet);

    static QString languageSnippetForObjectName(const QString& objectName);
    static bool parseAbsoluteOffset(const QString& text, quint64* offset);

signals:
    void previewRequested();
    void previewClearRequested();
    void addViewRequested();
    void currentViewsRemoved(const QVector<quint64>& ids);
    void currentViewChanged(quint64 id, const QString& name, int repeat, quint64 offset);
    void sourceLocationActivated(const QString& filePath,
                                 quint64 absoluteOffset, quint64 byteLength);
    void parseStateChanged();
    void declarationFileLoaded(const QString& filePath);
    void structureScanRequested();
    void structureScanStopRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateEntryCombo();
    void updatePreviewControls();
    void updateStatusAndErrorHighlight();
    void updateRemoveEnabled();
    void setupLanguageReference();
    void setupStructureLibrary();
    void refreshStructureLibrary();
    void chooseStructureLibraryDirectory();
    void handleLoadRequested();
    void handleSaveRequested();
    void removeSelectedViews();
    int rowForViewId(quint64 id) const;
    CurrentStructView viewForRow(int row) const;
    void emitViewChangedForRow(int row);

    std::unique_ptr<Ui::StructModeLeftPanel> m_ui;
    StructureGraph m_graph;
    bool m_parseValid = false;
    bool m_isEmpty = true;
    bool m_previewActive = false;
    bool m_updatingTable = false;
    bool m_scanRunning = false;
    bool m_structureScanRunning = false;
    QString m_parseError;
    TextRange m_parseErrorRange;
    QPoint m_dragStartPosition;
    QLabel* m_dragLabel = nullptr;
    QHash<QObject*, QString> m_languageSnippets;
    QSplitter* m_sectionSplitter = nullptr;
    QWidget* m_libraryWidget = nullptr;
    QTreeWidget* m_libraryTree = nullptr;
};

}  // namespace breco

#pragma once

#include <QByteArray>
#include <QWidget>

#include <memory>

#include "visualize/VisualizeData.h"

QT_BEGIN_NAMESPACE
class QDockWidget;
class QEvent;
class QLabel;
class QMainWindow;
class QPlainTextEdit;
class QRadioButton;
class QStackedWidget;
class QTimer;
namespace Ui {
class VisualizePanel;
}
QT_END_NAMESPACE

namespace breco {

class Cartesian2DView;
class Cartesian3DView;
class VisualizeBitmapCanvas;

class VisualizePanel final : public QWidget {
    Q_OBJECT

public:
    explicit VisualizePanel(QWidget* parent = nullptr);
    ~VisualizePanel() override;

    void setData(QByteArray bytes, quint64 baseOffset, bool truncated = false,
                 quint64 fileSize = 0);
    void clearData();
    void setProgramDirectory(const QString& directory);
    void flushProgram();
    quint64 defaultWindowBytes() const { return m_defaultWindowBytes; }

    VisualizationMode visualizationMode() const;
    void setVisualizationMode(VisualizationMode mode);
    QString schemaText() const;
    void setSchemaText(const QString& text);

    QRadioButton* cartesian2DRadioButton() const;
    QRadioButton* cartesian3DRadioButton() const;
    QRadioButton* bitmapRadioButton() const;
    QLabel* statusLabel() const;
    QDockWidget* schemaDockWidget() const { return m_schemaDock; }
    QPlainTextEdit* schemaEditor() const { return m_schemaEditor; }
    QDockWidget* resultDockWidget() const { return m_resultDock; }
    QMainWindow* workspaceWindow() const { return m_workspaceWindow; }
    VisualizeBitmapCanvas* bitmapCanvas() const { return m_bitmapCanvas; }
    Cartesian2DView* cartesian2DView() const { return m_cartesian2DView; }
    Cartesian3DView* cartesian3DView() const { return m_cartesian3DView; }

signals:
    void configurationChanged();
    void defaultWindowBytesChanged();
    void inputWindowRequested(quint64 start, quint64 length);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Cartesian3DView* ensureCartesian3DView();
    void updateControlVisibility();
    void ensureWorkspaceLayout();
    void updateSchemaDockFeatures(bool floating);
    void updateResultDockFeatures(bool floating);
    void rebuildVisualization();
    void loadProgram();
    void saveProgram();
    void updateDefaultWindowBytes(quint64 bytes);
    void handleInputExtentResized(double areaScale, bool keepEnd);
    quint64 availableStart() const;
    quint64 availableEnd() const;

    std::unique_ptr<Ui::VisualizePanel> m_ui;
    QByteArray m_bytes;
    QString m_programPath;
    QString m_schemaSource;
    bool m_programDirty = false;
    quint64 m_defaultWindowBytes = kDefaultVisualizationBytes;
    quint64 m_baseOffset = 0;
    quint64 m_fileSize = 0;
    bool m_truncated = false;
    Cartesian2DView* m_cartesian2DView = nullptr;
    Cartesian3DView* m_cartesian3DView = nullptr;
    VisualizeBitmapCanvas* m_bitmapCanvas = nullptr;
    QMainWindow* m_workspaceWindow = nullptr;
    QDockWidget* m_schemaDock = nullptr;
    QDockWidget* m_resultDock = nullptr;
    QPlainTextEdit* m_schemaEditor = nullptr;
    QTimer* m_programTimer = nullptr;
    bool m_workspaceLayoutApplied = false;
};

}  // namespace breco

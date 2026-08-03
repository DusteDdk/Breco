#pragma once

#include <QMainWindow>

QT_BEGIN_NAMESPACE
class QDockWidget;
QT_END_NAMESPACE

namespace breco {

class DataViewStructuredPanel : public QMainWindow {
    Q_OBJECT

public:
    explicit DataViewStructuredPanel(QWidget* parent = nullptr);
    ~DataViewStructuredPanel() override;

    QWidget* structEditorHost() const;
    QWidget* structViewHost() const;
    QDockWidget* structEditorDock() const { return m_structEditorDock; }
    QDockWidget* structViewDock() const { return m_structViewDock; }

private:
    QWidget* m_structEditorHost = nullptr;
    QWidget* m_structViewHost = nullptr;
    QDockWidget* m_structEditorDock = nullptr;
    QDockWidget* m_structViewDock = nullptr;
};

}  // namespace breco

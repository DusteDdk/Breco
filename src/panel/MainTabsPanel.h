#pragma once

#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QFrame;
class QTabWidget;
class QVBoxLayout;
class QWidget;
namespace Ui {
class MainTabsPanel;
}
QT_END_NAMESPACE

namespace breco {

class ScanControlsPanel;

class MainTabsPanel : public QWidget {
    Q_OBJECT

public:
    explicit MainTabsPanel(QWidget* parent = nullptr);
    ~MainTabsPanel() override;

    QTabWidget* mainTabWidget() const;
    QVBoxLayout* scanTabLayout() const;
    ScanControlsPanel* scanControlsPanel() const;
    QWidget* resultsPanelHost() const;
    QWidget* dataViewHost() const;
    QFrame* editStack() const;

private:
    std::unique_ptr<Ui::MainTabsPanel> m_ui;
};

}  // namespace breco

#pragma once

#include <QWidget>
#include <QHash>

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
    void activateScanTab();
    bool detachTab(int index);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    std::unique_ptr<Ui::MainTabsPanel> m_ui;
    struct DetachedTab {
        QWidget* window = nullptr;
        QString title;
        int index = 0;
    };
    QHash<QWidget*, DetachedTab> m_detachedTabs;
};

}  // namespace breco

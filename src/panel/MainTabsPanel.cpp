#include "panel/MainTabsPanel.h"

#include "ui_MainTabsPanel.h"

namespace breco {

MainTabsPanel::MainTabsPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::MainTabsPanel>()) {
    m_ui->setupUi(this);
}

MainTabsPanel::~MainTabsPanel() = default;

QTabWidget* MainTabsPanel::mainTabWidget() const { return m_ui->mainTabWidget; }

QVBoxLayout* MainTabsPanel::scanTabLayout() const { return m_ui->scanTabLayout; }

ScanControlsPanel* MainTabsPanel::scanControlsPanel() const {
    return m_ui->scanControlsPanel;
}

QWidget* MainTabsPanel::resultsPanelHost() const { return m_ui->resultsPanelHost; }

QWidget* MainTabsPanel::dataViewHost() const { return m_ui->DataViewHost; }

QFrame* MainTabsPanel::editStack() const { return m_ui->editStack; }

}  // namespace breco

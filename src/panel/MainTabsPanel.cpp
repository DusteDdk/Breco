#include "panel/MainTabsPanel.h"

#include "ui_MainTabsPanel.h"

#include <QCloseEvent>
#include <QMenu>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

namespace breco {

MainTabsPanel::MainTabsPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::MainTabsPanel>()) {
    m_ui->setupUi(this);
    m_ui->mainTabWidget->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_ui->mainTabWidget->tabBar(), &QWidget::customContextMenuRequested,
            this, [this](const QPoint& point) {
                const int index = m_ui->mainTabWidget->tabBar()->tabAt(point);
                if (index < 0) {
                    return;
                }
                QMenu menu(this);
                QAction* detach = menu.addAction(QStringLiteral("Detach view"));
                if (menu.exec(m_ui->mainTabWidget->tabBar()->mapToGlobal(point)) == detach) {
                    detachTab(index);
                }
            });
    connect(m_ui->mainTabWidget->tabBar(), &QTabBar::tabBarDoubleClicked,
            this, &MainTabsPanel::detachTab);
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

void MainTabsPanel::activateScanTab() {
    const int index = m_ui->mainTabWidget->indexOf(m_ui->scanTab);
    if (index >= 0) {
        m_ui->mainTabWidget->setCurrentIndex(index);
        return;
    }
    const auto detached = m_detachedTabs.constFind(m_ui->scanTab);
    if (detached != m_detachedTabs.constEnd() && detached->window != nullptr) {
        detached->window->show();
        detached->window->raise();
        detached->window->activateWindow();
    }
}

bool MainTabsPanel::detachTab(int index) {
    if (index < 0 || index >= m_ui->mainTabWidget->count()) {
        return false;
    }
    QWidget* page = m_ui->mainTabWidget->widget(index);
    const QString title = m_ui->mainTabWidget->tabText(index);
    m_ui->mainTabWidget->removeTab(index);
    auto* window = new QWidget(nullptr, Qt::Window);
    window->setAttribute(Qt::WA_DeleteOnClose, false);
    window->setWindowTitle(title);
    window->resize(page->size().expandedTo(QSize(640, 480)));
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(page);
    window->installEventFilter(this);
    m_detachedTabs.insert(page, {window, title, index});
    window->show();
    return true;
}

bool MainTabsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Close) {
        for (auto it = m_detachedTabs.begin(); it != m_detachedTabs.end(); ++it) {
            if (it->window == watched) {
                QWidget* page = it.key();
                const DetachedTab detached = it.value();
                page->setParent(m_ui->mainTabWidget);
                m_ui->mainTabWidget->insertTab(
                    qMin(detached.index, m_ui->mainTabWidget->count()), page,
                    detached.title);
                m_detachedTabs.erase(it);
                detached.window->deleteLater();
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace breco

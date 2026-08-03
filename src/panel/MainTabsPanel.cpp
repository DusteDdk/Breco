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
    for (int index = 0; index < m_ui->mainTabWidget->count(); ++index) {
        m_pageOrder.insert(m_ui->mainTabWidget->widget(index), index);
    }
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

MainTabsPanel::~MainTabsPanel() {
    const auto pages = m_detachedTabs.keys();
    for (QWidget* page : pages) {
        const DetachedTab detached = m_detachedTabs.take(page);
        if (detached.window != nullptr) {
            detached.window->removeEventFilter(this);
            page->setParent(m_ui->mainTabWidget);
            delete detached.window;
        }
    }
}

QTabWidget* MainTabsPanel::mainTabWidget() const { return m_ui->mainTabWidget; }

QVBoxLayout* MainTabsPanel::scanTabLayout() const { return m_ui->scanTabLayout; }

ScanControlsPanel* MainTabsPanel::scanControlsPanel() const {
    return m_ui->scanControlsPanel;
}

QWidget* MainTabsPanel::resultsPanelHost() const { return m_ui->resultsPanelHost; }

QWidget* MainTabsPanel::rawDataHost() const { return m_ui->rawDataHost; }

QWidget* MainTabsPanel::structDataHost() const { return m_ui->structDataHost; }

QWidget* MainTabsPanel::imageDataHost() const { return m_ui->imageDataHost; }

QWidget* MainTabsPanel::rawDataTab() const { return m_ui->rawDataTab; }

QWidget* MainTabsPanel::structDataTab() const { return m_ui->structDataTab; }

QWidget* MainTabsPanel::imageDataTab() const { return m_ui->imageDataTab; }

QFrame* MainTabsPanel::editStack() const { return m_ui->editStack; }

void MainTabsPanel::activateScanTab() {
    activateTab(m_ui->scanTab);
}

void MainTabsPanel::activateTab(QWidget* page) {
    const int index = m_ui->mainTabWidget->indexOf(page);
    if (index >= 0) {
        m_ui->mainTabWidget->setCurrentIndex(index);
        return;
    }
    const auto detached = m_detachedTabs.constFind(page);
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
    if (m_detachedTabs.contains(page)) {
        return false;
    }
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
    m_detachedTabs.insert(page, {window, title, m_pageOrder.value(page, index)});
    window->show();
    return true;
}

bool MainTabsPanel::isTabDetached(QWidget* page) const {
    return m_detachedTabs.contains(page);
}

QWidget* MainTabsPanel::detachedWindow(QWidget* page) const {
    const auto detached = m_detachedTabs.constFind(page);
    return detached == m_detachedTabs.constEnd() ? nullptr : detached->window;
}

void MainTabsPanel::reattachTab(QWidget* page) {
    const auto found = m_detachedTabs.find(page);
    if (found == m_detachedTabs.end()) {
        return;
    }
    const DetachedTab detached = found.value();
    int insertionIndex = m_ui->mainTabWidget->count();
    for (int index = 0; index < m_ui->mainTabWidget->count(); ++index) {
        QWidget* existing = m_ui->mainTabWidget->widget(index);
        if (m_pageOrder.value(existing, index) > detached.order) {
            insertionIndex = index;
            break;
        }
    }
    page->setParent(m_ui->mainTabWidget);
    m_ui->mainTabWidget->insertTab(insertionIndex, page, detached.title);
    m_detachedTabs.erase(found);
    detached.window->deleteLater();
}

bool MainTabsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Close) {
        QWidget* pageToReattach = nullptr;
        for (auto it = m_detachedTabs.cbegin(); it != m_detachedTabs.cend(); ++it) {
            if (it->window == watched) {
                pageToReattach = it.key();
                break;
            }
        }
        if (pageToReattach != nullptr) {
            reattachTab(pageToReattach);
        }
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace breco

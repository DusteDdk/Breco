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
    m_brecoLangTab = new QWidget(m_ui->mainTabWidget);
    m_brecoLangTab->setObjectName(QStringLiteral("brecoLangTab"));
    auto* brecoLangLayout = new QVBoxLayout(m_brecoLangTab);
    brecoLangLayout->setContentsMargins(0, 0, 0, 0);
    brecoLangLayout->setSpacing(0);
    m_brecoLangHost = new QWidget(m_brecoLangTab);
    m_brecoLangHost->setObjectName(QStringLiteral("brecoLangHost"));
    brecoLangLayout->addWidget(m_brecoLangHost);
    const int imageIndex = m_ui->mainTabWidget->indexOf(m_ui->imageDataTab);
    m_ui->mainTabWidget->insertTab(imageIndex, m_brecoLangTab,
                                   QStringLiteral("BrecoLang"));
    m_visualizeTab = new QWidget(m_ui->mainTabWidget);
    m_visualizeTab->setObjectName(QStringLiteral("visualizeTab"));
    auto* visualizeLayout = new QVBoxLayout(m_visualizeTab);
    visualizeLayout->setContentsMargins(0, 0, 0, 0);
    visualizeLayout->setSpacing(0);
    m_visualizeHost = new QWidget(m_visualizeTab);
    m_visualizeHost->setObjectName(QStringLiteral("visualizeHost"));
    visualizeLayout->addWidget(m_visualizeHost);
    m_ui->mainTabWidget->insertTab(
        m_ui->mainTabWidget->indexOf(m_ui->imageDataTab), m_visualizeTab,
        QStringLiteral("Visualize"));
    m_editsTab = new QWidget(m_ui->mainTabWidget);
    m_editsTab->setObjectName(QStringLiteral("editsTab"));
    auto* editsLayout = new QVBoxLayout(m_editsTab);
    editsLayout->setContentsMargins(0, 0, 0, 0);
    editsLayout->setSpacing(0);
    m_editsHost = new QWidget(m_editsTab);
    m_editsHost->setObjectName(QStringLiteral("editsHost"));
    editsLayout->addWidget(m_editsHost);
    m_ui->mainTabWidget->insertTab(
        m_ui->mainTabWidget->indexOf(m_ui->imageDataTab), m_editsTab,
        QStringLiteral("Edits"));
    for (int index = 0; index < m_ui->mainTabWidget->count(); ++index) {
        m_pageOrder.insert(m_ui->mainTabWidget->widget(index), index);
    }
    m_editsTabOrder = m_pageOrder.value(m_editsTab, m_ui->mainTabWidget->indexOf(m_editsTab));
    setEditsTabVisible(false);
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

QWidget* MainTabsPanel::brecoLangHost() const { return m_brecoLangHost; }

QWidget* MainTabsPanel::visualizeHost() const { return m_visualizeHost; }

QWidget* MainTabsPanel::editsHost() const { return m_editsHost; }

QWidget* MainTabsPanel::imageDataHost() const { return m_ui->imageDataHost; }

QWidget* MainTabsPanel::rawDataTab() const { return m_ui->rawDataTab; }

QWidget* MainTabsPanel::brecoLangTab() const { return m_brecoLangTab; }

QWidget* MainTabsPanel::visualizeTab() const { return m_visualizeTab; }

QWidget* MainTabsPanel::editsTab() const { return m_editsTab; }

QWidget* MainTabsPanel::imageDataTab() const { return m_ui->imageDataTab; }

QFrame* MainTabsPanel::editStack() const { return m_ui->editStack; }

void MainTabsPanel::setEditsTabVisible(bool visible) {
    if (m_editsTab == nullptr) {
        return;
    }
    const int index = m_ui->mainTabWidget->indexOf(m_editsTab);
    if (visible) {
        if (index < 0 && !m_detachedTabs.contains(m_editsTab)) {
            int insertionIndex = m_ui->mainTabWidget->count();
            for (int i = 0; i < m_ui->mainTabWidget->count(); ++i) {
                QWidget* existing = m_ui->mainTabWidget->widget(i);
                if (m_pageOrder.value(existing, i) > m_editsTabOrder) {
                    insertionIndex = i;
                    break;
                }
            }
            m_ui->mainTabWidget->insertTab(insertionIndex, m_editsTab,
                                           QStringLiteral("Edits"));
        }
        activateTab(m_editsTab);
        return;
    }
    if (index >= 0) {
        m_ui->mainTabWidget->removeTab(index);
    }
}

bool MainTabsPanel::isEditsTabVisible() const {
    return m_ui->mainTabWidget->indexOf(m_editsTab) >= 0 ||
           m_detachedTabs.contains(m_editsTab);
}

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

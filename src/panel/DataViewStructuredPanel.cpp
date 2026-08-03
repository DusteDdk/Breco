#include "panel/DataViewStructuredPanel.h"

#include <QDockWidget>
#include <QWidget>

namespace breco {

DataViewStructuredPanel::DataViewStructuredPanel(QWidget* parent)
    : QMainWindow(parent) {
    setWindowFlags(Qt::Widget);
    setObjectName(QStringLiteral("structuredDockWindow"));
    setDockNestingEnabled(true);
    m_structEditorHost = new QWidget(this);
    m_structEditorHost->setObjectName(QStringLiteral("structEditorHost"));
    m_structViewHost = new QWidget(this);
    m_structViewHost->setObjectName(QStringLiteral("structViewHost"));

    const auto dockFeatures = QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable;
    m_structEditorDock = new QDockWidget(QStringLiteral("Structure controls"),
                                         this);
    m_structEditorDock->setObjectName(QStringLiteral("structEditorDock"));
    m_structEditorDock->setFeatures(dockFeatures);
    m_structEditorDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_structEditorDock->setWidget(m_structEditorHost);

    m_structViewDock = new QDockWidget(QStringLiteral("Decoded structure"),
                                      this);
    m_structViewDock->setObjectName(QStringLiteral("structViewDock"));
    m_structViewDock->setFeatures(dockFeatures);
    m_structViewDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_structViewDock->setWidget(m_structViewHost);

    addDockWidget(Qt::LeftDockWidgetArea, m_structEditorDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_structViewDock);
    splitDockWidget(m_structEditorDock, m_structViewDock, Qt::Horizontal);
    resizeDocks({m_structEditorDock, m_structViewDock}, {45, 55},
                Qt::Horizontal);
}

DataViewStructuredPanel::~DataViewStructuredPanel() = default;

QWidget* DataViewStructuredPanel::structEditorHost() const {
    return m_structEditorHost;
}

QWidget* DataViewStructuredPanel::structViewHost() const { return m_structViewHost; }

}  // namespace breco

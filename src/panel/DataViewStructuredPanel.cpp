#include "panel/DataViewStructuredPanel.h"

#include "ui_DataViewStructured.h"

namespace breco {

DataViewStructuredPanel::DataViewStructuredPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::DataViewStructured>()) {
    m_ui->setupUi(this);
}

DataViewStructuredPanel::~DataViewStructuredPanel() = default;

QWidget* DataViewStructuredPanel::structEditorHost() const { return m_ui->structEditorHost; }

QWidget* DataViewStructuredPanel::structViewHost() const { return m_ui->structViewHost; }

QSplitter* DataViewStructuredPanel::splitter() const { return m_ui->structuredSplitter; }

}  // namespace breco

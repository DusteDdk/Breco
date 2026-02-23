#include "panel/ResultsTablePanel.h"

#include "ui_ResultsTablePanel.h"

namespace breco {

ResultsTablePanel::ResultsTablePanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::ResultsTablePanel>()) {
    m_ui->setupUi(this);
}

ResultsTablePanel::~ResultsTablePanel() = default;

QTableView* ResultsTablePanel::resultsTableView() const { return m_ui->resultsTableView; }

}  // namespace breco

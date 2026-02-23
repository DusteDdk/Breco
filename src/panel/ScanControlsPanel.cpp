#include "panel/ScanControlsPanel.h"

#include "ui_ScanControlsPanel.h"

namespace breco {

ScanControlsPanel::ScanControlsPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::ScanControlsPanel>()) {
    m_ui->setupUi(this);
}

ScanControlsPanel::~ScanControlsPanel() = default;

QLineEdit* ScanControlsPanel::searchTermLineEdit() const { return m_ui->searchTermLineEdit; }

QCheckBox* ScanControlsPanel::ignoreCaseCheckBox() const { return m_ui->ignoreCaseCheckBox; }

QCheckBox* ScanControlsPanel::prefillOnMergeCheckBox() const {
    return m_ui->prefillOnMergeCheckBox;
}

QSpinBox* ScanControlsPanel::shiftValueSpin() const { return m_ui->shiftValueSpin; }

QComboBox* ScanControlsPanel::shiftUnitCombo() const { return m_ui->shiftUnitCombo; }

QPushButton* ScanControlsPanel::startScanButton() const { return m_ui->startScanButton; }

QPushButton* ScanControlsPanel::openFileButton() const { return m_ui->openFileButton; }

QPushButton* ScanControlsPanel::openDirButton() const { return m_ui->openDirButton; }

QLabel* ScanControlsPanel::blockSizeLabel() const { return m_ui->blockSizeLabel; }

QSpinBox* ScanControlsPanel::blockSizeSpin() const { return m_ui->blockSizeSpin; }

QComboBox* ScanControlsPanel::blockSizeUnitCombo() const { return m_ui->blockSizeUnitCombo; }

QComboBox* ScanControlsPanel::workerCountCombo() const { return m_ui->workerCountCombo; }

QLabel* ScanControlsPanel::filesCountValueLabel() const { return m_ui->filesCountValueLabel; }

QLabel* ScanControlsPanel::searchSpaceValueLabel() const { return m_ui->searchSpaceValueLabel; }

QLabel* ScanControlsPanel::scannedValueLabel() const { return m_ui->scannedValueLabel; }

QProgressBar* ScanControlsPanel::scanProgressBar() const { return m_ui->scanProgressBar; }

QLabel* ScanControlsPanel::selectedSourceValueLabel() const {
    return m_ui->selectedSourceValueLabel;
}

}  // namespace breco

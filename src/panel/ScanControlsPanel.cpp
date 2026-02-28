#include "panel/ScanControlsPanel.h"

#include <QFont>
#include <QListWidget>
#include <QToolButton>

#include "ui_ScanControlsPanel.h"

namespace breco {

ScanControlsPanel::ScanControlsPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::ScanControlsPanel>()) {
    m_ui->setupUi(this);
    m_ui->lifecycleCard->setVisible(false);
    QFont sourceButtonFont = m_ui->openFileButton->font();
    sourceButtonFont.setFamilies(
        {QStringLiteral("Noto Sans"), QStringLiteral("Noto Sans Symbols 2"),
         QStringLiteral("DejaVu Sans"), QStringLiteral("Arial Unicode MS")});
    m_ui->openFileButton->setFont(sourceButtonFont);
    m_ui->openDirButton->setFont(sourceButtonFont);
}

ScanControlsPanel::~ScanControlsPanel() = default;

QLineEdit* ScanControlsPanel::searchTermLineEdit() const { return m_ui->searchTermLineEdit; }

QCheckBox* ScanControlsPanel::ignoreCaseCheckBox() const { return m_ui->ignoreCaseCheckBox; }

QCheckBox* ScanControlsPanel::prefillOnMergeCheckBox() const {
    return m_ui->prefillOnMergeCheckBox;
}

QSpinBox* ScanControlsPanel::shiftValueSpin() const {
    return findChild<QSpinBox*>(QStringLiteral("shiftValueSpin"));
}

QComboBox* ScanControlsPanel::shiftUnitCombo() const {
    return findChild<QComboBox*>(QStringLiteral("shiftUnitCombo"));
}

QPushButton* ScanControlsPanel::startScanButton() const { return m_ui->startScanButton; }

QToolButton* ScanControlsPanel::openFileButton() const { return m_ui->openFileButton; }

QToolButton* ScanControlsPanel::openDirButton() const { return m_ui->openDirButton; }

QLabel* ScanControlsPanel::blockSizeLabel() const { return m_ui->blockSizeLabel; }

QSpinBox* ScanControlsPanel::blockSizeSpin() const { return m_ui->blockSizeSpin; }

QComboBox* ScanControlsPanel::blockSizeUnitCombo() const { return m_ui->blockSizeUnitCombo; }

QComboBox* ScanControlsPanel::workerCountCombo() const { return m_ui->workerCountCombo; }

QLabel* ScanControlsPanel::filesCountValueLabel() const { return m_ui->filesCountValueLabel; }

QLabel* ScanControlsPanel::searchSpaceValueLabel() const { return m_ui->searchSpaceValueLabel; }

QLabel* ScanControlsPanel::scannedValueLabel() const {
    return findChild<QLabel*>(QStringLiteral("scannedValueLabel"));
}

QProgressBar* ScanControlsPanel::scanProgressBar() const { return m_ui->scanProgressBar; }

QLabel* ScanControlsPanel::selectedSourceValueLabel() const {
    return m_ui->selectedSourceValueLabel;
}

QWidget* ScanControlsPanel::advancedSearchGroup() const { return m_ui->advancedSearch; }

QWidget* ScanControlsPanel::lifecycleCard() const { return m_ui->lifecycleCard; }

QToolButton* ScanControlsPanel::hideLifecycleCardButton() const { return m_ui->btnHideLifecycleCard; }

QListWidget* ScanControlsPanel::lifecycleLogListWidget() const {
    return m_ui->lifecycleLogListWidget;
}

void ScanControlsPanel::showLifecycleCard() { m_ui->lifecycleCard->setVisible(true); }

void ScanControlsPanel::hideLifecycleCard() { m_ui->lifecycleCard->setVisible(false); }

void ScanControlsPanel::clearLifecycleLog() { m_ui->lifecycleLogListWidget->clear(); }

void ScanControlsPanel::appendLifecycleMessage(const QString& message) {
    m_ui->lifecycleLogListWidget->addItem(message);
    m_ui->lifecycleLogListWidget->scrollToBottom();
}

}  // namespace breco

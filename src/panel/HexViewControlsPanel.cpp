#include "panel/HexViewControlsPanel.h"

#include "ui_HexViewControlsPanel.h"

namespace breco {

HexViewControlsPanel::HexViewControlsPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::HexViewControlsPanel>()) {
    m_ui->setupUi(this);
}

HexViewControlsPanel::~HexViewControlsPanel() = default;

QLabel* HexViewControlsPanel::fileNameValueLabel() const { return m_ui->fileNameValueLabel; }

QLabel* HexViewControlsPanel::fileSizeValueLabel() const { return m_ui->fileSizeValueLabel; }

QLineEdit* HexViewControlsPanel::offsetValueEdit() const { return m_ui->offsetValueEdit; }

QLineEdit* HexViewControlsPanel::selectedValueEdit() const { return m_ui->selectedValueEdit; }

QLineEdit* HexViewControlsPanel::selectToValueEdit() const { return m_ui->selectToValueEdit; }

QCheckBox* HexViewControlsPanel::highlightResultCheckBox() const {
    return m_ui->highlightResultCheckBox;
}

QComboBox* HexViewControlsPanel::showAsComboBox() const { return m_ui->showAsComboBox; }

QComboBox* HexViewControlsPanel::newlineModeComboBox() const { return m_ui->newlineModeComboBox; }

QRadioButton* HexViewControlsPanel::littleEndianRadioButton() const {
    return m_ui->littleEndianRadioButton;
}

QRadioButton* HexViewControlsPanel::bigEndianRadioButton() const { return m_ui->bigEndianRadioButton; }

QComboBox* HexViewControlsPanel::bytesPerLineComboBox() const { return m_ui->bytesPerLineComboBox; }

QCheckBox* HexViewControlsPanel::stringsOnlyCheckBox() const { return m_ui->stringsOnlyCheckBox; }

QCheckBox* HexViewControlsPanel::monospaceCheckBox() const { return m_ui->monospaceCheckBox; }

QCheckBox* HexViewControlsPanel::wrapCheckBox() const { return m_ui->wrapCheckBox; }

QCheckBox* HexViewControlsPanel::breatheCheckBox() const { return m_ui->breatheCheckBox; }

QCheckBox* HexViewControlsPanel::collapseCheckBox() const { return m_ui->collapseCheckBox; }

QSpinBox* HexViewControlsPanel::shiftBitsSpinBox() const { return m_ui->shiftBitsSpinBox; }

}  // namespace breco

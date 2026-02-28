#include "panel/TextViewPanel.h"

#include "ui_TextViewPanel.h"

namespace breco {

TextViewPanel::TextViewPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::TextViewPanel>()) {
    m_ui->setupUi(this);
}

TextViewPanel::~TextViewPanel() = default;

QComboBox* TextViewPanel::textModeCombo() const { return m_ui->textModeCombo; }

QRadioButton* TextViewPanel::stringModeRadioButton() const { return m_ui->stringModeRadioButton; }

QRadioButton* TextViewPanel::byteModeRadioButton() const { return m_ui->byteModeRadioButton; }

QCheckBox* TextViewPanel::wrapModeCheckBox() const { return m_ui->wrapModeCheckBox; }

QCheckBox* TextViewPanel::collapseCheckBox() const { return m_ui->collapseCheckBox; }

QCheckBox* TextViewPanel::breatheCheckBox() const { return m_ui->breatheCheckBox; }

QComboBox* TextViewPanel::newlineModeComboBox() const { return m_ui->newlineModeComboBox; }

QCheckBox* TextViewPanel::monospaceCheckBox() const { return m_ui->monospaceCheckBox; }

QComboBox* TextViewPanel::bytesPerLineComboBox() const { return m_ui->bytesPerLineComboBox; }

QWidget* TextViewPanel::textViewContainer() const { return m_ui->textViewContainer; }

QHBoxLayout* TextViewPanel::textModeRowLayout() const { return m_ui->textModeRowLayout; }

QVBoxLayout* TextViewPanel::textViewPanelLayout() const { return m_ui->textViewPanelLayout; }

}  // namespace breco

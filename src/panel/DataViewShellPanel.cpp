#include "panel/DataViewShellPanel.h"

#include <QComboBox>
#include <QLabel>
#include <QRadioButton>
#include <QToolButton>

#include "ui_DataViewShell.h"

namespace breco {

DataViewShellPanel::DataViewShellPanel(ControlMode mode, QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::DataViewShell>()), m_mode(mode) {
    m_ui->setupUi(this);
    const bool rawMode = mode == ControlMode::Raw;
    m_ui->textInterpretationLabel->setVisible(rawMode);
    m_ui->textInterpretationComboBox->setVisible(rawMode);
    m_ui->bitmapModeLabel->setVisible(rawMode);
    m_ui->bitmapModeComboBox->setVisible(rawMode);
    m_ui->zoomOutButton->setVisible(rawMode);
    m_ui->zoomLabel->setVisible(rawMode);
    m_ui->zoomInButton->setVisible(rawMode);
}

DataViewShellPanel::~DataViewShellPanel() = default;

QRadioButton* DataViewShellPanel::littleEndianRadioButton() const {
    return m_ui->littleEndianRadioButton;
}

QRadioButton* DataViewShellPanel::bigEndianRadioButton() const { return m_ui->bigEndianRadioButton; }

QLabel* DataViewShellPanel::textInterpretationLabel() const {
    return m_ui->textInterpretationLabel;
}

QComboBox* DataViewShellPanel::textInterpretationComboBox() const {
    return m_ui->textInterpretationComboBox;
}

QLabel* DataViewShellPanel::bitmapModeLabel() const { return m_ui->bitmapModeLabel; }

QComboBox* DataViewShellPanel::bitmapModeComboBox() const { return m_ui->bitmapModeComboBox; }

QToolButton* DataViewShellPanel::zoomOutButton() const { return m_ui->zoomOutButton; }

QLabel* DataViewShellPanel::zoomLabel() const { return m_ui->zoomLabel; }

QToolButton* DataViewShellPanel::zoomInButton() const { return m_ui->zoomInButton; }

QWidget* DataViewShellPanel::bodyHost() const { return m_ui->bodyHost; }

DataViewShellPanel::ControlMode DataViewShellPanel::controlMode() const { return m_mode; }

}  // namespace breco

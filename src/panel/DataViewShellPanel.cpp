#include "panel/DataViewShellPanel.h"

#include <QComboBox>
#include <QLabel>
#include <QRadioButton>
#include <QToolButton>

#include "ui_DataViewShell.h"

namespace breco {

DataViewShellPanel::DataViewShellPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::DataViewShell>()) {
    m_ui->setupUi(this);
}

DataViewShellPanel::~DataViewShellPanel() = default;

QComboBox* DataViewShellPanel::modeComboBox() const { return m_ui->modeComboBox; }

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

QStackedWidget* DataViewShellPanel::bodyStackedWidget() const { return m_ui->bodyStackedWidget; }

void DataViewShellPanel::setRawControlsVisible(bool visible) {
    m_ui->textInterpretationLabel->setVisible(visible);
    m_ui->textInterpretationComboBox->setVisible(visible);
    m_ui->bitmapModeLabel->setVisible(visible);
    m_ui->bitmapModeComboBox->setVisible(visible);
    m_ui->zoomOutButton->setVisible(visible);
    m_ui->zoomLabel->setVisible(visible);
    m_ui->zoomInButton->setVisible(visible);
}

void DataViewShellPanel::setControlMode(ControlMode mode) {
    const bool rawMode = mode == ControlMode::Raw;
    const bool imageMode = mode == ControlMode::Image;
    m_ui->endianLabel->setVisible(!imageMode);
    m_ui->littleEndianRadioButton->setVisible(!imageMode);
    m_ui->bigEndianRadioButton->setVisible(!imageMode);
    setRawControlsVisible(rawMode);
}

}  // namespace breco

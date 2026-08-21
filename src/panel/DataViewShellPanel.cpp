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

}  // namespace breco

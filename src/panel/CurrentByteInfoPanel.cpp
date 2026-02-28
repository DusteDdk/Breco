#include "panel/CurrentByteInfoPanel.h"

#include <QCheckBox>
#include <QLabel>
#include <QRadioButton>

#include "ui_CurrentByteInfo.h"

namespace breco {

CurrentByteInfoPanel::CurrentByteInfoPanel(QWidget* parent)
    : QGroupBox(parent), m_ui(std::make_unique<Ui::gbCurrentByteInfo>()) {
    m_ui->setupUi(this);
    // Let splitter sizing drive this panel, not dynamic label content size hints.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setMinimumSize(0, 0);
    m_ui->lblByteInterpretationLarge->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

CurrentByteInfoPanel::~CurrentByteInfoPanel() = default;

QLabel* CurrentByteInfoPanel::byteInterpretationLargeLabel() const { return m_ui->lblByteInterpretationLarge; }

QLabel* CurrentByteInfoPanel::asciiValueLabel() const { return m_ui->lblAsciiValue; }

QLabel* CurrentByteInfoPanel::utf8ValueLabel() const { return m_ui->lblUtf8Value; }

QLabel* CurrentByteInfoPanel::utf16ValueLabel() const { return m_ui->lblUtf16Value; }

QLabel* CurrentByteInfoPanel::hexStr8BytesValueLabel() const { return m_ui->lblHexStr8bytes; }

QRadioButton* CurrentByteInfoPanel::decimalModeRadioButton() const { return m_ui->rbDec; }

QRadioButton* CurrentByteInfoPanel::hexModeRadioButton() const { return m_ui->rbHex; }

QRadioButton* CurrentByteInfoPanel::octalModeRadioButton() const { return m_ui->rbOct; }

QCheckBox* CurrentByteInfoPanel::bigEndianCheckBox() const { return m_ui->cbBigEndian; }

QLabel* CurrentByteInfoPanel::s8ValueLabel() const { return m_ui->lbls8; }

QLabel* CurrentByteInfoPanel::u8ValueLabel() const { return m_ui->lblu8; }

QLabel* CurrentByteInfoPanel::s16ValueLabel() const { return m_ui->lbls16; }

QLabel* CurrentByteInfoPanel::u16ValueLabel() const { return m_ui->lblu16; }

QLabel* CurrentByteInfoPanel::s32ValueLabel() const { return m_ui->lbls32; }

QLabel* CurrentByteInfoPanel::u32ValueLabel() const { return m_ui->lblu32; }

QLabel* CurrentByteInfoPanel::s64ValueLabel() const { return m_ui->lbls64le; }

QLabel* CurrentByteInfoPanel::u64ValueLabel() const { return m_ui->lblu64le; }

QLabel* CurrentByteInfoPanel::s8CaptionLabel() const { return m_ui->label_10; }

QLabel* CurrentByteInfoPanel::u8CaptionLabel() const { return m_ui->label_10; }

QLabel* CurrentByteInfoPanel::s16CaptionLabel() const { return m_ui->label_3; }

QLabel* CurrentByteInfoPanel::u16CaptionLabel() const { return m_ui->label_3; }

QLabel* CurrentByteInfoPanel::s32CaptionLabel() const { return m_ui->label_13; }

QLabel* CurrentByteInfoPanel::u32CaptionLabel() const { return m_ui->label_13; }

QLabel* CurrentByteInfoPanel::s64CaptionLabel() const { return m_ui->label_14; }

QLabel* CurrentByteInfoPanel::u64CaptionLabel() const { return m_ui->label_12; }

}  // namespace breco

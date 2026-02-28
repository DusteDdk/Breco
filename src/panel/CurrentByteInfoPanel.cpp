#include "panel/CurrentByteInfoPanel.h"

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

QRadioButton* CurrentByteInfoPanel::littleEndianCharModeRadioButton() const { return m_ui->rbLECharMode; }

QRadioButton* CurrentByteInfoPanel::bigEndianCharModeRadioButton() const { return m_ui->rbBECharMode; }

QLabel* CurrentByteInfoPanel::s8ValueLabel() const { return m_ui->lbls8; }

QLabel* CurrentByteInfoPanel::u8ValueLabel() const { return m_ui->lblu8; }

QLabel* CurrentByteInfoPanel::s16LeValueLabel() const { return m_ui->lbls16le; }

QLabel* CurrentByteInfoPanel::s16BeValueLabel() const { return m_ui->lbls16be; }

QLabel* CurrentByteInfoPanel::u16LeValueLabel() const { return m_ui->lblu16le; }

QLabel* CurrentByteInfoPanel::u16BeValueLabel() const { return m_ui->lblu16be; }

QLabel* CurrentByteInfoPanel::s32LeValueLabel() const { return m_ui->lbls32le; }

QLabel* CurrentByteInfoPanel::s32BeValueLabel() const { return m_ui->lbls32be; }

QLabel* CurrentByteInfoPanel::u32LeValueLabel() const { return m_ui->lblu32le; }

QLabel* CurrentByteInfoPanel::u32BeValueLabel() const { return m_ui->lblu32be; }

QLabel* CurrentByteInfoPanel::s64LeValueLabel() const { return m_ui->lbls64le; }

QLabel* CurrentByteInfoPanel::s64BeValueLabel() const { return m_ui->lbls64be; }

QLabel* CurrentByteInfoPanel::u64LeValueLabel() const { return m_ui->lblu64le; }

QLabel* CurrentByteInfoPanel::u64BeValueLabel() const { return m_ui->lblu64be; }

QLabel* CurrentByteInfoPanel::s8CaptionLabel() const { return m_ui->label; }

QLabel* CurrentByteInfoPanel::u8CaptionLabel() const { return m_ui->label_7; }

QLabel* CurrentByteInfoPanel::s16CaptionLabel() const { return m_ui->label_10; }

QLabel* CurrentByteInfoPanel::u16CaptionLabel() const { return m_ui->label_3; }

QLabel* CurrentByteInfoPanel::s32CaptionLabel() const { return m_ui->label_13; }

QLabel* CurrentByteInfoPanel::u32CaptionLabel() const { return m_ui->label_5; }

QLabel* CurrentByteInfoPanel::s64LeCaptionLabel() const { return m_ui->label_14; }

QLabel* CurrentByteInfoPanel::s64BeCaptionLabel() const { return m_ui->label_16; }

QLabel* CurrentByteInfoPanel::u64LeCaptionLabel() const { return m_ui->label_12; }

QLabel* CurrentByteInfoPanel::u64BeCaptionLabel() const { return m_ui->label_17; }

}  // namespace breco

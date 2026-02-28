#pragma once

#include <QGroupBox>

#include <memory>

QT_BEGIN_NAMESPACE
class QLabel;
class QRadioButton;
namespace Ui {
class gbCurrentByteInfo;
}
QT_END_NAMESPACE

namespace breco {

class CurrentByteInfoPanel : public QGroupBox {
    Q_OBJECT

public:
    explicit CurrentByteInfoPanel(QWidget* parent = nullptr);
    ~CurrentByteInfoPanel() override;

    QLabel* byteInterpretationLargeLabel() const;
    QLabel* asciiValueLabel() const;
    QLabel* utf8ValueLabel() const;
    QLabel* utf16ValueLabel() const;
    QRadioButton* littleEndianCharModeRadioButton() const;
    QRadioButton* bigEndianCharModeRadioButton() const;

    QLabel* s8ValueLabel() const;
    QLabel* u8ValueLabel() const;
    QLabel* s16LeValueLabel() const;
    QLabel* s16BeValueLabel() const;
    QLabel* u16LeValueLabel() const;
    QLabel* u16BeValueLabel() const;
    QLabel* s32LeValueLabel() const;
    QLabel* s32BeValueLabel() const;
    QLabel* u32LeValueLabel() const;
    QLabel* u32BeValueLabel() const;
    QLabel* s64LeValueLabel() const;
    QLabel* s64BeValueLabel() const;
    QLabel* u64LeValueLabel() const;
    QLabel* u64BeValueLabel() const;

    QLabel* s8CaptionLabel() const;
    QLabel* u8CaptionLabel() const;
    QLabel* s16CaptionLabel() const;
    QLabel* u16CaptionLabel() const;
    QLabel* s32CaptionLabel() const;
    QLabel* u32CaptionLabel() const;
    QLabel* s64LeCaptionLabel() const;
    QLabel* s64BeCaptionLabel() const;
    QLabel* u64LeCaptionLabel() const;
    QLabel* u64BeCaptionLabel() const;

private:
    std::unique_ptr<Ui::gbCurrentByteInfo> m_ui;
};

}  // namespace breco

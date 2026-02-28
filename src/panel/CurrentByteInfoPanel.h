#pragma once

#include <QGroupBox>

#include <memory>

QT_BEGIN_NAMESPACE
class QCheckBox;
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
    QLabel* hexStr8BytesValueLabel() const;
    QRadioButton* decimalModeRadioButton() const;
    QRadioButton* hexModeRadioButton() const;
    QRadioButton* octalModeRadioButton() const;
    QCheckBox* bigEndianCheckBox() const;

    QLabel* s8ValueLabel() const;
    QLabel* u8ValueLabel() const;
    QLabel* s16ValueLabel() const;
    QLabel* u16ValueLabel() const;
    QLabel* s32ValueLabel() const;
    QLabel* u32ValueLabel() const;
    QLabel* s64ValueLabel() const;
    QLabel* u64ValueLabel() const;

    QLabel* s8CaptionLabel() const;
    QLabel* u8CaptionLabel() const;
    QLabel* s16CaptionLabel() const;
    QLabel* u16CaptionLabel() const;
    QLabel* s32CaptionLabel() const;
    QLabel* u32CaptionLabel() const;
    QLabel* s64CaptionLabel() const;
    QLabel* u64CaptionLabel() const;

private:
    std::unique_ptr<Ui::gbCurrentByteInfo> m_ui;
};

}  // namespace breco

#pragma once

#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
namespace Ui {
class HexViewControlsPanel;
}
QT_END_NAMESPACE

namespace breco {

class HexViewControlsPanel : public QWidget {
    Q_OBJECT

public:
    explicit HexViewControlsPanel(QWidget* parent = nullptr);
    ~HexViewControlsPanel() override;

    QLabel* fileNameValueLabel() const;
    QLabel* fileSizeValueLabel() const;
    QLineEdit* offsetValueEdit() const;
    QLineEdit* selectedValueEdit() const;
    QLineEdit* selectToValueEdit() const;
    QCheckBox* highlightResultCheckBox() const;
    QComboBox* showAsComboBox() const;
    QComboBox* newlineModeComboBox() const;
    QRadioButton* littleEndianRadioButton() const;
    QRadioButton* bigEndianRadioButton() const;
    QComboBox* bytesPerLineComboBox() const;
    QCheckBox* stringsOnlyCheckBox() const;
    QCheckBox* monospaceCheckBox() const;
    QCheckBox* wrapCheckBox() const;
    QCheckBox* breatheCheckBox() const;
    QCheckBox* collapseCheckBox() const;
    QCheckBox* allowEditingCheckBox() const;
    QSpinBox* shiftBitsSpinBox() const;

private:
    std::unique_ptr<Ui::HexViewControlsPanel> m_ui;
};

}  // namespace breco

#pragma once

#include <QString>
#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QListWidget;
class QSpinBox;
class QToolButton;
namespace Ui {
class ScanControlsPanel;
}
QT_END_NAMESPACE

namespace breco {

class ScanControlsPanel : public QWidget {
    Q_OBJECT

public:
    explicit ScanControlsPanel(QWidget* parent = nullptr);
    ~ScanControlsPanel() override;

    QLineEdit* searchTermLineEdit() const;
    QCheckBox* ignoreCaseCheckBox() const;
    QCheckBox* prefillOnMergeCheckBox() const;
    QSpinBox* shiftValueSpin() const;
    QComboBox* shiftUnitCombo() const;
    QPushButton* startScanButton() const;
    QToolButton* openFileButton() const;
    QToolButton* openDirButton() const;
    QLabel* blockSizeLabel() const;
    QSpinBox* blockSizeSpin() const;
    QComboBox* blockSizeUnitCombo() const;
    QComboBox* workerCountCombo() const;
    QLabel* filesCountValueLabel() const;
    QLabel* searchSpaceValueLabel() const;
    QLabel* scannedValueLabel() const;
    QProgressBar* scanProgressBar() const;
    QLabel* selectedSourceValueLabel() const;
    QWidget* advancedSearchGroup() const;
    QWidget* lifecycleCard() const;
    QToolButton* hideLifecycleCardButton() const;
    QListWidget* lifecycleLogListWidget() const;
    void showLifecycleCard();
    void hideLifecycleCard();
    void clearLifecycleLog();
    void appendLifecycleMessage(const QString& message);

private:
    std::unique_ptr<Ui::ScanControlsPanel> m_ui;
};

}  // namespace breco

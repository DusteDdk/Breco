#pragma once

#include <QString>
#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QCompleter;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QListWidget;
class QSpinBox;
class QStringListModel;
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
    QLabel* selectedSourceTypeIconLabel() const;
    QLineEdit* sourcePathLineEdit() const;
    QWidget* advancedSearchGroup() const;
    QWidget* lifecycleCard() const;
    QToolButton* hideLifecycleCardButton() const;
    QListWidget* lifecycleLogListWidget() const;
    void showLifecycleCard();
    void hideLifecycleCard();
    void clearLifecycleLog();
    void appendLifecycleMessage(const QString& message);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateSourcePathSuggestions(const QString& text);

    std::unique_ptr<Ui::ScanControlsPanel> m_ui;
    QCompleter* m_sourcePathCompleter = nullptr;
    QStringListModel* m_sourcePathCompletionModel = nullptr;
};

}  // namespace breco

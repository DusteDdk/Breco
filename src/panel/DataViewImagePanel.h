#pragma once

#include <QWidget>

#include "image/EmbeddedImageScanner.h"
#include "scan/ScanProgress.h"

#include <memory>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
namespace Ui {
class DataViewImage;
}
QT_END_NAMESPACE

namespace breco {

class DataViewImagePanel : public QWidget {
    Q_OBJECT

public:
    explicit DataViewImagePanel(QWidget* parent = nullptr);
    ~DataViewImagePanel() override;

    QCheckBox* formatCheckBox(EmbeddedImageFormat format) const;
    QComboBox* scopeComboBox() const;
    QSpinBox* maxPixelsKSpinBox() const;
    QSpinBox* maxResultsSpinBox() const;
    QSpinBox* jobsSpinBox() const;
    QProgressBar* fileProgressBar() const;
    QProgressBar* resultsProgressBar() const;
    QPushButton* scanButton() const;
    QLabel* statusLabel() const;

    EmbeddedImageFormats selectedFormats() const;
    void setSelectedFormats(EmbeddedImageFormats formats);
    EmbeddedImageScope selectedScope() const;
    void setSelectedScope(EmbeddedImageScope scope);
    EmbeddedImageScanOptions scanOptions() const;
    void setSupportedFormats(EmbeddedImageFormats formats);

    void setScanRunning(bool running);
    void resetProgress();
    void updateProgress(const ScanProgressSnapshot& progress, int resultsFound,
                        int resultsLimit);
    void setStatusText(const QString& text);
    void clearResults();
    void addResult(const EmbeddedImageResult& result);
    int resultCount() const;

signals:
    void scanRequested();
    void resultActivated(quint64 offset);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct ResultItem {
        EmbeddedImageResult result;
        QLabel* imageLabel = nullptr;
        QTimer* animationTimer = nullptr;
        int currentFrame = 0;
    };

    void updateResultPixmaps();
    void updateResultPixmap(ResultItem& item, const QSize& box);
    void updateControlsEnabled();
    void updateResultsProgressVisibility(int resultsLimit);
    QSize previewBoxSize() const;

    std::unique_ptr<Ui::DataViewImage> m_ui;
    QVector<ResultItem> m_results;
    EmbeddedImageFormats m_supportedFormats = allEmbeddedImageFormats();
    bool m_scanRunning = false;
};

}  // namespace breco

#include "panel/DataViewImagePanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPalette>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include "ui_DataViewImage.h"

#include <array>

namespace breco {

namespace {

class ClickableImageLabel final : public QLabel {
public:
    using QLabel::QLabel;

    std::function<void()> clicked;
    std::function<void()> saveRequested;
    std::function<void(bool)> hoverChanged;

protected:
    void enterEvent(QEnterEvent* event) override {
        QLabel::enterEvent(event);
        if (hoverChanged) {
            hoverChanged(true);
        }
    }

    void leaveEvent(QEvent* event) override {
        QLabel::leaveEvent(event);
        if (hoverChanged) {
            hoverChanged(false);
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        QLabel::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton && rect().contains(event->pos()) && clicked) {
            clicked();
        } else if (event->button() == Qt::RightButton && rect().contains(event->pos()) &&
                   saveRequested) {
            saveRequested();
        }
    }
};

class HighlightImageFrame final : public QFrame {
public:
    explicit HighlightImageFrame(QWidget* parent = nullptr)
        : QFrame(parent), m_normalPalette(palette()) {}

    void setHighlighted(bool highlighted) {
        if (highlighted) {
            QPalette highlightedPalette = palette();
            highlightedPalette.setColor(
                QPalette::Window,
                highlightedPalette.color(QPalette::Highlight).lighter(175));
            setPalette(highlightedPalette);
            setAutoFillBackground(true);
        } else {
            setPalette(m_normalPalette);
            setAutoFillBackground(false);
        }
    }

private:
    QPalette m_normalPalette;
};

QString offsetText(quint64 offset) {
    return QStringLiteral("0x%1").arg(offset, 0, 16).toUpper();
}

void saveEncodedImage(QWidget* parent, const EmbeddedImageResult& result, int imageNumber) {
    if (result.encodedData.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("Save File"),
                             QStringLiteral("The original encoded image data is unavailable."));
        return;
    }
    const QString extension = embeddedImageFileExtension(result.format);
    const QString suggestedPath = QDir::home().filePath(
        QStringLiteral("image-%1.%2").arg(imageNumber).arg(extension));
    QString path = QFileDialog::getSaveFileName(
        parent, QStringLiteral("Save File"), suggestedPath,
        QStringLiteral("%1 image (*.%2);;All files (*)")
            .arg(result.formatName)
            .arg(extension));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".") + extension;
    }
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(result.encodedData) != result.encodedData.size() || !output.commit()) {
        QMessageBox::warning(parent, QStringLiteral("Save File"),
                             QStringLiteral("Could not save the image to %1.").arg(path));
    }
}

QString progressBytesText(quint64 bytes) {
    static constexpr std::array<const char*, 4> kUnits = {"KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes) / 1024.0;
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < static_cast<int>(kUnits.size()) - 1) {
        value /= 1024.0;
        ++unitIndex;
    }
    return QStringLiteral("%1 %2")
        .arg(value, 0, 'f', 2)
        .arg(QString::fromLatin1(kUnits.at(unitIndex)));
}

}  // namespace

DataViewImagePanel::DataViewImagePanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::DataViewImage>()) {
    m_ui->setupUi(this);
    const int threadCount = qMax(1, QThread::idealThreadCount());
    m_ui->jobsSpinBox->setRange(1, qMin(256, threadCount));
    m_ui->jobsSpinBox->setValue(threadCount);
    updateResultsProgressVisibility(m_ui->maxResultsSpinBox->value());
    resetProgress();
    m_ui->resultsScrollArea->viewport()->installEventFilter(this);
    connect(m_ui->scanButton, &QPushButton::clicked, this, &DataViewImagePanel::scanRequested);
    connect(m_ui->maxResultsSpinBox, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int value) { updateResultsProgressVisibility(value); });
}

DataViewImagePanel::~DataViewImagePanel() = default;

QCheckBox* DataViewImagePanel::formatCheckBox(EmbeddedImageFormat format) const {
    switch (format) {
        case EmbeddedImageFormat::Tga:
            return m_ui->tgaCheckBox;
        case EmbeddedImageFormat::Tiff:
            return m_ui->tiffCheckBox;
        case EmbeddedImageFormat::Png:
            return m_ui->pngCheckBox;
        case EmbeddedImageFormat::Jpeg:
            return m_ui->jpegCheckBox;
        case EmbeddedImageFormat::Bmp:
            return m_ui->bmpCheckBox;
        case EmbeddedImageFormat::Ico:
            return m_ui->icoCheckBox;
        case EmbeddedImageFormat::Gif:
            return m_ui->gifCheckBox;
        case EmbeddedImageFormat::Xbm:
            return m_ui->xbmCheckBox;
        case EmbeddedImageFormat::Xpm:
            return m_ui->xpmCheckBox;
        case EmbeddedImageFormat::Svg:
            return m_ui->svgCheckBox;
    }
    return nullptr;
}

QComboBox* DataViewImagePanel::scopeComboBox() const { return m_ui->scopeComboBox; }

QSpinBox* DataViewImagePanel::maxPixelsKSpinBox() const { return m_ui->maxPixelsKSpinBox; }

QSpinBox* DataViewImagePanel::maxResultsSpinBox() const { return m_ui->maxResultsSpinBox; }

QSpinBox* DataViewImagePanel::jobsSpinBox() const { return m_ui->jobsSpinBox; }

QProgressBar* DataViewImagePanel::fileProgressBar() const { return m_ui->fileProgressBar; }

QProgressBar* DataViewImagePanel::resultsProgressBar() const { return m_ui->resultsProgressBar; }

QPushButton* DataViewImagePanel::scanButton() const { return m_ui->scanButton; }

QLabel* DataViewImagePanel::statusLabel() const { return m_ui->statusLabel; }

EmbeddedImageFormats DataViewImagePanel::selectedFormats() const {
    EmbeddedImageFormats formats;
    for (const EmbeddedImageFormat format :
         {EmbeddedImageFormat::Tga, EmbeddedImageFormat::Tiff, EmbeddedImageFormat::Png,
          EmbeddedImageFormat::Jpeg, EmbeddedImageFormat::Bmp, EmbeddedImageFormat::Ico,
          EmbeddedImageFormat::Gif, EmbeddedImageFormat::Xbm, EmbeddedImageFormat::Xpm,
          EmbeddedImageFormat::Svg}) {
        const QCheckBox* checkBox = formatCheckBox(format);
        if (checkBox != nullptr && checkBox->isChecked() && m_supportedFormats.testFlag(format)) {
            formats |= EmbeddedImageFormats(format);
        }
    }
    return formats;
}

void DataViewImagePanel::setSelectedFormats(EmbeddedImageFormats formats) {
    for (const EmbeddedImageFormat format :
         {EmbeddedImageFormat::Tga, EmbeddedImageFormat::Tiff, EmbeddedImageFormat::Png,
          EmbeddedImageFormat::Jpeg, EmbeddedImageFormat::Bmp, EmbeddedImageFormat::Ico,
          EmbeddedImageFormat::Gif, EmbeddedImageFormat::Xbm, EmbeddedImageFormat::Xpm,
          EmbeddedImageFormat::Svg}) {
        if (QCheckBox* checkBox = formatCheckBox(format); checkBox != nullptr) {
            checkBox->setChecked(formats.testFlag(format));
        }
    }
}

EmbeddedImageScope DataViewImagePanel::selectedScope() const {
    return static_cast<EmbeddedImageScope>(qBound(0, m_ui->scopeComboBox->currentIndex(), 2));
}

void DataViewImagePanel::setSelectedScope(EmbeddedImageScope scope) {
    m_ui->scopeComboBox->setCurrentIndex(qBound(0, static_cast<int>(scope), 2));
}

EmbeddedImageScanOptions DataViewImagePanel::scanOptions() const {
    EmbeddedImageScanOptions options;
    options.formats = selectedFormats();
    options.maxPixelsK = static_cast<quint32>(qMax(1, m_ui->maxPixelsKSpinBox->value()));
    options.maxResults = qMax(0, m_ui->maxResultsSpinBox->value());
    options.workerCount = qMax(1, m_ui->jobsSpinBox->value());
    return options;
}

void DataViewImagePanel::setSupportedFormats(EmbeddedImageFormats formats) {
    m_supportedFormats = formats;
    updateControlsEnabled();
}

void DataViewImagePanel::setScanRunning(bool running) {
    m_scanRunning = running;
    m_ui->scanButton->setEnabled(true);
    m_ui->scanButton->setText(running ? QStringLiteral("Stop") : QStringLiteral("Scan"));
    updateControlsEnabled();
}

void DataViewImagePanel::resetProgress() {
    m_ui->fileProgressBar->setMaximum(1000);
    m_ui->fileProgressBar->setValue(0);
    m_ui->fileProgressBar->setTextVisible(true);
    m_ui->fileProgressBar->setFormat(
        QStringLiteral("%1 / %2").arg(progressBytesText(0)).arg(progressBytesText(0)));
    m_fileProgressTextTimer.invalidate();
    m_ui->resultsProgressBar->setMaximum(1000);
    m_ui->resultsProgressBar->setValue(0);
    m_ui->resultsProgressBar->setFormat(QStringLiteral("0 / %1").arg(m_ui->maxResultsSpinBox->value()));
    updateResultsProgressVisibility(m_ui->maxResultsSpinBox->value());
}

void DataViewImagePanel::updateProgress(quint64 bytesScanned, quint64 bytesTotal,
                                        int resultsFound, int resultsLimit) {
    const int fileValue =
        bytesTotal == 0
            ? 0
            : static_cast<int>(qMin<quint64>(1000ULL, (bytesScanned * 1000ULL) / bytesTotal));
    m_ui->fileProgressBar->setValue(fileValue);
    const bool forceTextUpdate = bytesScanned == 0 || bytesScanned >= bytesTotal;
    if (forceTextUpdate || !m_fileProgressTextTimer.isValid() ||
        m_fileProgressTextTimer.elapsed() >= 1000) {
        m_ui->fileProgressBar->setFormat(QStringLiteral("%1 / %2")
                                             .arg(progressBytesText(bytesScanned))
                                             .arg(progressBytesText(bytesTotal)));
        m_fileProgressTextTimer.restart();
    }
    if (resultsLimit > 0) {
        m_ui->resultsProgressBar->setVisible(true);
        if (m_ui->resultsProgressLabel != nullptr) {
            m_ui->resultsProgressLabel->setVisible(true);
        }
        m_ui->resultsProgressBar->setValue(
            qBound(0, static_cast<int>((static_cast<qint64>(resultsFound) * 1000) / resultsLimit),
                   1000));
        m_ui->resultsProgressBar->setFormat(
            QStringLiteral("%1 / %2").arg(resultsFound).arg(resultsLimit));
    } else {
        updateResultsProgressVisibility(0);
    }
}

void DataViewImagePanel::updateControlsEnabled() {
    for (const EmbeddedImageFormat format :
         {EmbeddedImageFormat::Tga, EmbeddedImageFormat::Tiff, EmbeddedImageFormat::Png,
          EmbeddedImageFormat::Jpeg, EmbeddedImageFormat::Bmp, EmbeddedImageFormat::Ico,
          EmbeddedImageFormat::Gif, EmbeddedImageFormat::Xbm, EmbeddedImageFormat::Xpm,
          EmbeddedImageFormat::Svg}) {
        QCheckBox* checkBox = formatCheckBox(format);
        if (checkBox == nullptr) {
            continue;
        }
        const bool supported = m_supportedFormats.testFlag(format);
        checkBox->setEnabled(supported && !m_scanRunning);
        checkBox->setToolTip(supported ? QString()
                                       : QStringLiteral("No Qt image reader is available for %1.")
                                             .arg(embeddedImageFormatName(format)));
    }
    m_ui->scopeComboBox->setEnabled(!m_scanRunning);
    m_ui->jobsSpinBox->setEnabled(!m_scanRunning);
    m_ui->maxPixelsKSpinBox->setEnabled(!m_scanRunning);
    m_ui->maxResultsSpinBox->setEnabled(!m_scanRunning);
}

void DataViewImagePanel::updateResultsProgressVisibility(int resultsLimit) {
    const bool visible = resultsLimit > 0;
    m_ui->resultsProgressLabel->setVisible(visible);
    m_ui->resultsProgressBar->setVisible(visible);
    if (visible) {
        m_ui->resultsProgressBar->setFormat(QStringLiteral("%1 / %2")
                                                .arg(resultCount())
                                                .arg(resultsLimit));
    }
}

void DataViewImagePanel::setStatusText(const QString& text) {
    m_ui->statusLabel->setText(text);
}

void DataViewImagePanel::clearResults() {
    while (m_ui->resultsLayout->count() > 1) {
        QLayoutItem* item = m_ui->resultsLayout->takeAt(0);
        if (item == nullptr) {
            continue;
        }
        delete item->widget();
        delete item;
    }
    m_results.clear();
}

void DataViewImagePanel::addResult(const EmbeddedImageResult& result) {
    const int imageNumber = m_results.size() + 1;
    auto* card = new HighlightImageFrame(m_ui->resultsContainer);
    card->setObjectName(QStringLiteral("imageResultCard"));
    card->setFrameShape(QFrame::StyledPanel);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    const QString frameText =
        result.format == EmbeddedImageFormat::Gif
            ? QStringLiteral("  %1 frame%2")
                  .arg(result.frameCount())
                  .arg(result.frameCount() == 1 ? QString() : QStringLiteral("s"))
            : QString();
    auto* title =
        new QLabel(QStringLiteral("Image: %1  %2  %3x%4%5  offset %6")
                       .arg(imageNumber)
                       .arg(result.formatName)
                       .arg(result.size.width())
                       .arg(result.size.height())
                       .arg(frameText)
                       .arg(offsetText(result.offset)),
                   card);
    title->setObjectName(QStringLiteral("imageResultTitle"));
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);

    auto* imageLabel = new ClickableImageLabel(card);
    imageLabel->setObjectName(QStringLiteral("imagePreviewLabel"));
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setCursor(Qt::PointingHandCursor);
    imageLabel->setMinimumHeight(80);
    imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    imageLabel->setToolTip(
        QStringLiteral("Click to jump to %1. Right-click to save the original image.")
            .arg(offsetText(result.offset)));
    imageLabel->clicked = [this, offset = result.offset]() { emit resultActivated(offset); };
    imageLabel->saveRequested =
        [card, result, imageNumber]() { saveEncodedImage(card, result, imageNumber); };
    imageLabel->hoverChanged = [card](bool highlighted) {
        card->setHighlighted(highlighted);
    };
    layout->addWidget(imageLabel);

    m_ui->resultsLayout->insertWidget(qMax(0, m_ui->resultsLayout->count() - 1), card);
    m_results.push_back({result, imageLabel});
    ResultItem& stored = m_results.last();
    if (result.animationFrames.size() > 1) {
        stored.animationTimer = new QTimer(card);
        stored.animationTimer->setObjectName(QStringLiteral("imageAnimationTimer"));
        stored.animationTimer->setTimerType(Qt::PreciseTimer);
        connect(stored.animationTimer, &QTimer::timeout, this, [this, imageLabel]() {
            for (ResultItem& item : m_results) {
                if (item.imageLabel != imageLabel || item.result.animationFrames.isEmpty()) {
                    continue;
                }
                item.currentFrame =
                    (item.currentFrame + 1) % item.result.animationFrames.size();
                updateResultPixmap(item, previewBoxSize());
                const int delay = item.currentFrame < item.result.frameDelaysMs.size()
                                      ? item.result.frameDelaysMs.at(item.currentFrame)
                                      : 100;
                item.animationTimer->setInterval(qMax(16, delay));
                break;
            }
        });
        const int firstDelay =
            result.frameDelaysMs.isEmpty() ? 100 : result.frameDelaysMs.first();
        stored.animationTimer->start(qMax(16, firstDelay));
    }
    updateResultPixmaps();
}

int DataViewImagePanel::resultCount() const {
    return m_results.size();
}

bool DataViewImagePanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_ui->resultsScrollArea->viewport() && event->type() == QEvent::Resize) {
        updateResultPixmaps();
    }
    return QWidget::eventFilter(watched, event);
}

void DataViewImagePanel::updateResultPixmaps() {
    const QSize box = previewBoxSize();
    for (ResultItem& item : m_results) {
        updateResultPixmap(item, box);
    }
}

void DataViewImagePanel::updateResultPixmap(ResultItem& item, const QSize& box) {
    if (item.imageLabel == nullptr || item.result.image.isNull()) {
        return;
    }
    const QImage& image =
        item.result.animationFrames.isEmpty()
            ? item.result.image
            : item.result.animationFrames.at(
                  qBound(0, item.currentFrame, item.result.animationFrames.size() - 1));
    QSize target = image.size();
    if (target.width() > box.width() || target.height() > box.height()) {
        target.scale(box, Qt::KeepAspectRatio);
    }
    const QImage scaled =
        image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    item.imageLabel->setPixmap(QPixmap::fromImage(scaled));
    item.imageLabel->setMinimumHeight(qMax(80, scaled.height() + 8));
}

QSize DataViewImagePanel::previewBoxSize() const {
    const int width = qMax(80, m_ui->resultsScrollArea->viewport()->width() - 32);
    const int height = qMax(80, qMin(420, m_ui->resultsScrollArea->viewport()->height() - 48));
    return QSize(width, height);
}

}  // namespace breco

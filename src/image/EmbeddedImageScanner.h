#pragma once

#include <QFlags>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include "model/ResultTypes.h"
#include "scan/ScanProgress.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace breco {

enum class EmbeddedImageFormat : quint32 {
    Tga = 0x001,
    Tiff = 0x002,
    Png = 0x004,
    Jpeg = 0x008,
    Bmp = 0x010,
    Ico = 0x020,
    Gif = 0x040,
    Xbm = 0x080,
    Xpm = 0x100,
    Svg = 0x200,
};
Q_DECLARE_FLAGS(EmbeddedImageFormats, EmbeddedImageFormat)

enum class EmbeddedImageScope {
    FromStart = 0,
    FromHere,
    VisibleBuffer,
};

struct EmbeddedImageScanSource {
    QString filePath;
    quint64 fileSize = 0;
    std::function<std::optional<QByteArray>(quint64 offset, quint64 size)> read;
};

struct EmbeddedImageScanOptions {
    EmbeddedImageFormats formats;
    quint64 startOffset = 0;
    quint64 endOffsetExclusive = 0;
    quint32 maxPixelsK = 4096;
    int maxResults = 5;
    int workerCount = 1;
    quint64 chunkSize = 1024ULL * 1024ULL;
    quint64 maxDecodeBytes = 64ULL * 1024ULL * 1024ULL;
};

struct EmbeddedImageResult {
    quint64 offset = 0;
    EmbeddedImageFormat format = EmbeddedImageFormat::Png;
    QString formatName;
    QSize size;
    QImage image;
    QByteArray encodedData;
    QVector<QImage> animationFrames;
    QVector<int> frameDelaysMs;

    int frameCount() const { return animationFrames.isEmpty() ? (image.isNull() ? 0 : 1)
                                                              : animationFrames.size(); }
};

struct EmbeddedImageScanSummary {
    quint64 bytesScanned = 0;
    quint64 rawBytesRead = 0;
    bool cancelled = false;
    QString message;
};

struct EmbeddedImageScanRequest {
    EmbeddedImageScanSource source;
    EmbeddedImageScanOptions options;
};

EmbeddedImageFormats allEmbeddedImageFormats();
QString embeddedImageFormatName(EmbeddedImageFormat format);
QString embeddedImageFileExtension(EmbeddedImageFormat format);
QByteArray embeddedImageQtFormatName(EmbeddedImageFormat format);
bool embeddedImageFormatHasReader(EmbeddedImageFormat format);
EmbeddedImageFormats supportedEmbeddedImageFormats();

using EmbeddedImageCancelCallback = std::function<bool()>;
using EmbeddedImageProgressCallback =
    std::function<void(quint64 bytesScanned, quint64 bytesTotal, quint64 rawBytesRead,
                       int resultsFound, int resultsLimit)>;
using EmbeddedImageResultCallback = std::function<void(const EmbeddedImageResult& result)>;

QVector<EmbeddedImageResult> scanEmbeddedImages(
    const EmbeddedImageScanSource& source, EmbeddedImageScanOptions options,
    EmbeddedImageScanSummary* summary = nullptr,
    EmbeddedImageCancelCallback shouldCancel = {},
    EmbeddedImageProgressCallback progressCallback = {},
    EmbeddedImageResultCallback resultCallback = {});

class EmbeddedImageScanController : public QObject {
    Q_OBJECT

public:
    explicit EmbeddedImageScanController(QObject* parent = nullptr);
    ~EmbeddedImageScanController() override;

    quint64 startScan(const EmbeddedImageScanRequest& request);
    void requestStop();
    bool isRunning() const;

signals:
    void scanStarted(quint64 scanId);
    void progressUpdated(quint64 scanId, const breco::ScanProgressSnapshot& progress,
                         int resultsFound, int resultsLimit);
    void resultReady(quint64 scanId, const breco::EmbeddedImageResult& result);
    void scanFinished(quint64 scanId, const breco::EmbeddedImageScanSummary& summary,
                      const QVector<breco::EmbeddedImageResult>& results);

private:
    void joinFinishedWorker();

    std::jthread m_worker;
    std::atomic_bool m_running{false};
    std::atomic<quint64> m_nextScanId{1};
};

}  // namespace breco

Q_DECLARE_OPERATORS_FOR_FLAGS(breco::EmbeddedImageFormats)
Q_DECLARE_METATYPE(breco::EmbeddedImageResult)
Q_DECLARE_METATYPE(breco::EmbeddedImageScanSummary)

#pragma once

#include <QMainWindow>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <memory>
#include <optional>

#include "io/OpenFilePool.h"
#include "io/ShiftedWindowLoader.h"
#include "model/ResultModel.h"
#include "scan/ScanController.h"

QT_BEGIN_NAMESPACE
class QComboBox;
class QSpinBox;
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

namespace breco {

class BitmapViewWidget;
class BitmapViewPanel;
class CurrentByteInfoPanel;
class ResultsTablePanel;
class ScanControlsPanel;
class TextViewWidget;
class TextViewPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    bool selectSourcePath(const QString& path);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onOpenFile();
    void onOpenDirectory();
    void onStartScan();
    void onStopScan();
    void onResultActivated(const QModelIndex& index);
    void onResultsBatchReady(const QVector<MatchRecord>& matches, int mergedTotal);
    void onProgressUpdated(quint64 scanned, quint64 total);
    void onScanStarted(int fileCount, quint64 totalBytes);
    void onScanFinished(bool stoppedByUser, bool autoStoppedLimitExceeded);
    void onTextModeChanged(int idx);
    void onBitmapModeChanged(int idx);
    void onTextBackingScrollRequested(int wheelSteps, int bytesPerStepHint, int visibleBytesHint);
    void onTextHoverOffsetChanged(quint64 absoluteOffset);
    void onTextCenterAnchorRequested(quint64 absoluteOffset);
    void onBitmapHoverOffsetChanged(quint64 absoluteOffset);
    void onBitmapByteClicked(quint64 absoluteOffset);
    void onHoverLeft();

private:
    enum class SourceMode { None, SingleFile, Directory };
    enum class HoverSource { None, Text, Bitmap };

    struct HoverBuffer {
        QString filePath;
        quint64 baseOffset = 0;
        QByteArray data;
    };

    struct ViewportWindow {
        quint64 start = 0;
        QByteArray data;
    };

    struct ByteSpan {
        quint64 start = 0;
        quint64 size = 0;
    };

    quint64 effectiveBlockSizeBytes() const;
    ShiftSettings currentShiftSettings() const;
    TextInterpretationMode selectedTextMode() const;
    void setScanButtonMode(bool running);
    void updateBlockSizeLabel();
    int selectedWorkerCount() const;
    QString humanBytes(quint64 bytes) const;
    bool selectSingleFileSource(const QString& filePath);
    bool selectDirectorySource(const QString& dirPath);
    void refreshSourceSummary();
    void buildScanTargets(const QVector<QString>& filePaths);
    quint64 currentSelectedSourceBytes() const;
    void selectResultRow(int row);
    void requestSharedCenter(quint64 absoluteOffset);
    void shiftSharedCenterBy(qint64 signedBytes);
    void scheduleSharedPreviewUpdate();
    void updateSharedPreviewNow();
    ByteSpan centeredSpan(const ResultBuffer& buffer, quint64 centerOffset,
                          quint64 desiredWindowBytes) const;
    quint64 textViewportByteWindow() const;
    quint64 bitmapViewportByteWindow() const;

    QString filePathForTarget(int targetIdx) const;
    QVector<int> bufferReferenceCounts() const;
    quint64 totalResidentBufferBytes(const QVector<int>& refCounts) const;
    bool evictOneBufferLargestFirstLeastUsed(const QSet<int>& protectedBufferIndices = {});
    int enforceBufferCacheBudget(const QSet<int>& protectedBufferIndices = {});
    bool ensureRowBufferLoaded(int row, const MatchRecord& match,
                               const QSet<int>& protectedBufferIndices = {});
    ResultBuffer makeEvictedPlaceholderBuffer(const MatchRecord& match) const;
    ResultBuffer loadEvictedWindowForMatch(const MatchRecord& match) const;
    bool restoreBufferRawIfDirty(int bufferIndex);
    void restoreDirtyBufferForRow(int row);
    void applyShiftToBufferIfEnabled(int bufferIndex);
    bool expandActivePreviewBuffer(int direction);
    void clearResultBufferCacheState();
    void rebuildTargetMatchIntervals();
    std::optional<unsigned char> previousByteBeforeViewport(const ResultBuffer& buffer,
                                                            quint64 viewportStart) const;
    quint64 clampViewportStart(const ResultBuffer& buffer, quint64 desiredStart,
                               quint64 windowBytes) const;
    ViewportWindow viewportFromStart(const ResultBuffer& buffer, quint64 startOffset,
                                     quint64 windowBytes) const;
    void showMatchPreview(int row, const MatchRecord& match);
    void loadNotEmptyPreview();

    void refreshCurrentByteInfoFromLastHover();
    void updateCurrentByteInfoFromHover(const HoverBuffer& buffer, quint64 absoluteOffset);
    void setCurrentByteCaptionHighlights(int availableBytes);
    void resetCurrentByteCaptionHighlights();
    void clearCurrentByteInfo();
    void writeStatusLineToStdout(const QString& line);
    QString formatBinarySizeFixed2(quint64 bytes) const;
    void updateBufferStatusLine();
    bool isSingleFileModeActive() const;
    bool isSyntheticPreviewMatch(const MatchRecord& match) const;
    bool insertSyntheticPreviewResultAtTop();
    void requestSharedCenterFromTextScrollPosition(int sliderValue, int sliderMaximum);

    std::unique_ptr<Ui::MainWindow> m_ui;
    ResultModel m_resultModel;
    OpenFilePool m_filePool;
    ShiftedWindowLoader m_windowLoader;
    ScanController m_scanController;

    QVector<QString> m_sourceFiles;
    QVector<ScanTarget> m_scanTargets;
    QVector<ResultBuffer> m_resultBuffers;
    QVector<int> m_matchBufferIndices;

    ScanControlsPanel* m_scanControlsPanel = nullptr;
    ResultsTablePanel* m_resultsPanel = nullptr;
    TextViewPanel* m_textPanel = nullptr;
    CurrentByteInfoPanel* m_currentByteInfoPanel = nullptr;
    BitmapViewPanel* m_bitmapPanel = nullptr;
    TextViewWidget* m_textView = nullptr;
    BitmapViewWidget* m_bitmapView = nullptr;
    QSpinBox* m_shiftValueSpin = nullptr;
    QComboBox* m_shiftUnitCombo = nullptr;

    QHash<int, QVector<QPair<quint64, quint64>>> m_targetMatchIntervals;
    SourceMode m_sourceMode = SourceMode::None;
    QString m_selectedSourceDisplay;
    QString m_lastStatusLineText;
    HoverBuffer m_textHoverBuffer;
    HoverBuffer m_bitmapHoverBuffer;
    HoverSource m_lastHoverSource = HoverSource::None;
    std::optional<quint64> m_lastHoverAbsoluteOffset;
    int m_activePreviewRow = -1;
    quint64 m_sharedCenterOffset = 0;
    bool m_previewSyncInProgress = false;
    bool m_previewUpdateScheduled = false;
    std::optional<quint64> m_pendingCenterOffset;
    int m_activeOverlapTargetIdx = -1;
    bool m_mainSplitterHandleDragInProgress = false;
    quint64 m_textExpandBeforeBytes = 0;
    quint64 m_textExpandAfterBytes = 0;
    int m_lastSyntheticBufferIndex = -1;
    int m_pendingPageDirection = 0;
    std::optional<quint64> m_pendingPageEdgeOffset;
    int m_pendingFileEdgeNavigation = 0;
    bool m_textScrollDragInProgress = false;
    bool m_pendingPreviewAfterTextScrollDrag = false;
};

}  // namespace breco

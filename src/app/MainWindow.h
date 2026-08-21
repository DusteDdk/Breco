#pragma once

#include <QMainWindow>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

#include "io/OpenFilePool.h"
#include "io/ShiftedWindowLoader.h"
#include "model/ResultModel.h"
#include "scan/ScanController.h"

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QFileInfo;
class QSpinBox;
class QStackedWidget;
class QTimer;
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

namespace breco {

namespace lang {
class BrecoLangPanel;
}

class BitmapViewWidget;
class BitmapViewPanel;
class CurrentByteInfoPanel;
class DataViewByteAndBitmapPanel;
class DataViewImagePanel;
class DataViewShellPanel;
class EmbeddedImageScanController;
struct EmbeddedImageResult;
struct EmbeddedImageScanOptions;
struct EmbeddedImageScanSource;
struct EmbeddedImageScanSummary;
class HexViewControlsPanel;
class MainTabsPanel;
class ProtectedSourceOpener;
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
    void setProtectedSourceOpenerForTests(std::unique_ptr<ProtectedSourceOpener> opener);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

#ifdef BRECO_MAINWINDOW_TEST_ACCESS
public slots:
#else
private slots:
#endif
    void onSourcePathTextChanged(const QString& path);
    void validateSourcePathInput();
    void onOpenFile();
    void onOpenDirectory();
    void onStartScan();
    void onStartBrecoLangScan();
    void onStopScan();
    void onResultActivated(const QModelIndex& index);
    void onResultsBatchReady(const QVector<MatchRecord>& matches, int mergedTotal);
    void onProgressUpdated(const breco::ScanProgressSnapshot& progress);
    void onScanStarted(int fileCount, quint64 totalBytes);
    void onScanFinished(bool stoppedByUser, bool autoStoppedLimitExceeded);
    void onTextModeChanged(int idx);
    void onBitmapModeChanged(int idx);
    void onTextBackingScrollRequested(int wheelSteps, int bytesPerStepHint, int visibleBytesHint);
    void onTextHoverOffsetChanged(quint64 absoluteOffset);
    void onTextCenterAnchorRequested(quint64 absoluteOffset);
    void onBitmapHoverOffsetChanged(quint64 absoluteOffset);
    void onBitmapByteClicked(quint64 absoluteOffset);
    void onTextByteClicked(quint64 absoluteOffset);
    void onHoverLeft();

#ifdef BRECO_MAINWINDOW_TEST_ACCESS
public:
#else
private:
#endif
    enum class SourceMode { None, SingleFile, Directory };
    enum class SourceTargetKind { None, File, BlockDevice, Directory };
    enum class SourcePathFeedback { None, NotFound, Found, PermissionDenied, Open };
    enum class HoverSource { None, Text, Bitmap };
    enum class ScanKind { None, Text, BrecoLang };
    enum class HexNavigatorField { Offset, Selected, SelectTo };

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
    TextInterpretationMode selectedDataViewTextMode() const;
    bool dataViewBigEndianEnabled() const;
    void setDataViewBigEndianEnabled(bool enabled);
    void updateTextModeControlVisibility();
    void updateHexControlsVisibility();
    void updateHexInfoPanel();
    void commitHexNavigatorEdit(HexNavigatorField field);
    bool navigateHexView(quint64 viewportOffset,
                         std::optional<QPair<quint64, quint64>> selectionRange);
    static bool parseHexNavigatorOffset(const QString& text, quint64* offset);
    void refreshDataViewFromNavigator();
    void navigateToDecodedSource(const QString& filePath,
                                 quint64 absoluteOffset, quint64 byteLength);
    void setDecodedSourceHighlight(quint64 absoluteOffset,
                                   quint64 byteLength);
    void clearDecodedSourceHighlight();
    void setScanButtonMode(bool running);
    void startScan(ScanKind kind);
    void restoreTransientScanUi();
    void updateBlockSizeLabel();
    int selectedWorkerCount() const;
    QString humanBytes(quint64 bytes) const;
    bool selectSingleFileSource(const QString& filePath);
    bool selectDirectorySource(const QString& dirPath);
    bool applySourcePath(const QString& path, bool syncInputText);
    void previewSourcePath(const QString& path);
    SourceTargetKind classifySourceTarget(const QFileInfo& info) const;
    bool canOpenSourceTarget(const QString& path, SourceTargetKind kind) const;
    bool tryOpenProtectedSource(const QString& path, SourceTargetKind kind);
    void clearSourceSelection(bool clearRememberedSource = true);
    void rememberActiveSingleFileOffset(quint64 offset);
    void syncSourcePathInputText(const QString& path);
    void updateSourcePathFeedback(SourcePathFeedback feedback, SourceTargetKind kind,
                                  const QString& path);
    void refreshSourceSummary();
    void buildScanTargets(const QVector<QString>& filePaths);
    quint64 currentSelectedSourceBytes() const;
    void selectResultRow(int row);
    void requestSharedCenter(quint64 absoluteOffset);
    void jumpToAbsoluteOffset(quint64 absoluteOffset);
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
    void startImageScan();
    void cancelImageScan();
    std::optional<int> activePreviewTargetIndex() const;
    quint64 imageScanStartOffset() const;
    bool buildImageScanRequest(EmbeddedImageScanSource& source,
                               EmbeddedImageScanOptions& options,
                               QString& errorMessage) const;
    void finishImageScan(quint64 scanId, const EmbeddedImageScanSummary& summary,
                         const QVector<EmbeddedImageResult>& results);
    void saveSelectedBinaryRange(quint64 startOffset, quint64 endOffsetExclusive);
    void saveBinaryFromHere(quint64 startOffset);
    std::optional<quint64> promptBinaryRangeLength(quint64 remainingBytes);
    void saveBinaryRangeWithDialogs(const ScanTarget& target, quint64 startOffset,
                                    quint64 length);
    void saveBinaryRangeWithProgress(const QString& outputPath, const ScanTarget& target,
                                     quint64 startOffset, quint64 length);
    bool writeBinaryRangeToFile(
        const QString& outputPath, const ScanTarget& target, quint64 startOffset,
        quint64 length, const ShiftSettings& shift, QString* errorMessage,
        const std::function<void(quint64)>& progressCallback = {});
    static quint64 binaryLengthFromInput(double value, int unitIndex,
                                         quint64 remainingBytes);
    static QString binaryAmountText(quint64 bytes, int unitIndex = -1);
    static QString binaryProgressText(quint64 written, quint64 total);

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
    std::unique_ptr<EmbeddedImageScanController> m_imageScanController;
    std::unique_ptr<ProtectedSourceOpener> m_protectedSourceOpener;

    QVector<QString> m_sourceFiles;
    QVector<ScanTarget> m_scanTargets;
    QVector<ResultBuffer> m_resultBuffers;
    QVector<int> m_matchBufferIndices;

    MainTabsPanel* m_mainTabsPanel = nullptr;
    ScanControlsPanel* m_scanControlsPanel = nullptr;
    ResultsTablePanel* m_resultsPanel = nullptr;
    TextViewPanel* m_textPanel = nullptr;
    HexViewControlsPanel* m_hexControlsPanel = nullptr;
    DataViewShellPanel* m_rawDataViewShellPanel = nullptr;
    DataViewByteAndBitmapPanel* m_dataViewByteAndBitmapPanel = nullptr;
    DataViewImagePanel* m_dataViewImagePanel = nullptr;
    lang::BrecoLangPanel* m_brecoLangPanel = nullptr;
    CurrentByteInfoPanel* m_currentByteInfoPanel = nullptr;
    BitmapViewPanel* m_bitmapPanel = nullptr;
    TextViewWidget* m_textView = nullptr;
    BitmapViewWidget* m_bitmapView = nullptr;
    QSpinBox* m_shiftValueSpin = nullptr;
    QTimer* m_sourcePathValidationTimer = nullptr;

    QHash<int, QVector<QPair<quint64, quint64>>> m_targetMatchIntervals;
    SourceMode m_sourceMode = SourceMode::None;
    QString m_selectedSourceDisplay;
    QString m_lastStatusLineText;
    HoverBuffer m_textHoverBuffer;
    HoverBuffer m_bitmapHoverBuffer;
    HoverSource m_lastHoverSource = HoverSource::None;
    TextInterpretationMode m_lastTextInterpretationMode = TextInterpretationMode::Ascii;
    std::optional<quint64> m_lastHoverAbsoluteOffset;
    std::optional<QPair<quint64, quint64>> m_activeTextSelectionRange;
    bool m_decodedNavigationInProgress = false;
    std::optional<QPair<quint64, quint64>> m_decodedSourceHighlightRange;
    int m_activePreviewRow = -1;
    quint64 m_sharedCenterOffset = 0;
    bool m_previewSyncInProgress = false;
    bool m_previewUpdateScheduled = false;
    std::optional<quint64> m_pendingCenterOffset;
    std::optional<quint64> m_pendingHexViewportOffset;
    std::optional<QPair<quint64, quint64>> m_pendingHexSelectionRange;
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
    bool m_dataViewBigEndian = false;
    quint64 m_activeImageScanId = 0;
    ScanKind m_activeScanKind = ScanKind::None;
    QString m_savedSchemaSearchTerm;
    bool m_savedSchemaTermEnabled = true;
    bool m_savedIgnoreCaseEnabled = true;
    bool m_destroying = false;
};

}  // namespace breco

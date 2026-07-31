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
#include "struct/VisualizedNode.h"

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

class BitmapViewWidget;
class BitmapViewPanel;
class CurrentByteInfoPanel;
class DataViewByteAndBitmapPanel;
class DataViewImagePanel;
class DataViewShellPanel;
class DataViewStructuredPanel;
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
class StructDataViewPanel;
class StructModeLeftPanel;
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

private slots:
    void onSourcePathTextChanged(const QString& path);
    void validateSourcePathInput();
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
    void onDataViewModeChanged(int idx);
    void onBitmapModeChanged(int idx);
    void onTextBackingScrollRequested(int wheelSteps, int bytesPerStepHint, int visibleBytesHint);
    void onTextHoverOffsetChanged(quint64 absoluteOffset);
    void onTextCenterAnchorRequested(quint64 absoluteOffset);
    void onBitmapHoverOffsetChanged(quint64 absoluteOffset);
    void onBitmapByteClicked(quint64 absoluteOffset);
    void onTextByteClicked(quint64 absoluteOffset);
    void onHoverLeft();

private:
    enum class SourceMode { None, SingleFile, Directory };
    enum class SourceTargetKind { None, File, BlockDevice, Directory };
    enum class SourcePathFeedback { None, NotFound, Found, PermissionDenied, Open };
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

    struct StructViewState {
        quint64 id = 0;
        QString name;
        QString type;
        int repeat = 1;
        quint64 offset = 0;
        QString filePath;
        quint64 fileSize = 0;
        VisualizedNode decodedRoot;
        QString reloadError;
    };

    quint64 effectiveBlockSizeBytes() const;
    ShiftSettings currentShiftSettings() const;
    TextInterpretationMode selectedTextMode() const;
    TextInterpretationMode selectedDataViewTextMode() const;
    bool dataViewBigEndianEnabled() const;
    bool isStructViewActive() const;
    bool isImageViewActive() const;
    void updateStructViewVisibility();
    void updateTextModeControlVisibility();
    void updateHexControlsVisibility();
    void updateHexInfoPanel();
    void refreshDataViewFromNavigator();
    void syncStructPreviewToControls();
    void createStructPreview(quint64 absoluteOffset);
    void clearStructPreview();
    void addCurrentStructView();
    void removeCurrentStructViews(const QVector<quint64>& ids);
    void updateCurrentStructView(quint64 id, const QString& name, int repeat,
                                 quint64 offset);
    void rebuildStructVisualization();
    bool decodeStructView(StructViewState& view, bool allowSourceReload);
    quint64 structVisualizationStartOffset() const;
    void navigateToStructSource(const QString& filePath,
                                quint64 absoluteOffset, quint64 byteLength);
    void setStructSourceHighlight(quint64 absoluteOffset,
                                  quint64 byteLength);
    void clearStructSourceHighlight();
    void setScanButtonMode(bool running);
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
    DataViewShellPanel* m_dataViewShellPanel = nullptr;
    DataViewByteAndBitmapPanel* m_dataViewByteAndBitmapPanel = nullptr;
    DataViewImagePanel* m_dataViewImagePanel = nullptr;
    DataViewStructuredPanel* m_dataViewStructuredPanel = nullptr;
    CurrentByteInfoPanel* m_currentByteInfoPanel = nullptr;
    StructModeLeftPanel* m_structModeLeftPanel = nullptr;
    BitmapViewPanel* m_bitmapPanel = nullptr;
    StructDataViewPanel* m_structDataViewPanel = nullptr;
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
    std::optional<StructViewState> m_structPreview;
    QVector<StructViewState> m_currentStructViews;
    bool m_structNavigationInProgress = false;
    std::optional<QPair<quint64, quint64>> m_structSourceHighlightRange;
    quint64 m_nextStructViewId = 1;
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
    std::optional<int> m_protectedSourceDialogAnswerForTests;
    quint64 m_activeImageScanId = 0;
};

}  // namespace breco

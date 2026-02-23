#include "app/MainWindow.h"

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStringDecoder>
#include <QTableView>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>

#include "debug/SelectionTrace.h"
#include "io/FileEnumerator.h"
#include "panel/BitmapViewPanel.h"
#include "panel/ResultsTablePanel.h"
#include "panel/ScanControlsPanel.h"
#include "panel/TextViewPanel.h"
#include "scan/ShiftTransform.h"
#include "settings/AppSettings.h"
#include "ui_MainWindow.h"
#include "view/BitmapViewWidget.h"
#include "view/TextViewWidget.h"

namespace breco {

namespace {
constexpr quint64 kEvictedWindowRadiusBytes = 8ULL * 1024ULL * 1024ULL;
constexpr quint64 kResultBufferCacheBudgetBytes = 2048ULL * 1024ULL * 1024ULL;
constexpr quint64 kNotEmptyInitialBytes = 16ULL * 1024ULL * 1024ULL;

quint64 readUnsignedLittle(const QByteArray& bytes, int start, int widthBytes, bool* ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (start < 0 || widthBytes <= 0 || start + widthBytes > bytes.size()) {
        return 0;
    }
    quint64 value = 0;
    for (int i = 0; i < widthBytes; ++i) {
        value |= (static_cast<quint64>(static_cast<unsigned char>(bytes.at(start + i))) << (8 * i));
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return value;
}

QString printableAsciiChar(unsigned char byte) {
    if (byte >= 0x20 && byte <= 0x7E) {
        return QString(QChar::fromLatin1(static_cast<char>(byte)));
    }
    if (byte == '\n') {
        return QStringLiteral("\\n");
    }
    if (byte == '\r') {
        return QStringLiteral("\\r");
    }
    if (byte == '\t') {
        return QStringLiteral("\\t");
    }
    return QStringLiteral(".");
}

QString utf8Glyph(const QByteArray& bytes, int start) {
    if (start < 0 || start >= bytes.size()) {
        return QStringLiteral("n/a");
    }
    const QByteArray slice = bytes.mid(start, qMin(4, bytes.size() - start));
    const QString decoded = QString::fromUtf8(slice);
    if (decoded.isEmpty()) {
        return QStringLiteral("n/a");
    }
    return decoded.left(1);
}

QString utf16Glyph(const QByteArray& bytes, int start) {
    if (start < 0 || start >= bytes.size()) {
        return QStringLiteral("n/a");
    }
    const QByteArray slice = bytes.mid(start, qMin(4, bytes.size() - start));
    QStringDecoder decoder(QStringDecoder::Utf16LE);
    const QString decoded = decoder.decode(slice);
    if (decoded.isEmpty()) {
        return QStringLiteral("n/a");
    }
    return decoded.left(1);
}

QString formatHex(quint64 value, int widthNibbles) {
    return QStringLiteral("0x%1").arg(value, widthNibbles, 16, QChar('0')).toUpper();
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_ui(std::make_unique<Ui::MainWindow>()),
      m_resultModel(this),
      m_filePool(),
      m_windowLoader(&m_filePool),
      m_scanController(&m_filePool, this) {
    m_ui->setupUi(this);

    auto* scanHostLayout = new QVBoxLayout(m_ui->scanControlsHost);
    scanHostLayout->setContentsMargins(0, 0, 0, 0);
    scanHostLayout->setSpacing(0);
    m_scanControlsPanel = new ScanControlsPanel(m_ui->scanControlsHost);
    scanHostLayout->addWidget(m_scanControlsPanel);

    auto* resultsHostLayout = new QVBoxLayout(m_ui->resultsPanelHost);
    resultsHostLayout->setContentsMargins(0, 0, 0, 0);
    resultsHostLayout->setSpacing(0);
    m_resultsPanel = new ResultsTablePanel(m_ui->resultsPanelHost);
    resultsHostLayout->addWidget(m_resultsPanel);

    auto* textHostLayout = new QVBoxLayout(m_ui->textViewPanelHost);
    textHostLayout->setContentsMargins(0, 0, 0, 0);
    textHostLayout->setSpacing(0);
    m_textPanel = new TextViewPanel(m_ui->textViewPanelHost);
    textHostLayout->addWidget(m_textPanel);

    auto* bitmapHostLayout = new QVBoxLayout(m_ui->bitmapViewPanelHost);
    bitmapHostLayout->setContentsMargins(0, 0, 0, 0);
    bitmapHostLayout->setSpacing(0);
    m_bitmapPanel = new BitmapViewPanel(m_ui->bitmapViewPanelHost);
    bitmapHostLayout->addWidget(m_bitmapPanel);

    QTableView* resultsTable = m_resultsPanel->resultsTableView();
    resultsTable->setModel(&m_resultModel);
    resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    resultsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    resultsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int col = 2; col < m_resultModel.columnCount(); ++col) {
        resultsTable->horizontalHeader()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    }

    m_textPanel->textModeCombo()->addItems(
        {QStringLiteral("ASCII"), QStringLiteral("UTF-8"), QStringLiteral("UTF-16")});
    m_bitmapPanel->bitmapModeCombo()->addItems({QStringLiteral("RGB24"), QStringLiteral("Grey8"),
                                                QStringLiteral("Grey24"), QStringLiteral("RGBi256"),
                                                QStringLiteral("Binary"), QStringLiteral("Text")});
    m_scanControlsPanel->blockSizeSpin()->setValue(16);
    m_scanControlsPanel->blockSizeUnitCombo()->setCurrentIndex(2);
    m_scanControlsPanel->workerCountCombo()->clear();
    const int threadCount = qMax(1, QThread::idealThreadCount());
    for (int workers = 1; workers <= threadCount; ++workers) {
        m_scanControlsPanel->workerCountCombo()->addItem(QString::number(workers), workers);
    }
    m_scanControlsPanel->workerCountCombo()->setCurrentIndex(threadCount - 1);

    m_textView = new TextViewWidget(m_textPanel->textViewContainer());
    m_bitmapView = new BitmapViewWidget(m_bitmapPanel->bitmapViewContainer());
    m_textView->setMinimumHeight(220);
    m_bitmapView->setMinimumHeight(220);
    m_textView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_bitmapView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_textPanel->textViewContainer()->setMinimumHeight(220);
    m_bitmapPanel->bitmapViewContainer()->setMinimumHeight(220);

    auto* textLayout = new QVBoxLayout(m_textPanel->textViewContainer());
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(0);
    textLayout->addWidget(m_textView);

    auto* bitmapLayout = new QVBoxLayout(m_bitmapPanel->bitmapViewContainer());
    bitmapLayout->setContentsMargins(0, 0, 0, 0);
    bitmapLayout->setSpacing(0);
    bitmapLayout->addWidget(m_bitmapView);

    m_textPanel->textViewPanelLayout()->setStretch(1, 1);
    m_bitmapPanel->bitmapViewPanelLayout()->setStretch(1, 1);
    m_ui->verticalLayout->setStretch(1, 1);
    m_ui->verticalLayout->setStretch(2, 2);

    connect(m_scanControlsPanel->openFileButton(), &QPushButton::clicked, this,
            &MainWindow::onOpenFile);
    connect(m_scanControlsPanel->openDirButton(), &QPushButton::clicked, this,
            &MainWindow::onOpenDirectory);
    connect(m_scanControlsPanel->startScanButton(), &QPushButton::clicked, this,
            &MainWindow::onStartScan);
    connect(m_scanControlsPanel->searchTermLineEdit(), &QLineEdit::returnPressed, this,
            &MainWindow::onStartScan);
    connect(resultsTable->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& current, const QModelIndex&) { onResultActivated(current); });
    connect(m_textPanel->textModeCombo(), qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MainWindow::onTextModeChanged);
    auto updateTextModeControlVisibility = [this]() {
        const bool stringMode = m_textPanel->stringModeRadioButton()->isChecked();
        m_textPanel->wrapModeCheckBox()->setVisible(stringMode);
        m_textPanel->newlineModeComboBox()->setVisible(stringMode);
        m_textPanel->monospaceCheckBox()->setVisible(stringMode);
        m_textPanel->bytesPerLineComboBox()->setVisible(!stringMode);
    };
    connect(m_textPanel->stringModeRadioButton(), &QRadioButton::toggled, this,
            [this, updateTextModeControlVisibility](bool checked) {
                if (!checked) {
                    return;
                }
                m_textView->setDisplayMode(TextDisplayMode::StringMode);
                AppSettings::setTextByteModeEnabled(false);
                updateTextModeControlVisibility();
                scheduleSharedPreviewUpdate();
            });
    connect(m_textPanel->byteModeRadioButton(), &QRadioButton::toggled, this,
            [this, updateTextModeControlVisibility](bool checked) {
                if (!checked) {
                    return;
                }
                m_textView->setDisplayMode(TextDisplayMode::ByteMode);
                AppSettings::setTextByteModeEnabled(true);
                updateTextModeControlVisibility();
                scheduleSharedPreviewUpdate();
            });
    connect(m_textPanel->wrapModeCheckBox(), &QCheckBox::toggled, this,
            [this](bool checked) {
                m_textView->setWrapMode(checked);
                AppSettings::setTextWrapModeEnabled(checked);
                scheduleSharedPreviewUpdate();
            });
    connect(m_textPanel->newlineModeComboBox(), qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                m_textView->setNewlineMode(static_cast<TextNewlineMode>(qBound(0, idx, 4)));
                AppSettings::setTextNewlineModeIndex(idx);
                scheduleSharedPreviewUpdate();
            });
    connect(m_textPanel->monospaceCheckBox(), &QCheckBox::toggled, this,
            [this](bool checked) {
                m_textView->setMonospaceEnabled(checked);
                AppSettings::setTextMonospaceEnabled(checked);
                scheduleSharedPreviewUpdate();
            });
    connect(m_textPanel->bytesPerLineComboBox(),
            qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
                m_textView->setByteLineMode(static_cast<ByteLineMode>(qBound(0, idx, 4)));
                AppSettings::setTextByteLineModeIndex(idx);
                scheduleSharedPreviewUpdate();
            });

    connect(m_bitmapPanel->bitmapModeCombo(), qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MainWindow::onBitmapModeChanged);
    connect(m_scanControlsPanel->blockSizeSpin(), qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int) { updateBlockSizeLabel(); });
    connect(m_scanControlsPanel->blockSizeUnitCombo(), qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { updateBlockSizeLabel(); });

    connect(m_scanControlsPanel->shiftUnitCombo(), qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                if (idx == 0) {
                    m_scanControlsPanel->shiftValueSpin()->setRange(-7, 7);
                } else {
                    m_scanControlsPanel->shiftValueSpin()->setRange(-127, 127);
                }
                const QModelIndex current = m_resultsPanel->resultsTableView()->currentIndex();
                if (current.isValid()) {
                    onResultActivated(current);
                }
            });

    connect(m_scanControlsPanel->shiftValueSpin(), qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int) {
                const QModelIndex current = m_resultsPanel->resultsTableView()->currentIndex();
                if (current.isValid()) {
                    onResultActivated(current);
                }
            });

    connect(m_bitmapPanel->resultOverlayCheckBox(), &QCheckBox::toggled, this,
            [this](bool checked) { m_bitmapView->setResultOverlayEnabled(checked); });
    connect(m_textView, &TextViewWidget::centerAnchorOffsetChanged, this,
            &MainWindow::onTextCenterAnchorRequested);
    connect(m_textView, &TextViewWidget::hoverAbsoluteOffsetChanged, this,
            &MainWindow::onTextHoverOffsetChanged);
    connect(m_textView, &TextViewWidget::hoverLeft, this, &MainWindow::onHoverLeft);
    connect(m_textView, &TextViewWidget::selectionRangeChanged, this,
            [this](bool hasRange, quint64 start, quint64 end) {
                if (!hasRange) {
                    m_bitmapView->setExternalSelectionRange(std::nullopt);
                    return;
                }
                m_bitmapView->setExternalSelectionRange(qMakePair(start, end));
            });
    connect(m_textView, &TextViewWidget::backingScrollRequested, this,
            &MainWindow::onTextBackingScrollRequested);
    connect(m_bitmapView, &BitmapViewWidget::hoverAbsoluteOffsetChanged, this,
            &MainWindow::onBitmapHoverOffsetChanged);
    connect(m_bitmapView, &BitmapViewWidget::byteClicked, this, &MainWindow::onBitmapByteClicked);
    connect(m_bitmapView, &BitmapViewWidget::hoverLeft, this, &MainWindow::onHoverLeft);

    connect(m_bitmapPanel->bitmapZoomOutButton(), &QToolButton::clicked, this, [this]() {
        const int next = qMax(1, m_bitmapView->zoom() - 1);
        m_bitmapView->setZoom(next);
        m_bitmapPanel->bitmapZoomLabel()->setText(QStringLiteral("%1x").arg(next));
    });
    connect(m_bitmapPanel->bitmapZoomInButton(), &QToolButton::clicked, this, [this]() {
        const int next = qMin(32, m_bitmapView->zoom() + 1);
        m_bitmapView->setZoom(next);
        m_bitmapPanel->bitmapZoomLabel()->setText(QStringLiteral("%1x").arg(next));
    });
    connect(m_bitmapView, &BitmapViewWidget::zoomChanged, this,
            [this](int zoom) {
                m_bitmapPanel->bitmapZoomLabel()->setText(QStringLiteral("%1x").arg(zoom));
                scheduleSharedPreviewUpdate();
            });

    connect(&m_scanController, &ScanController::scanStarted, this, &MainWindow::onScanStarted);
    connect(&m_scanController, &ScanController::progressUpdated, this,
            &MainWindow::onProgressUpdated);
    connect(&m_scanController, &ScanController::resultsBatchReady, this,
            &MainWindow::onResultsBatchReady);
    connect(&m_scanController, &ScanController::scanFinished, this,
            &MainWindow::onScanFinished);
    connect(&m_scanController, &ScanController::scanError, this,
            [this](const QString& message) { QMessageBox::warning(this, "Breco", message); });

    m_ui->mainSplitter->setSizes({75, 25});
    m_textPanel->textModeRowLayout()->setContentsMargins(0, 0, 0, 0);
    m_textPanel->textModeRowLayout()->setSpacing(6);
    m_bitmapPanel->bitmapModeRowLayout()->setContentsMargins(0, 0, 0, 0);
    m_bitmapPanel->bitmapModeRowLayout()->setSpacing(6);
    const int controlsH = m_textPanel->textModeCombo()->sizeHint().height();
    m_textPanel->stringModeRadioButton()->setMaximumHeight(controlsH);
    m_textPanel->byteModeRadioButton()->setMaximumHeight(controlsH);
    m_textPanel->wrapModeCheckBox()->setMaximumHeight(controlsH);
    m_textPanel->newlineModeComboBox()->setMaximumHeight(controlsH);
    m_textPanel->monospaceCheckBox()->setMaximumHeight(controlsH);
    m_textPanel->bytesPerLineComboBox()->setMaximumHeight(controlsH);
    m_bitmapPanel->bitmapModeCombo()->setFixedHeight(controlsH);
    m_bitmapPanel->resultOverlayCheckBox()->setMaximumHeight(controlsH);
    m_bitmapPanel->bitmapZoomOutButton()->setFixedHeight(controlsH);
    m_bitmapPanel->bitmapZoomInButton()->setFixedHeight(controlsH);
    m_bitmapPanel->bitmapZoomLabel()->setFixedHeight(controlsH);
    m_bitmapPanel->bitmapZoomLabel()->setMinimumWidth(36);

    m_bitmapView->setResultOverlayEnabled(m_bitmapPanel->resultOverlayCheckBox()->isChecked());
    m_bitmapView->setZoom(1);
    m_bitmapPanel->bitmapZoomLabel()->setText(QStringLiteral("1x"));

    m_scanControlsPanel->shiftUnitCombo()->setCurrentIndex(0);
    m_scanControlsPanel->shiftValueSpin()->setRange(-7, 7);
    m_scanControlsPanel->shiftValueSpin()->setValue(0);

    setScanButtonMode(false);

    const bool byteMode = AppSettings::textByteModeEnabled();
    const bool wrap = AppSettings::textWrapModeEnabled();
    const bool monospace = AppSettings::textMonospaceEnabled();
    const int newlineModeIdx =
        qBound(0, AppSettings::textNewlineModeIndex(), m_textPanel->newlineModeComboBox()->count() - 1);
    const int byteLineModeIdx =
        qBound(0, AppSettings::textByteLineModeIndex(), m_textPanel->bytesPerLineComboBox()->count() - 1);
    const bool prefillOnMerge = AppSettings::prefillOnMergeEnabled();
    m_textPanel->stringModeRadioButton()->setChecked(!byteMode);
    m_textPanel->byteModeRadioButton()->setChecked(byteMode);
    m_textPanel->wrapModeCheckBox()->setChecked(wrap);
    m_textPanel->newlineModeComboBox()->setCurrentIndex(newlineModeIdx);
    m_textPanel->monospaceCheckBox()->setChecked(monospace);
    m_textPanel->bytesPerLineComboBox()->setCurrentIndex(byteLineModeIdx);
    m_scanControlsPanel->prefillOnMergeCheckBox()->setChecked(prefillOnMerge);
    m_textView->setDisplayMode(byteMode ? TextDisplayMode::ByteMode : TextDisplayMode::StringMode);
    m_textView->setNewlineMode(static_cast<TextNewlineMode>(newlineModeIdx));
    m_textView->setWrapMode(wrap);
    m_textView->setMonospaceEnabled(monospace);
    m_textView->setByteLineMode(static_cast<ByteLineMode>(byteLineModeIdx));
    m_bitmapView->setTextMode(selectedTextMode());
    connect(m_scanControlsPanel->prefillOnMergeCheckBox(), &QCheckBox::toggled, this,
            [](bool checked) { AppSettings::setPrefillOnMergeEnabled(checked); });

    updateTextModeControlVisibility();
    writeStatusLineToStdout(QStringLiteral("Hover: -"));

    m_resultModel.setScanTargets(&m_scanTargets);
    refreshSourceSummary();
    updateBlockSizeLabel();
}

MainWindow::~MainWindow() = default;

void MainWindow::onOpenFile() {
    const QString filePath = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open file/device"), AppSettings::lastFileDialogPath());
    if (filePath.isEmpty()) {
        return;
    }

    m_sourceFiles = FileEnumerator::enumerateSingleFile(filePath);
    m_sourceMode = SourceMode::SingleFile;
    m_selectedSourceDisplay = filePath;
    buildScanTargets(m_sourceFiles);
    m_resultModel.clear();
    clearResultBufferCacheState();
    m_targetMatchIntervals.clear();
    m_textHoverBuffer = {};
    m_bitmapHoverBuffer = {};

    AppSettings::setLastFileDialogPath(filePath);
    refreshSourceSummary();
    loadNotEmptyPreview();
}

void MainWindow::onOpenDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open directory"), AppSettings::lastDirectoryDialogPath());
    if (dir.isEmpty()) {
        return;
    }

    m_sourceFiles = FileEnumerator::enumerateRecursive(dir);
    m_sourceMode = SourceMode::Directory;
    m_selectedSourceDisplay = dir;
    buildScanTargets(m_sourceFiles);
    m_resultModel.clear();
    clearResultBufferCacheState();
    m_targetMatchIntervals.clear();
    m_textHoverBuffer = {};
    m_bitmapHoverBuffer = {};

    AppSettings::setLastDirectoryDialogPath(dir);
    refreshSourceSummary();
}

void MainWindow::onStartScan() {
    if (m_scanController.isRunning()) {
        onStopScan();
        return;
    }
    if (m_scanTargets.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Breco"),
                                 QStringLiteral("Select file or directory first."));
        return;
    }

    const QByteArray term = m_scanControlsPanel->searchTermLineEdit()->text().toUtf8();
    if (term.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Breco"),
                                 QStringLiteral("Enter a search term."));
        return;
    }
    const auto scanButtonPressedAt = std::chrono::steady_clock::now();

    m_resultModel.clear();
    clearResultBufferCacheState();
    m_targetMatchIntervals.clear();
    m_textHoverBuffer = {};
    m_bitmapHoverBuffer = {};
    onHoverLeft();

    m_scanControlsPanel->scanProgressBar()->setValue(0);
    m_resultShiftSettings = currentShiftSettings();
    m_hasResultShiftSettings = true;
    m_scanController.startScan(m_scanTargets, term, effectiveBlockSizeBytes(), selectedWorkerCount(),
                               selectedTextMode(),
                               m_scanControlsPanel->ignoreCaseCheckBox()->isChecked(),
                               currentShiftSettings(),
                               m_scanControlsPanel->prefillOnMergeCheckBox()->isChecked(),
                               scanButtonPressedAt);
}

void MainWindow::onStopScan() { m_scanController.requestStop(); }

void MainWindow::onResultActivated(const QModelIndex& index) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onResultActivated: indexValid=%1 row=%2")
                           .arg(index.isValid() ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(index.row()));
    }
    if (!index.isValid()) {
        BRECO_SELTRACE("onResultActivated: invalid index, return");
        return;
    }

    const int row = index.row();
    const MatchRecord* match = m_resultModel.matchAt(row);
    if (match == nullptr) {
        BRECO_SELTRACE(QStringLiteral("onResultActivated: no match for row=%1, return").arg(row));
        return;
    }
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onResultActivated: row=%1 targetIdx=%2 offset=%3")
                           .arg(row)
                           .arg(match->scanTargetIdx)
                           .arg(match->offset));
    }

    if (match->scanTargetIdx != m_activeOverlapTargetIdx) {
        const auto it = m_targetMatchIntervals.constFind(match->scanTargetIdx);
        if (it != m_targetMatchIntervals.constEnd()) {
            if (debug::selectionTraceEnabled()) {
                BRECO_SELTRACE(QStringLiteral("onResultActivated: setOverlapIntervals targetIdx=%1 intervals=%2")
                                   .arg(match->scanTargetIdx)
                                   .arg(it->size()));
            }
            m_bitmapView->setOverlapIntervals(*it);
        } else {
            BRECO_SELTRACE(QStringLiteral("onResultActivated: setOverlapIntervals targetIdx=%1 intervals=0")
                               .arg(match->scanTargetIdx));
            m_bitmapView->setOverlapIntervals({});
        }
        m_activeOverlapTargetIdx = match->scanTargetIdx;
    } else if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onResultActivated: overlap target unchanged targetIdx=%1")
                           .arg(match->scanTargetIdx));
    }

    BRECO_SELTRACE("onResultActivated: showMatchPreview begin");
    showMatchPreview(row, *match);
    BRECO_SELTRACE("onResultActivated: showMatchPreview end");
}

void MainWindow::onResultsBatchReady(const QVector<MatchRecord>& matches, int mergedTotal) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onResultsBatchReady: start matches=%1 mergedTotal=%2")
                           .arg(matches.size())
                           .arg(mergedTotal));
    }
    m_resultBuffers = m_scanController.resultBuffers();
    m_matchBufferIndices = m_scanController.matchBufferIndices();
    m_resultModel.appendBatch(matches);
    BRECO_SELTRACE("onResultsBatchReady: enforceBufferCacheBudget begin");
    const int evictions = enforceBufferCacheBudget();
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onResultsBatchReady: enforceBufferCacheBudget evictions=%1")
                           .arg(evictions));
    }
    BRECO_SELTRACE("onResultsBatchReady: enforceBufferCacheBudget end");
    rebuildTargetMatchIntervals();
    m_activeOverlapTargetIdx = -1;
    writeStatusLineToStdout(QStringLiteral("Merged results: %1").arg(mergedTotal));
    BRECO_SELTRACE("onResultsBatchReady: done");
}

void MainWindow::onProgressUpdated(quint64 scanned, quint64 total) {
    if (total > 0) {
        const int progress = static_cast<int>((static_cast<long double>(scanned) /
                                               static_cast<long double>(total)) *
                                              1000.0L);
        m_scanControlsPanel->scanProgressBar()->setValue(qBound(0, progress, 1000));
    }
    m_scanControlsPanel->scannedValueLabel()->setText(humanBytes(scanned));
    m_scanControlsPanel->searchSpaceValueLabel()->setText(humanBytes(total));
}

void MainWindow::onScanStarted(int fileCount, quint64 totalBytes) {
    m_scanControlsPanel->filesCountValueLabel()->setText(QString::number(fileCount));
    m_scanControlsPanel->searchSpaceValueLabel()->setText(humanBytes(totalBytes));
    setScanButtonMode(true);
    writeStatusLineToStdout(QStringLiteral("Scanning..."));
}

void MainWindow::onScanFinished(bool stoppedByUser, bool) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onScanFinished: stoppedByUser=%1 rows=%2")
                           .arg(stoppedByUser ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(m_resultModel.rowCount()));
    }
    setScanButtonMode(false);
    QString msg = QStringLiteral("Scan finished");
    if (stoppedByUser) {
        msg = QStringLiteral("Scan stopped by user");
    }
    writeStatusLineToStdout(msg);
    if (m_resultModel.rowCount() > 0) {
        BRECO_SELTRACE("onScanFinished: selecting first row");
        selectResultRow(0);
    }
}

void MainWindow::onTextModeChanged(int idx) {
    switch (idx) {
        case 0:
            m_textView->setMode(TextInterpretationMode::Ascii);
            m_bitmapView->setTextMode(TextInterpretationMode::Ascii);
            break;
        case 1:
            m_textView->setMode(TextInterpretationMode::Utf8);
            m_bitmapView->setTextMode(TextInterpretationMode::Utf8);
            break;
        case 2:
            m_textView->setMode(TextInterpretationMode::Utf16);
            m_bitmapView->setTextMode(TextInterpretationMode::Utf16);
            break;
        default:
            break;
    }
    scheduleSharedPreviewUpdate();
}

void MainWindow::onBitmapModeChanged(int idx) {
    switch (idx) {
        case 0:
            m_bitmapView->setMode(BitmapMode::Rgb24);
            break;
        case 1:
            m_bitmapView->setMode(BitmapMode::Grey8);
            break;
        case 2:
            m_bitmapView->setMode(BitmapMode::Grey24);
            break;
        case 3:
            m_bitmapView->setMode(BitmapMode::Rgbi256);
            break;
        case 4:
            m_bitmapView->setMode(BitmapMode::Binary);
            break;
        case 5:
            m_bitmapView->setMode(BitmapMode::Text);
            break;
        default:
            break;
    }
    scheduleSharedPreviewUpdate();
}

void MainWindow::onTextBackingScrollRequested(int wheelSteps, int bytesPerStepHint,
                                              int visibleBytesHint) {
    Q_UNUSED(visibleBytesHint);
    const qint64 bytesPerWheelStep =
        static_cast<qint64>(qMax(1, bytesPerStepHint)) * 4LL;
    const qint64 delta = -static_cast<qint64>(wheelSteps) * bytesPerWheelStep;
    shiftSharedCenterBy(delta);
}

quint64 MainWindow::effectiveBlockSizeBytes() const {
    quint64 blockSize = static_cast<quint64>(qMax(1, m_scanControlsPanel->blockSizeSpin()->value()));
    switch (m_scanControlsPanel->blockSizeUnitCombo()->currentIndex()) {
        case 0:
            return blockSize;
        case 1:
            return blockSize * 1024ULL;
        case 2:
            return blockSize * 1024ULL * 1024ULL;
        default:
            return blockSize;
    }
}

ShiftSettings MainWindow::currentShiftSettings() const {
    ShiftSettings shift;
    shift.amount = m_scanControlsPanel->shiftValueSpin()->value();
    shift.unit =
        (m_scanControlsPanel->shiftUnitCombo()->currentIndex() == 0) ? ShiftUnit::Bytes : ShiftUnit::Bits;
    return shift;
}

TextInterpretationMode MainWindow::selectedTextMode() const {
    switch (m_textPanel->textModeCombo()->currentIndex()) {
        case 1:
            return TextInterpretationMode::Utf8;
        case 2:
            return TextInterpretationMode::Utf16;
        case 0:
        default:
            return TextInterpretationMode::Ascii;
    }
}

void MainWindow::setScanButtonMode(bool running) {
    m_scanControlsPanel->startScanButton()->setText(running ? QStringLiteral("Stop")
                                                            : QStringLiteral("Scan"));
}

void MainWindow::updateBlockSizeLabel() {
    if (m_sourceMode == SourceMode::SingleFile && m_scanTargets.size() == 1) {
        const quint64 bytes = m_scanTargets.first().fileSize;
        const quint64 block = effectiveBlockSizeBytes();
        const quint64 blockCount = (bytes + block - 1) / block;
        m_scanControlsPanel->blockSizeLabel()->setText(
            QStringLiteral("Block size (%1 blocks)").arg(QString::number(blockCount)));
        return;
    }
    m_scanControlsPanel->blockSizeLabel()->setText(QStringLiteral("Block size"));
}

int MainWindow::selectedWorkerCount() const {
    const QVariant workerData = m_scanControlsPanel->workerCountCombo()->currentData();
    if (workerData.isValid()) {
        return qMax(1, workerData.toInt());
    }
    return qMax(1, m_scanControlsPanel->workerCountCombo()->currentText().toInt());
}

QString MainWindow::humanBytes(quint64 bytes) const {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    long double value = static_cast<long double>(bytes);
    int unitIdx = 0;
    while (value >= 1024.0L && unitIdx < 4) {
        value /= 1024.0L;
        ++unitIdx;
    }
    return QStringLiteral("%1 %2").arg(QString::number(static_cast<double>(value), 'f', 2), units[unitIdx]);
}

void MainWindow::refreshSourceSummary() {
    m_scanControlsPanel->filesCountValueLabel()->setText(QString::number(m_scanTargets.size()));
    m_scanControlsPanel->searchSpaceValueLabel()->setText(humanBytes(currentSelectedSourceBytes()));
    m_scanControlsPanel->selectedSourceValueLabel()->setText(
        m_selectedSourceDisplay.isEmpty() ? QStringLiteral("-") : m_selectedSourceDisplay);
    updateBlockSizeLabel();
}

void MainWindow::buildScanTargets(const QVector<QString>& filePaths) {
    m_scanTargets.clear();
    for (const QString& path : filePaths) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile() || !info.isReadable() || info.size() <= 0) {
            continue;
        }
        ScanTarget target;
        target.filePath = info.absoluteFilePath();
        target.fileSize = static_cast<quint64>(info.size());
        m_scanTargets.push_back(target);
    }
    m_resultModel.setScanTargets(&m_scanTargets);
}

quint64 MainWindow::currentSelectedSourceBytes() const {
    quint64 total = 0;
    for (const ScanTarget& target : m_scanTargets) {
        total += target.fileSize;
    }
    return total;
}

void MainWindow::selectResultRow(int row) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("selectResultRow: requestedRow=%1 rowCount=%2")
                           .arg(row)
                           .arg(m_resultModel.rowCount()));
    }
    if (row < 0 || row >= m_resultModel.rowCount()) {
        BRECO_SELTRACE("selectResultRow: out of range, return");
        return;
    }
    QTableView* resultsTable = m_resultsPanel->resultsTableView();
    const QModelIndex idx = m_resultModel.index(row, 0);
    const QModelIndex previous = resultsTable->currentIndex();
    resultsTable->setCurrentIndex(idx);
    resultsTable->selectRow(row);
    if (!previous.isValid() || previous.row() != row) {
        if (debug::selectionTraceEnabled()) {
            BRECO_SELTRACE(QStringLiteral("selectResultRow: selection model emitted change (previousRow=%1), return")
                               .arg(previous.row()));
        }
        return;
    }
    BRECO_SELTRACE("selectResultRow: current row unchanged, invoking onResultActivated directly");
    onResultActivated(idx);
}

QString MainWindow::filePathForTarget(int targetIdx) const {
    if (targetIdx < 0 || targetIdx >= m_scanTargets.size()) {
        return {};
    }
    return m_scanTargets.at(targetIdx).filePath;
}

QVector<int> MainWindow::bufferReferenceCounts() const {
    QVector<int> counts;
    counts.fill(0, m_resultBuffers.size());
    for (const int bufferIndex : m_matchBufferIndices) {
        if (bufferIndex >= 0 && bufferIndex < counts.size()) {
            ++counts[bufferIndex];
        }
    }
    return counts;
}

quint64 MainWindow::totalResidentBufferBytes(const QVector<int>& refCounts) const {
    quint64 total = 0;
    const int count = qMin(refCounts.size(), m_resultBuffers.size());
    for (int i = 0; i < count; ++i) {
        if (refCounts.at(i) <= 0) {
            continue;
        }
        total += static_cast<quint64>(qMax(0, m_resultBuffers.at(i).bytes.size()));
    }
    return total;
}

bool MainWindow::evictOneBufferLargestFirstLeastUsed(const QSet<int>& protectedBufferIndices) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("evictOneBufferLargestFirstLeastUsed: start buffers=%1 protected=%2")
                           .arg(m_resultBuffers.size())
                           .arg(protectedBufferIndices.size()));
    }
    if (m_resultBuffers.isEmpty() || m_matchBufferIndices.isEmpty()) {
        BRECO_SELTRACE("evictOneBufferLargestFirstLeastUsed: no buffers or mapping, return false");
        return false;
    }

    const QVector<int> refCounts = bufferReferenceCounts();
    int candidate = -1;
    quint64 candidateSize = 0;
    int candidateRefs = std::numeric_limits<int>::max();
    for (int i = 0; i < m_resultBuffers.size() && i < refCounts.size(); ++i) {
        if (protectedBufferIndices.contains(i)) {
            continue;
        }
        if (refCounts.at(i) <= 0) {
            continue;
        }
        const quint64 size = static_cast<quint64>(qMax(0, m_resultBuffers.at(i).bytes.size()));
        if (size == 0) {
            continue;
        }
        const int refs = refCounts.at(i);
        if (candidate < 0 || size > candidateSize ||
            (size == candidateSize && refs < candidateRefs)) {
            candidate = i;
            candidateSize = size;
            candidateRefs = refs;
        }
    }
    if (candidate < 0) {
        BRECO_SELTRACE("evictOneBufferLargestFirstLeastUsed: no eviction candidate");
        return false;
    }
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("evictOneBufferLargestFirstLeastUsed: candidate=%1 size=%2 refs=%3")
                           .arg(candidate)
                           .arg(candidateSize)
                           .arg(candidateRefs));
    }

    QVector<int> affectedRows;
    affectedRows.reserve(candidateRefs);
    for (int row = 0; row < m_matchBufferIndices.size(); ++row) {
        if (m_matchBufferIndices.at(row) == candidate) {
            affectedRows.push_back(row);
        }
    }
    if (affectedRows.isEmpty()) {
        std::cout << "[cache] evicting buffer#" << candidate
                  << " size=" << candidateSize
                  << " refs=" << candidateRefs
                  << " action=clear-unreferenced-by-rows" << std::endl;
        m_resultBuffers[candidate].bytes.clear();
        BRECO_SELTRACE(QStringLiteral("evictOneBufferLargestFirstLeastUsed: cleared unreferenced candidate=%1")
                           .arg(candidate));
        return true;
    }

    const int firstRow = affectedRows.first();
    const MatchRecord* firstMatch = m_resultModel.matchAt(firstRow);
    if (firstMatch == nullptr) {
        BRECO_SELTRACE("evictOneBufferLargestFirstLeastUsed: firstMatch missing, return false");
        return false;
    }
    std::cout << "[cache] evicting buffer#" << candidate
              << " size=" << candidateSize
              << " refs=" << candidateRefs
              << " affectedRows=" << affectedRows.size()
              << " action=replace-with-zero-length-placeholders" << std::endl;
    m_resultBuffers[candidate] = makeEvictedPlaceholderBuffer(*firstMatch);
    m_matchBufferIndices[firstRow] = candidate;

    for (int i = 1; i < affectedRows.size(); ++i) {
        const int row = affectedRows.at(i);
        const MatchRecord* match = m_resultModel.matchAt(row);
        if (match == nullptr) {
            continue;
        }
        const int newIndex = m_resultBuffers.size();
        m_resultBuffers.push_back(makeEvictedPlaceholderBuffer(*match));
        m_matchBufferIndices[row] = newIndex;
    }
    BRECO_SELTRACE(QStringLiteral("evictOneBufferLargestFirstLeastUsed: replaced candidate=%1 with %2 placeholder rows")
                       .arg(candidate)
                       .arg(affectedRows.size()));
    return true;
}

int MainWindow::enforceBufferCacheBudget(const QSet<int>& protectedBufferIndices) {
    const bool traceEnabled = debug::selectionTraceEnabled();
    if (traceEnabled) {
        const QVector<int> refCounts = bufferReferenceCounts();
        BRECO_SELTRACE(QStringLiteral("enforceBufferCacheBudget: start resident=%1 budget=%2 protected=%3")
                           .arg(totalResidentBufferBytes(refCounts))
                           .arg(kResultBufferCacheBudgetBytes)
                           .arg(protectedBufferIndices.size()));
    }
    int evictions = 0;
    while (true) {
        const QVector<int> refCounts = bufferReferenceCounts();
        const quint64 resident = totalResidentBufferBytes(refCounts);
        if (resident <= kResultBufferCacheBudgetBytes) {
            if (traceEnabled) {
                BRECO_SELTRACE(QStringLiteral("enforceBufferCacheBudget: within budget resident=%1 evictions=%2")
                                   .arg(resident)
                                   .arg(evictions));
            }
            break;
        }
        if (traceEnabled) {
            BRECO_SELTRACE(QStringLiteral("enforceBufferCacheBudget: over budget resident=%1 evictions=%2")
                               .arg(resident)
                               .arg(evictions));
        }
        if (!evictOneBufferLargestFirstLeastUsed(protectedBufferIndices)) {
            if (traceEnabled) {
                BRECO_SELTRACE(QStringLiteral("enforceBufferCacheBudget: eviction unavailable at resident=%1")
                                   .arg(resident));
            }
            break;
        }
        ++evictions;
    }
    return evictions;
}

bool MainWindow::ensureRowBufferLoaded(int row, const MatchRecord& match,
                                       const QSet<int>& protectedBufferIndices) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: row=%1 targetIdx=%2 offset=%3")
                           .arg(row)
                           .arg(match.scanTargetIdx)
                           .arg(match.offset));
    }
    if (row < 0 || row >= m_matchBufferIndices.size()) {
        BRECO_SELTRACE("ensureRowBufferLoaded: row outside mapping, return false");
        return false;
    }
    const int bufferIndex = m_matchBufferIndices.at(row);
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: invalid bufferIndex=%1, return false")
                           .arg(bufferIndex));
        return false;
    }
    if (!m_resultBuffers.at(bufferIndex).bytes.isEmpty()) {
        BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: bufferIndex=%1 already resident").arg(bufferIndex));
        return true;
    }

    std::cout << "[cache] on-demand load start t+"
              << (debug::selectionTraceElapsedUs() / 1000ULL) << "ms: row=" << row
              << " buffer#" << bufferIndex
              << " targetIdx=" << match.scanTargetIdx
              << " matchOffset=" << match.offset << std::endl;

    const quint64 loadStartUs = debug::selectionTraceElapsedUs();
    const ResultBuffer loaded = loadEvictedWindowForMatch(match);
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: loadEvictedWindowForMatch took=%1us size=%2")
                           .arg(debug::selectionTraceElapsedUs() - loadStartUs)
                           .arg(loaded.bytes.size()));
    }
    const quint64 loadElapsedUs = debug::selectionTraceElapsedUs() - loadStartUs;
    const quint64 loadElapsedMs = loadElapsedUs / 1000ULL;
    if (loaded.bytes.isEmpty()) {
        std::cout << "[cache] on-demand load failed t+"
                  << (debug::selectionTraceElapsedUs() / 1000ULL) << "ms: row=" << row
                  << " buffer#" << bufferIndex
                  << " elapsedMs=" << loadElapsedMs << std::endl;
        BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: on-demand load failed row=%1 bufferIndex=%2")
                           .arg(row)
                           .arg(bufferIndex));
        return false;
    }
    m_resultBuffers[bufferIndex] = loaded;
    std::cout << "[cache] on-demand load finished t+"
              << (debug::selectionTraceElapsedUs() / 1000ULL) << "ms: row=" << row
              << " buffer#" << bufferIndex
              << " start=" << loaded.fileOffset
              << " size=" << loaded.bytes.size()
              << " elapsedMs=" << loadElapsedMs << std::endl;

    QSet<int> protectedSet = protectedBufferIndices;
    protectedSet.insert(bufferIndex);
    BRECO_SELTRACE("ensureRowBufferLoaded: enforceBufferCacheBudget begin");
    enforceBufferCacheBudget(protectedSet);
    BRECO_SELTRACE("ensureRowBufferLoaded: enforceBufferCacheBudget end");
    BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: finished row=%1 bufferIndex=%2")
                       .arg(row)
                       .arg(bufferIndex));
    return !m_resultBuffers.at(bufferIndex).bytes.isEmpty();
}

ResultBuffer MainWindow::makeEvictedPlaceholderBuffer(const MatchRecord& match) const {
    ResultBuffer placeholder;
    placeholder.scanTargetIdx = match.scanTargetIdx;
    placeholder.fileOffset = match.offset;
    placeholder.bytes.clear();
    return placeholder;
}

ResultBuffer MainWindow::loadEvictedWindowForMatch(const MatchRecord& match) const {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("loadEvictedWindowForMatch: start targetIdx=%1 offset=%2")
                           .arg(match.scanTargetIdx)
                           .arg(match.offset));
    }
    ResultBuffer out;
    if (match.scanTargetIdx < 0 || match.scanTargetIdx >= m_scanTargets.size()) {
        BRECO_SELTRACE("loadEvictedWindowForMatch: invalid target index, return empty");
        return out;
    }

    const ScanTarget& target = m_scanTargets.at(match.scanTargetIdx);
    if (target.filePath.isEmpty() || target.fileSize == 0) {
        BRECO_SELTRACE("loadEvictedWindowForMatch: empty target path or size, return empty");
        return out;
    }

    const quint64 termLen = static_cast<quint64>(m_scanController.searchTermLength());
    const quint64 start =
        (match.offset > kEvictedWindowRadiusBytes) ? (match.offset - kEvictedWindowRadiusBytes) : 0;
    const quint64 end =
        qMin(target.fileSize, match.offset + termLen + kEvictedWindowRadiusBytes);
    if (end <= start) {
        BRECO_SELTRACE(QStringLiteral("loadEvictedWindowForMatch: invalid range start=%1 end=%2")
                           .arg(start)
                           .arg(end));
        return out;
    }
    const quint64 size = end - start;

    const ShiftSettings shift = m_hasResultShiftSettings ? m_resultShiftSettings : currentShiftSettings();
    const ShiftReadPlan plan = ShiftTransform::makeReadPlan(start, size, target.fileSize, shift);
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral(
                           "loadEvictedWindowForMatch: outputStart=%1 outputSize=%2 readStart=%3 readSize=%4")
                           .arg(start)
                           .arg(size)
                           .arg(plan.readStart)
                           .arg(plan.readSize));
    }
    const quint64 loadStartUs = debug::selectionTraceElapsedUs();
    const auto transformed =
        m_windowLoader.loadTransformedWindow(target.filePath, target.fileSize, start, size, shift);
    if (debug::selectionTraceEnabled()) {
        const quint64 elapsed = debug::selectionTraceElapsedUs() - loadStartUs;
        BRECO_SELTRACE(QStringLiteral(
                           "loadEvictedWindowForMatch: loadTransformedWindow done elapsed=%1us hasValue=%2 size=%3")
                           .arg(elapsed)
                           .arg(transformed.has_value() ? QStringLiteral("true")
                                                        : QStringLiteral("false"))
                           .arg(transformed.has_value() ? transformed.value().size() : -1));
    }
    if (!transformed.has_value()) {
        BRECO_SELTRACE("loadEvictedWindowForMatch: loadTransformedWindow failed, return empty");
        return out;
    }

    out.scanTargetIdx = match.scanTargetIdx;
    out.fileOffset = start;
    out.bytes = transformed.value();
    if (debug::selectionTraceEnabled()) {
        if (shift.amount == 0 && plan.readStart == start && plan.readSize == size) {
            BRECO_SELTRACE(QStringLiteral(
                               "loadEvictedWindowForMatch: transform skipped (zero shift fast path) outSize=%1")
                               .arg(out.bytes.size()));
        }
    }
    BRECO_SELTRACE("loadEvictedWindowForMatch: done");
    return out;
}

void MainWindow::clearResultBufferCacheState() {
    m_resultBuffers.clear();
    m_matchBufferIndices.clear();
    m_hasResultShiftSettings = false;
    m_resultShiftSettings = {};
    m_activePreviewRow = -1;
    m_activeOverlapTargetIdx = -1;
    m_sharedCenterOffset = 0;
    m_pendingCenterOffset.reset();
    m_previewUpdateScheduled = false;
    m_textHoverBuffer = {};
    m_bitmapHoverBuffer = {};
}

void MainWindow::rebuildTargetMatchIntervals() {
    m_targetMatchIntervals.clear();
    const quint64 termLen = static_cast<quint64>(m_scanController.searchTermLength());
    const QVector<MatchRecord>& matches = m_resultModel.allMatches();
    for (const MatchRecord& match : matches) {
        const quint64 start = match.offset;
        const quint64 end = start + qMax<quint64>(1, termLen);
        m_targetMatchIntervals[match.scanTargetIdx].push_back(qMakePair(start, end));
    }
}

std::optional<unsigned char> MainWindow::previousByteBeforeViewport(const ResultBuffer& buffer,
                                                                    quint64 viewportStart) const {
    if (buffer.bytes.isEmpty() || viewportStart <= buffer.fileOffset) {
        return std::nullopt;
    }

    const quint64 rel = viewportStart - buffer.fileOffset;
    if (rel == 0 || rel > static_cast<quint64>(buffer.bytes.size())) {
        return std::nullopt;
    }
    return static_cast<unsigned char>(buffer.bytes.at(static_cast<int>(rel - 1)));
}

quint64 MainWindow::clampViewportStart(const ResultBuffer& buffer, quint64 desiredStart,
                                       quint64 windowBytes) const {
    if (buffer.bytes.isEmpty()) {
        return buffer.fileOffset;
    }

    const quint64 bufferStart = buffer.fileOffset;
    const quint64 bufferSize = static_cast<quint64>(qMax(0, buffer.bytes.size()));
    const quint64 clampedWindow = qMin(windowBytes, bufferSize);
    if (clampedWindow == 0) {
        return bufferStart;
    }

    const quint64 maxStart = bufferStart + (bufferSize - clampedWindow);
    return qBound(bufferStart, desiredStart, maxStart);
}

MainWindow::ViewportWindow MainWindow::viewportFromStart(const ResultBuffer& buffer,
                                                         quint64 startOffset,
                                                         quint64 windowBytes) const {
    ViewportWindow window;
    if (buffer.bytes.isEmpty() || windowBytes == 0) {
        return window;
    }

    const quint64 clampedStart = clampViewportStart(buffer, startOffset, windowBytes);
    const quint64 bufferStart = buffer.fileOffset;
    const quint64 bufferSize = static_cast<quint64>(qMax(0, buffer.bytes.size()));
    const quint64 clampedWindow = qMin(windowBytes, bufferSize);

    const int relStart = static_cast<int>(clampedStart - bufferStart);
    const int len = static_cast<int>(clampedWindow);
    window.start = clampedStart;
    window.data = buffer.bytes.mid(relStart, len);
    return window;
}

quint64 MainWindow::textViewportByteWindow() const {
    if (m_textView == nullptr) {
        return 1;
    }
    return static_cast<quint64>(qMax(1, m_textView->recommendedViewportByteCount()));
}

quint64 MainWindow::bitmapViewportByteWindow() const {
    if (m_bitmapView == nullptr) {
        return 1;
    }
    return qMax<quint64>(1, m_bitmapView->viewportByteCapacity());
}

MainWindow::ByteSpan MainWindow::centeredSpan(const ResultBuffer& buffer, quint64 centerOffset,
                                              quint64 desiredWindowBytes) const {
    ByteSpan span;
    const quint64 bufferSize = static_cast<quint64>(qMax(0, buffer.bytes.size()));
    if (bufferSize == 0) {
        span.start = buffer.fileOffset;
        span.size = 0;
        return span;
    }

    const quint64 bufferStart = buffer.fileOffset;
    const quint64 bufferEnd = bufferStart + bufferSize;
    const quint64 clampedCenter = qBound(bufferStart, centerOffset, bufferEnd - 1);
    const quint64 windowSize = qMax<quint64>(1, qMin(desiredWindowBytes, bufferSize));
    const quint64 before = windowSize / 2;

    quint64 start = (clampedCenter > before) ? (clampedCenter - before) : 0;
    if (start < bufferStart) {
        start = bufferStart;
    }
    const quint64 maxStart = bufferStart + (bufferSize - windowSize);
    if (start > maxStart) {
        start = maxStart;
    }

    span.start = start;
    span.size = windowSize;
    return span;
}

void MainWindow::requestSharedCenter(quint64 absoluteOffset) {
    m_pendingCenterOffset = absoluteOffset;
    scheduleSharedPreviewUpdate();
}

void MainWindow::shiftSharedCenterBy(qint64 signedBytes) {
    if (m_activePreviewRow < 0) {
        return;
    }
    const quint64 currentCenter =
        m_pendingCenterOffset.has_value() ? m_pendingCenterOffset.value() : m_sharedCenterOffset;
    quint64 nextCenter = currentCenter;
    if (signedBytes < 0) {
        const quint64 delta = static_cast<quint64>(-signedBytes);
        nextCenter = (delta >= nextCenter) ? 0 : (nextCenter - delta);
    } else if (signedBytes > 0) {
        nextCenter += static_cast<quint64>(signedBytes);
    }
    requestSharedCenter(nextCenter);
}

void MainWindow::scheduleSharedPreviewUpdate() {
    if (m_previewUpdateScheduled) {
        return;
    }
    m_previewUpdateScheduled = true;
    QMetaObject::invokeMethod(
        this,
        [this]() {
            m_previewUpdateScheduled = false;
            updateSharedPreviewNow();
        },
        Qt::QueuedConnection);
}

void MainWindow::updateSharedPreviewNow() {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: start activeRow=%1 rows=%2")
                           .arg(m_activePreviewRow)
                           .arg(m_resultModel.rowCount()));
    }
    if (m_activePreviewRow < 0 || m_activePreviewRow >= m_resultModel.rowCount()) {
        BRECO_SELTRACE("updateSharedPreviewNow: active row invalid, return");
        return;
    }
    const MatchRecord* match = m_resultModel.matchAt(m_activePreviewRow);
    if (match == nullptr) {
        BRECO_SELTRACE("updateSharedPreviewNow: match not found, return");
        return;
    }
    const quint64 ensureStartUs = debug::selectionTraceElapsedUs();
    const bool hasBuffer = ensureRowBufferLoaded(m_activePreviewRow, *match);
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: ensureRowBufferLoaded ok=%1 elapsed=%2us")
                           .arg(hasBuffer ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(debug::selectionTraceElapsedUs() - ensureStartUs));
    }
    if (!hasBuffer) {
        return;
    }
    if (m_activePreviewRow < 0 || m_activePreviewRow >= m_matchBufferIndices.size()) {
        BRECO_SELTRACE("updateSharedPreviewNow: active row no longer mapped, return");
        return;
    }
    const int bufferIndex = m_matchBufferIndices.at(m_activePreviewRow);
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: invalid bufferIndex=%1, return").arg(bufferIndex));
        return;
    }

    const ResultBuffer& backing = m_resultBuffers.at(bufferIndex);
    const quint64 backingSize = static_cast<quint64>(qMax(0, backing.bytes.size()));
    if (backingSize == 0) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: backing bufferIndex=%1 empty, return")
                           .arg(bufferIndex));
        return;
    }
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: using bufferIndex=%1 fileOffset=%2 size=%3")
                           .arg(bufferIndex)
                           .arg(backing.fileOffset)
                           .arg(backingSize));
    }

    quint64 center = m_pendingCenterOffset.has_value() ? m_pendingCenterOffset.value() : m_sharedCenterOffset;
    m_pendingCenterOffset.reset();
    center = qBound(backing.fileOffset, center, backing.fileOffset + backingSize - 1);
    m_sharedCenterOffset = center;
    BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: center=%1").arg(center));

    const ByteSpan textSpan = centeredSpan(backing, center, textViewportByteWindow());
    const ByteSpan bitmapSpan = centeredSpan(backing, center, bitmapViewportByteWindow());
    const quint64 textEnd = textSpan.start + textSpan.size;
    const quint64 bitmapEnd = bitmapSpan.start + bitmapSpan.size;
    const quint64 unionStart = qMin(textSpan.start, bitmapSpan.start);
    const quint64 unionEnd = qMax(textEnd, bitmapEnd);
    const quint64 unionSize = unionEnd - unionStart;
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral(
                           "updateSharedPreviewNow: spans text=[%1,+%2] bitmap=[%3,+%4] union=[%5,+%6]")
                           .arg(textSpan.start)
                           .arg(textSpan.size)
                           .arg(bitmapSpan.start)
                           .arg(bitmapSpan.size)
                           .arg(unionStart)
                           .arg(unionSize));
    }

    QByteArray textBytes;
    QByteArray bitmapBytes;
    textBytes.reserve(static_cast<int>(qMin<quint64>(
        textSpan.size, static_cast<quint64>(std::numeric_limits<int>::max()))));
    bitmapBytes.reserve(static_cast<int>(qMin<quint64>(
        bitmapSpan.size, static_cast<quint64>(std::numeric_limits<int>::max()))));

    const quint64 sliceStartUs = debug::selectionTraceElapsedUs();
    const int unionRelStart = static_cast<int>(unionStart - backing.fileOffset);
    for (quint64 i = 0; i < unionSize; ++i) {
        const quint64 absOffset = unionStart + i;
        const char byte = backing.bytes.at(unionRelStart + static_cast<int>(i));
        if (absOffset >= textSpan.start && absOffset < textEnd) {
            textBytes.push_back(byte);
        }
        if (absOffset >= bitmapSpan.start && absOffset < bitmapEnd) {
            bitmapBytes.push_back(byte);
        }
    }
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: sliced textBytes=%1 bitmapBytes=%2 elapsed=%3us")
                           .arg(textBytes.size())
                           .arg(bitmapBytes.size())
                           .arg(debug::selectionTraceElapsedUs() - sliceStartUs));
    }

    const quint64 termLen = static_cast<quint64>(m_scanController.searchTermLength());
    const QString filePath = filePathForTarget(match->scanTargetIdx);
    const std::optional<unsigned char> previousTextByte =
        previousByteBeforeViewport(backing, textSpan.start);

    BRECO_SELTRACE("updateSharedPreviewNow: begin widget updates");
    m_previewSyncInProgress = true;
    m_textView->setData(textBytes, textSpan.start, previousTextByte);
    m_textView->setMatchRange(match->offset, static_cast<quint32>(termLen));
    m_textView->setSelectedOffset(center, true);

    m_bitmapView->setData(bitmapBytes);
    m_bitmapView->setCenterAnchorOffset(center);
    m_bitmapView->setResultHighlight(match->offset, 0, static_cast<quint32>(termLen), 0,
                                     bitmapSpan.start);
    m_previewSyncInProgress = false;
    BRECO_SELTRACE("updateSharedPreviewNow: widget updates done");

    m_textHoverBuffer.filePath = filePath;
    m_textHoverBuffer.baseOffset = textSpan.start;
    m_textHoverBuffer.data = textBytes;
    m_bitmapHoverBuffer.filePath = filePath;
    m_bitmapHoverBuffer.baseOffset = bitmapSpan.start;
    m_bitmapHoverBuffer.data = bitmapBytes;
    BRECO_SELTRACE("updateSharedPreviewNow: hover buffers updated");
    BRECO_SELTRACE("updateSharedPreviewNow: done");
}

void MainWindow::showMatchPreview(int row, const MatchRecord& match) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("showMatchPreview: start row=%1 targetIdx=%2 offset=%3")
                           .arg(row)
                           .arg(match.scanTargetIdx)
                           .arg(match.offset));
    }
    if (row < 0 || row >= m_matchBufferIndices.size()) {
        BRECO_SELTRACE("showMatchPreview: row outside mapping, return");
        return;
    }

    const int bufferIndex = m_matchBufferIndices.at(row);
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        BRECO_SELTRACE(QStringLiteral("showMatchPreview: invalid bufferIndex=%1, return").arg(bufferIndex));
        return;
    }
    m_activePreviewRow = row;
    m_sharedCenterOffset = match.offset;
    m_pendingCenterOffset.reset();
    BRECO_SELTRACE(QStringLiteral("showMatchPreview: updateSharedPreviewNow begin bufferIndex=%1").arg(bufferIndex));
    updateSharedPreviewNow();
    BRECO_SELTRACE("showMatchPreview: updateSharedPreviewNow end");
}

void MainWindow::loadNotEmptyPreview() {
    if (m_sourceMode != SourceMode::SingleFile || m_scanTargets.size() != 1) {
        return;
    }

    const ScanTarget& target = m_scanTargets.first();
    if (target.fileSize == 0) {
        return;
    }

    const quint64 size = qMin<quint64>(kNotEmptyInitialBytes, target.fileSize);
    const ShiftSettings shift = currentShiftSettings();
    const auto transformed =
        m_windowLoader.loadTransformedWindow(target.filePath, target.fileSize, 0, size, shift);
    if (!transformed.has_value()) {
        return;
    }
    const QByteArray transformedBytes = transformed.value();
    if (transformedBytes.isEmpty()) {
        return;
    }

    MatchRecord synthetic;
    synthetic.scanTargetIdx = 0;
    synthetic.threadId = 0;
    synthetic.offset = 0;
    synthetic.searchTimeNs = 0;

    clearResultBufferCacheState();
    ResultBuffer rb;
    rb.scanTargetIdx = 0;
    rb.fileOffset = 0;
    rb.bytes = transformedBytes;
    m_resultBuffers.push_back(rb);
    m_matchBufferIndices = {0};
    m_resultShiftSettings = shift;
    m_hasResultShiftSettings = true;

    m_resultModel.clear();
    m_resultModel.appendBatch({synthetic});
    rebuildTargetMatchIntervals();
    selectResultRow(0);
}

void MainWindow::writeStatusLineToStdout(const QString& line) {
    if (line == m_lastStatusLineText) {
        return;
    }
    m_lastStatusLineText = line;
    std::cout << "[status] " << line.toStdString() << std::endl;
}

void MainWindow::updateStatusLineFromHover(const HoverBuffer& buffer, quint64 absoluteOffset) {
    if (buffer.filePath.isEmpty() || buffer.data.isEmpty()) {
        writeStatusLineToStdout(QStringLiteral("Hover: -"));
        return;
    }
    if (absoluteOffset < buffer.baseOffset ||
        absoluteOffset >= buffer.baseOffset + static_cast<quint64>(buffer.data.size())) {
        writeStatusLineToStdout(QStringLiteral("Hover: -"));
        return;
    }

    const int relativeIndex = static_cast<int>(absoluteOffset - buffer.baseOffset);
    writeStatusLineToStdout(
        formatHoverValueLine(buffer.filePath, absoluteOffset, buffer.data, relativeIndex));
}

QString MainWindow::formatHoverValueLine(const QString& filePath, quint64 absoluteOffset,
                                         const QByteArray& windowData,
                                         int relativeIndex) const {
    if (relativeIndex < 0 || relativeIndex >= windowData.size()) {
        return QStringLiteral("Hover: -");
    }

    const unsigned char b0 = static_cast<unsigned char>(windowData.at(relativeIndex));
    const QString ascii = printableAsciiChar(b0);
    const QString utf8 = utf8Glyph(windowData, relativeIndex);
    const QString utf16 = utf16Glyph(windowData, relativeIndex);

    bool ok8 = false;
    bool ok16 = false;
    bool ok32 = false;
    bool ok64 = false;
    const quint64 v8 = readUnsignedLittle(windowData, relativeIndex, 1, &ok8);
    const quint64 v16 = readUnsignedLittle(windowData, relativeIndex, 2, &ok16);
    const quint64 v32 = readUnsignedLittle(windowData, relativeIndex, 4, &ok32);
    const quint64 v64 = readUnsignedLittle(windowData, relativeIndex, 8, &ok64);

    const QString hex8 = ok8 ? formatHex(v8, 2) : QStringLiteral("n/a");
    const QString uhex8 = ok8 ? formatHex(v8, 2) : QStringLiteral("n/a");
    const QString hex16 = ok16 ? formatHex(v16, 4) : QStringLiteral("n/a");
    const QString uhex16 = ok16 ? formatHex(v16, 4) : QStringLiteral("n/a");
    const QString hex32 = ok32 ? formatHex(v32, 8) : QStringLiteral("n/a");
    const QString uhex32 = ok32 ? formatHex(v32, 8) : QStringLiteral("n/a");
    const QString hex64 = ok64 ? formatHex(v64, 16) : QStringLiteral("n/a");
    const QString uhex64 = ok64 ? formatHex(v64, 16) : QStringLiteral("n/a");

    QString floatStr = QStringLiteral("n/a");
    QString doubleStr = QStringLiteral("n/a");
    if (ok32) {
        float fv = 0.0f;
        const quint32 raw32 = static_cast<quint32>(v32 & 0xFFFFFFFFULL);
        std::memcpy(&fv, &raw32, sizeof(float));
        floatStr = QString::number(fv, 'g', 8);
    }
    if (ok64) {
        double dv = 0.0;
        std::memcpy(&dv, &v64, sizeof(double));
        doubleStr = QString::number(dv, 'g', 12);
    }

    return QStringLiteral(
               "%1 @%2 | ASCII:%3 UTF8:%4 UTF16:%5 | HEX8:%6 UHEX8:%7 HEX16:%8 UHEX16:%9 "
               "HEX32:%10 UHEX32:%11 HEX64:%12 UHEX64:%13 | float:%14 double:%15")
        .arg(filePath)
        .arg(absoluteOffset)
        .arg(ascii)
        .arg(utf8)
        .arg(utf16)
        .arg(hex8)
        .arg(uhex8)
        .arg(hex16)
        .arg(uhex16)
        .arg(hex32)
        .arg(uhex32)
        .arg(hex64)
        .arg(uhex64)
        .arg(floatStr)
        .arg(doubleStr);
}

void MainWindow::onTextHoverOffsetChanged(quint64 absoluteOffset) {
    m_bitmapView->setExternalHoverOffset(absoluteOffset);
    updateStatusLineFromHover(m_textHoverBuffer, absoluteOffset);
}

void MainWindow::onTextCenterAnchorRequested(quint64 absoluteOffset) {
    if (m_previewSyncInProgress) {
        return;
    }
    requestSharedCenter(absoluteOffset);
}

void MainWindow::onBitmapHoverOffsetChanged(quint64 absoluteOffset) {
    updateStatusLineFromHover(m_bitmapHoverBuffer, absoluteOffset);
}

void MainWindow::onBitmapByteClicked(quint64 absoluteOffset) {
    requestSharedCenter(absoluteOffset);
}

void MainWindow::onHoverLeft() {
    m_bitmapView->setExternalHoverOffset(std::nullopt);
    writeStatusLineToStdout(QStringLiteral("Hover: -"));
}

}  // namespace breco

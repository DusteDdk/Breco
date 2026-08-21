#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSize>
#include <QSplitter>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableView>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

#include <memory>
#include <utility>

#include "app/MainWindow.h"
#include "brecolang/gui/BrecoLangPanel.h"
#include "brecolang/gui/DecodedTreeModel.h"
#include "io/ProtectedSourceOpener.h"
#include "panel/CurrentByteInfoPanel.h"
#include "panel/DataViewByteAndBitmapPanel.h"
#include "panel/DataViewImagePanel.h"
#include "panel/DataViewShellPanel.h"
#include "panel/HexViewControlsPanel.h"
#include "panel/MainTabsPanel.h"
#include "panel/ResultsTablePanel.h"
#include "panel/ScanControlsPanel.h"
#include "view/TextViewWidget.h"

namespace {

class FakeProtectedSourceOpener final : public breco::ProtectedSourceOpener {
public:
    ~FakeProtectedSourceOpener() override {
#ifdef Q_OS_UNIX
        if (result.fd >= 0) {
            ::close(result.fd);
        }
#endif
    }

    bool isAvailable(const QString&, breco::ProtectedSourceKind) const override { return available; }

    breco::ProtectedOpenResult open(const QString&, breco::ProtectedSourceKind) override {
        ++openCount;
        breco::ProtectedOpenResult out = result;
        result.fd = -1;
        return out;
    }

    bool available = false;
    int openCount = 0;
    breco::ProtectedOpenResult result = breco::ProtectedOpenResult::unavailable();
};

class SettingsValueGuard {
public:
    explicit SettingsValueGuard(QString key) : m_key(std::move(key)) {
        QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
        m_existed = settings.contains(m_key);
        m_value = settings.value(m_key);
    }

    ~SettingsValueGuard() {
        QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
        if (m_existed) {
            settings.setValue(m_key, m_value);
        } else {
            settings.remove(m_key);
        }
    }

private:
    QString m_key;
    QVariant m_value;
    bool m_existed = false;
};

class MainWindowIntegrationTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void lifecycleCardLogsAndResetsPerScan();
    void selectingResultRowUpdatesPreviewBuffers();
    void twoColumnCompositionAndDataViewToolbar();
    void brecoLangPanelDecodesRealFileWithoutAutomaticExpandAll();
    void navigatorLabelsAndDataViewEndianFollowSelection();
    void hexViewDefaultsPersistAcrossWindows();
    void hexNavigatorEditsPreserveDeltaAndSelectionLength();
    void currentBytePanelShowsEndianAndWidthAwareValues();
    void shiftMarksCurrentBufferDirtyAndRestoresOnDeselect();
    void binarySelectionMenuContainsBothFileActions();
    void binaryRangeDialogClampsNumericLength();
    void binaryRangeWriterUsesShiftAndSixteenMiBChunks();
    void binarySaveProgressReportsCompletion();
    void sourcePathInputValidatesAndOpensTargets();
    void sourcePathAutocompleteKeepsTypingFocusAndLimitsSuggestions();
#ifdef Q_OS_UNIX
    void protectedSourceOpenElevatesAutomaticallyAndReportsFailures();
#endif
    void brecoLangRuleRunsThroughAsyncScanPipeline();
    void brecoLangScanStopRestoresTransientControls();
    void brecoLangLibraryMigrationAndStartupRestore();
    void imageModeScansAndJumpsToResult();
    void imageModeStopPreservesPartialResults();
    void imagePanelAnimatesGifAndHighlightsHover();
};

QByteArray makePngBytes() {
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(QColor(0xCC, 0x44, 0x22));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

void MainWindowIntegrationTests::initTestCase() {
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
}

void MainWindowIntegrationTests::brecoLangRuleRunsThroughAsyncScanPipeline() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("schema-scan.bin"));
    QFile input(inputPath);
    QVERIFY(input.open(QIODevice::WriteOnly));
    QCOMPARE(input.write(QByteArray::fromHex("0042004200")), 5LL);
    input.close();

    breco::MainWindow window;
    QVERIFY(window.selectSingleFileSource(inputPath));
    const QString source = QStringLiteral(R"BRECO(
language breco 0.1
inputs { input data { default } }
record Hit {
    identify { magic: u8 match magic == 0x42 else "not a hit" }
    commit
}
entry HitAt from data { hit: Hit }
default entry HitAt
)BRECO");
    QVERIFY(window.m_brecoLangPanel->loadSchemaText(source));
    QVERIFY(window.m_brecoLangPanel->setInputPath(u"data", inputPath));
    window.m_scanControlsPanel->searchTermLineEdit()->setText(
        QStringLiteral("remember me"));
    window.m_mainTabsPanel->activateTab(window.m_mainTabsPanel->brecoLangTab());
    window.m_brecoLangPanel->scanButton()->click();
    QVERIFY(window.m_scanController.isRunning());
    QCOMPARE(window.m_mainTabsPanel->mainTabWidget()->currentIndex(), 0);
    QCOMPARE(window.m_scanControlsPanel->searchTermLineEdit()->text(),
             QStringLiteral("BrecoLang: HitAt"));
    QVERIFY(!window.m_scanControlsPanel->searchTermLineEdit()->isEnabled());
    QCOMPARE(window.m_brecoLangPanel->scanButton()->text(),
             QStringLiteral("Stop Scan"));
    QTRY_VERIFY_WITH_TIMEOUT(!window.m_scanController.isRunning(), 10000);
    QCOMPARE(window.m_scanControlsPanel->searchTermLineEdit()->text(),
             QStringLiteral("remember me"));
    QVERIFY(window.m_scanControlsPanel->searchTermLineEdit()->isEnabled());
    QCOMPARE(window.m_brecoLangPanel->scanButton()->text(),
             QStringLiteral("Scan for Entry"));
    QCOMPARE(window.m_resultModel.rowCount(), 3);
    QCOMPARE(window.m_resultModel.matchAt(1)->offset, 1ULL);
    QCOMPARE(window.m_resultModel.matchAt(2)->offset, 3ULL);
}

void MainWindowIntegrationTests::brecoLangScanStopRestoresTransientControls() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("schema-stop.bin"));
    QFile input(inputPath);
    QVERIFY(input.open(QIODevice::WriteOnly));
    QByteArray data(2 * 1024 * 1024, '\0');
    data[0] = 0x42;
    QCOMPARE(input.write(data), data.size());
    input.close();

    breco::MainWindow window;
    QVERIFY(window.selectSingleFileSource(inputPath));
    QVERIFY(window.m_brecoLangPanel->loadSchemaText(QStringLiteral(R"BRECO(
language breco 0.1
inputs { input data { default } }
record Hit { identify { magic: u8 match magic == 0x42 else "no" } commit }
entry HitAt from data { hit: Hit }
default entry HitAt
)BRECO")));
    QVERIFY(window.m_brecoLangPanel->setInputPath(u"data", inputPath));
    window.m_scanControlsPanel->searchTermLineEdit()->setText(
        QStringLiteral("original"));
    window.m_scanControlsPanel->blockSizeSpin()->setValue(64);
    window.m_scanControlsPanel->blockSizeUnitCombo()->setCurrentIndex(1);
    window.m_brecoLangPanel->scanButton()->click();
    QVERIFY(window.m_scanController.isRunning());
    QTRY_VERIFY_WITH_TIMEOUT(window.m_resultModel.rowCount() > 0, 10000);
    window.m_brecoLangPanel->scanButton()->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.m_scanController.isRunning(), 10000);
    QCOMPARE(window.m_scanControlsPanel->searchTermLineEdit()->text(),
             QStringLiteral("original"));
    QVERIFY(window.m_scanControlsPanel->searchTermLineEdit()->isEnabled());
    QVERIFY(window.m_scanControlsPanel->ignoreCaseCheckBox()->isEnabled());
    QCOMPARE(window.m_brecoLangPanel->scanButton()->text(),
             QStringLiteral("Scan for Entry"));
}

void MainWindowIntegrationTests::lifecycleCardLogsAndResetsPerScan() {
    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    QVERIFY(window.m_scanControlsPanel != nullptr);
    QLabel* openLabel = window.m_scanControlsPanel->findChild<QLabel*>(
        QStringLiteral("sourcePathLabel"));
    QVERIFY(openLabel != nullptr);
    QCOMPARE(openLabel->text(), QStringLiteral("Open"));
    const auto scanLabels = window.m_scanControlsPanel->findChildren<QLabel*>();
    for (const QLabel* label : scanLabels) {
        QVERIFY(label->text() != QStringLiteral("Scan rule"));
        QVERIFY(label->text() != QStringLiteral("Alignment"));
    }
    QVERIFY(window.m_scanControlsPanel->lifecycleCard() != nullptr);
    QVERIFY(window.m_scanControlsPanel->lifecycleLogListWidget() != nullptr);
    QVERIFY(!window.m_scanControlsPanel->lifecycleCard()->isVisible());

    window.onScanStarted(3, 1024);
    const QString started = QStringLiteral(
        "[scan] started: files=3 totalBytes=1024 workers=8 blockSize=4096 prefillOnMerge=false");
    emit window.m_scanController.lifecycleMessage(started);
    QCoreApplication::processEvents();

    QVERIFY(window.m_scanControlsPanel->lifecycleCard()->isVisible());
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->count(), 1);
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->item(0)->text(),
             started);

    window.onResultsBatchReady({}, 5);
    emit window.m_scanController.lifecycleMessage(QStringLiteral("[scan] results found: 5"));
    QCoreApplication::processEvents();
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->count(), 2);
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->item(1)->text(),
             QStringLiteral("[scan] results found: 5"));

    emit window.m_scanController.lifecycleMessage(
        QStringLiteral("[scan] merging started: results=5"));
    emit window.m_scanController.lifecycleMessage(
        QStringLiteral("[scan] merging finished: matches=5 buffers=1"));
    emit window.m_scanController.lifecycleMessage(
        QStringLiteral("[scan] finished: stoppedByUser=false scannedBytes=1024 totalBytes=1024"));
    window.onScanFinished(false, false);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->count(), 5);
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->item(4)->text(),
             QStringLiteral("[scan] finished: stoppedByUser=false scannedBytes=1024 totalBytes=1024"));

    QVERIFY(window.statusBar() != nullptr);
    QVERIFY(window.statusBar()->currentMessage().startsWith(QStringLiteral("Current buffer:")));

    window.m_scanControlsPanel->hideLifecycleCardButton()->click();
    QCoreApplication::processEvents();
    QVERIFY(!window.m_scanControlsPanel->lifecycleCard()->isVisible());

    window.onScanStarted(1, 64);
    emit window.m_scanController.lifecycleMessage(QStringLiteral("[scan] started: next"));
    QCoreApplication::processEvents();
    QVERIFY(window.m_scanControlsPanel->lifecycleCard()->isVisible());
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->count(), 1);
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->item(0)->text(),
             QStringLiteral("[scan] started: next"));
}

void MainWindowIntegrationTests::selectingResultRowUpdatesPreviewBuffers() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("preview.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray bytes("ABCDEFGHIJKLMNO");
    QCOMPARE(f.write(bytes), bytes.size());
    f.close();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    breco::ScanTarget target;
    target.filePath = filePath;
    target.fileSize = static_cast<quint64>(bytes.size());
    window.m_scanTargets = {target};
    window.m_sourceMode = breco::MainWindow::SourceMode::SingleFile;

    breco::ResultBuffer buffer;
    buffer.scanTargetIdx = 0;
    buffer.fileOffset = 0;
    buffer.bytes = bytes;
    window.m_resultBuffers = {buffer};
    window.m_matchBufferIndices = {0};

    breco::MatchRecord match;
    match.scanTargetIdx = 0;
    match.threadId = 1;
    match.offset = 4;
    match.searchTimeNs = 1000;
    window.m_resultModel.clear();
    window.m_resultModel.appendBatch({match});
    window.rebuildTargetMatchIntervals();

    QTableView* table = window.m_resultsPanel->resultsTableView();
    QVERIFY(table != nullptr);
    table->selectRow(0);
    QCoreApplication::processEvents();

    QCOMPARE(window.m_activePreviewRow, 0);
    QVERIFY(!window.m_textHoverBuffer.data.isEmpty());
    QVERIFY(!window.m_bitmapHoverBuffer.data.isEmpty());
    QVERIFY(window.m_sharedCenterOffset >= match.offset);
}

void MainWindowIntegrationTests::twoColumnCompositionAndDataViewToolbar() {
    QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
    settings.remove(QStringLiteral("ui/dataViewImageMaxPixelsK"));
    settings.remove(QStringLiteral("ui/dataViewImageMaxResults"));
    settings.remove(QStringLiteral("ui/dataViewImageJobs"));

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    auto* mainSplitter = window.findChild<QSplitter*>(QStringLiteral("mainSplitter"));
    QVERIFY(mainSplitter != nullptr);
    QCOMPARE(mainSplitter->count(), 2);
    QVERIFY(window.m_hexControlsPanel != nullptr);
    QVERIFY(window.m_textView != nullptr);
    QVERIFY(window.m_rawDataViewShellPanel != nullptr);
    QVERIFY(window.m_dataViewByteAndBitmapPanel != nullptr);
    QVERIFY(window.m_dataViewImagePanel != nullptr);
    QVERIFY(window.m_brecoLangPanel != nullptr);
    QTabWidget* tabs = window.m_mainTabsPanel->mainTabWidget();
    QCOMPARE(tabs->count(), 4);
    QCOMPARE(tabs->tabText(0), QStringLiteral("Scan"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("Raw"));
    QCOMPARE(tabs->tabText(2), QStringLiteral("BrecoLang"));
    QCOMPARE(tabs->tabText(3), QStringLiteral("Image"));
    QCOMPARE(tabs->indexOf(window.m_mainTabsPanel->rawDataTab()), 1);
    QCOMPARE(tabs->indexOf(window.m_mainTabsPanel->brecoLangTab()), 2);
    QCOMPARE(tabs->indexOf(window.m_mainTabsPanel->imageDataTab()), 3);
    QVERIFY(window.m_rawDataViewShellPanel->bodyHost()->isAncestorOf(
        window.m_dataViewByteAndBitmapPanel));
    QVERIFY(window.m_mainTabsPanel->brecoLangHost()->isAncestorOf(
        window.m_brecoLangPanel));
    QVERIFY(window.m_mainTabsPanel->imageDataHost()->isAncestorOf(
        window.m_dataViewImagePanel));
    QVERIFY(!window.m_rawDataViewShellPanel->bitmapModeComboBox()->isHidden());
    QVERIFY(!window.m_rawDataViewShellPanel->zoomInButton()->isHidden());

    for (QWidget* page : {window.m_mainTabsPanel->rawDataTab(),
                          window.m_mainTabsPanel->brecoLangTab(),
                          window.m_mainTabsPanel->imageDataTab()}) {
        QVERIFY(window.m_mainTabsPanel->detachTab(tabs->indexOf(page)));
        QVERIFY(window.m_mainTabsPanel->isTabDetached(page));
        QVERIFY(window.m_mainTabsPanel->detachedWindow(page) != nullptr);
    }
    QCOMPARE(tabs->count(), 1);
    window.m_mainTabsPanel->detachedWindow(window.m_mainTabsPanel->imageDataTab())->close();
    QCoreApplication::processEvents();
    window.m_mainTabsPanel->detachedWindow(window.m_mainTabsPanel->rawDataTab())->close();
    QCoreApplication::processEvents();
    window.m_mainTabsPanel->detachedWindow(window.m_mainTabsPanel->brecoLangTab())->close();
    QCoreApplication::processEvents();
    QCOMPARE(tabs->count(), 4);
    QCOMPARE(tabs->tabText(0), QStringLiteral("Scan"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("Raw"));
    QCOMPARE(tabs->tabText(2), QStringLiteral("BrecoLang"));
    QCOMPARE(tabs->tabText(3), QStringLiteral("Image"));

    QCOMPARE(window.m_dataViewImagePanel->maxPixelsKSpinBox()->value(), 4096);
    QCOMPARE(window.m_dataViewImagePanel->maxResultsSpinBox()->value(), 5);
    QCOMPARE(window.m_dataViewImagePanel->jobsSpinBox()->value(),
             qMin(256, qMax(1, QThread::idealThreadCount())));
    QVERIFY(!window.m_dataViewImagePanel->resultsProgressBar()->isHidden());

    window.m_dataViewImagePanel->maxResultsSpinBox()->setValue(0);
    QCoreApplication::processEvents();
    QVERIFY(window.m_dataViewImagePanel->resultsProgressBar()->isHidden());
    window.m_dataViewImagePanel->jobsSpinBox()->setValue(1);
    QCoreApplication::processEvents();
    breco::MainWindow restoredWindow;
    restoredWindow.show();
    QCoreApplication::processEvents();
    QCOMPARE(restoredWindow.m_dataViewImagePanel->jobsSpinBox()->value(), 1);

    QVERIFY(!window.m_rawDataViewShellPanel->bitmapModeComboBox()->isHidden());
    QVERIFY(!window.m_rawDataViewShellPanel->zoomInButton()->isHidden());
}

void MainWindowIntegrationTests::brecoLangPanelDecodesRealFileWithoutAutomaticExpandAll() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString schemaPath = directory.filePath(QStringLiteral("packet.breco"));
    const QString inputPath = directory.filePath(QStringLiteral("packet.bin"));

    QFile schema(schemaPath);
    QVERIFY(schema.open(QIODevice::WriteOnly));
    const QByteArray schemaText = R"BRECO(
language breco 0.1
inputs { input data { default } }
record Packet { marker: u8 length: u16le }
entry Inspect from data { packet: Packet preserve remaining as tail }
default entry Inspect
outform Summary(root: Inspect) text { emit "${root.packet.marker}" }
)BRECO";
    QCOMPARE(schema.write(schemaText), static_cast<qint64>(schemaText.size()));
    schema.close();

    QFile input(inputPath);
    QVERIFY(input.open(QIODevice::WriteOnly));
    QCOMPARE(input.write(QByteArray::fromHex("7e0400aabb")), 5LL);
    input.close();

    breco::MainWindow window;
    QVERIFY(window.m_brecoLangPanel != nullptr);
    QVERIFY(window.m_brecoLangPanel->loadSchemaFile(schemaPath));
    QVERIFY(window.m_brecoLangPanel->setInputPath(u"data", inputPath));
    QVERIFY(window.m_brecoLangPanel->selectEntry(u"Inspect"));
    QVERIFY2(window.m_brecoLangPanel->decodeSelected(),
             qPrintable(window.m_brecoLangPanel->statusText()));

    auto* model = window.m_brecoLangPanel->treeModel();
    QTreeView* view = window.m_brecoLangPanel->treeView();
    QCOMPARE(model->rowCount(), 1);
    const QModelIndex root = model->index(0, 0);
    QVERIFY(root.isValid());
    QVERIFY(view->isExpanded(root));
    const QModelIndex packet = model->index(0, 0, root);
    QVERIFY(packet.isValid());
    QVERIFY(!view->isExpanded(packet));
    window.m_brecoLangPanel->expandAllButton()->click();
    QVERIFY(view->isExpanded(packet));
    QVERIFY(window.m_brecoLangPanel->statusText().contains(
        QStringLiteral("Decoded 5 bytes")));

    QVERIFY(window.m_brecoLangPanel->pinCurrentView());
    QCOMPARE(window.m_brecoLangPanel->viewTabs()->count(), 2);
    QBuffer json;
    QVERIFY(json.open(QIODevice::WriteOnly));
    QString error;
    QVERIFY2(window.m_brecoLangPanel->exportJson(&json, &error),
             qPrintable(error));
    QVERIFY(json.data().contains("\"marker\":126"));

    QBuffer binary;
    QVERIFY(binary.open(QIODevice::WriteOnly));
    QVERIFY2(window.m_brecoLangPanel->exportBinary(&binary, &error),
             qPrintable(error));
    QCOMPARE(binary.data(), QByteArray::fromHex("7e0400aabb"));

    QBuffer outform;
    QVERIFY(outform.open(QIODevice::WriteOnly));
    QVERIFY2(window.m_brecoLangPanel->renderOutform(u"Summary", &outform,
                                                     &error),
             qPrintable(error));
    QCOMPARE(outform.data(), QByteArray("126"));
}

void MainWindowIntegrationTests::navigatorLabelsAndDataViewEndianFollowSelection() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("labels.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray bytes("HELLO WORLD");
    QCOMPARE(f.write(bytes), bytes.size());
    f.close();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    breco::ScanTarget target;
    target.filePath = filePath;
    target.fileSize = static_cast<quint64>(bytes.size());
    window.m_scanTargets = {target};
    window.m_sourceMode = breco::MainWindow::SourceMode::SingleFile;
    window.m_shiftValueSpin->setValue(0);

    breco::ResultBuffer buffer;
    buffer.scanTargetIdx = 0;
    buffer.fileOffset = 0;
    buffer.bytes = bytes;
    window.m_resultBuffers = {buffer};
    window.m_matchBufferIndices = {0};

    breco::MatchRecord match;
    match.scanTargetIdx = 0;
    match.threadId = 1;
    match.offset = 1;
    match.searchTimeNs = 1;
    window.m_resultModel.clear();
    window.m_resultModel.appendBatch({match});
    window.rebuildTargetMatchIntervals();

    window.showMatchPreview(0, match);
    QCoreApplication::processEvents();

    QCOMPARE(window.m_hexControlsPanel->fileNameValueLabel()->text(), QStringLiteral("labels.bin"));
    QCOMPARE(window.m_hexControlsPanel->offsetValueEdit()->text(), QStringLiteral("0X0"));
    QVERIFY(window.m_hexControlsPanel->fileSizeValueLabel()->text().contains(QStringLiteral("B")));

    window.m_activeTextSelectionRange = qMakePair<quint64, quint64>(1, 4);
    window.updateHexInfoPanel();
    window.refreshDataViewFromNavigator();
    QCoreApplication::processEvents();
    QCOMPARE(window.m_hexControlsPanel->selectedValueEdit()->text(),
             QStringLiteral("0X1 (+3 bytes)"));
    QCOMPARE(window.m_hexControlsPanel->selectToValueEdit()->text(),
             QStringLiteral("0X3"));
    QCOMPARE(window.m_currentByteInfoPanel->asciiValueLabel()->text(), QStringLiteral("E"));

    window.m_rawDataViewShellPanel->bigEndianRadioButton()->setChecked(true);
    QCoreApplication::processEvents();
    QVERIFY(window.m_currentByteInfoPanel->bigEndianCheckBox()->isChecked());
}

void MainWindowIntegrationTests::hexViewDefaultsPersistAcrossWindows() {
    SettingsValueGuard showAsGuard(QStringLiteral("ui/hexShowAsIndex"));
    SettingsValueGuard byteLineGuard(QStringLiteral("ui/textByteLineModeIndex"));
    SettingsValueGuard legacyByteModeGuard(QStringLiteral("ui/textByteModeEnabled"));
    QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
    settings.remove(QStringLiteral("ui/hexShowAsIndex"));
    settings.remove(QStringLiteral("ui/textByteLineModeIndex"));
    settings.remove(QStringLiteral("ui/textByteModeEnabled"));

    {
        breco::MainWindow window;
        QCOMPARE(window.m_hexControlsPanel->showAsComboBox()->currentText(),
                 QStringLiteral("Classic"));
        QCOMPARE(window.m_hexControlsPanel->bytesPerLineComboBox()->currentText(),
                 QStringLiteral("16"));
        window.m_hexControlsPanel->showAsComboBox()->setCurrentIndex(2);
        window.m_hexControlsPanel->bytesPerLineComboBox()->setCurrentIndex(3);
    }

    {
        breco::MainWindow restored;
        QCOMPARE(restored.m_hexControlsPanel->showAsComboBox()->currentIndex(), 2);
        QCOMPARE(restored.m_hexControlsPanel->bytesPerLineComboBox()->currentIndex(), 3);
    }
}

void MainWindowIntegrationTests::hexNavigatorEditsPreserveDeltaAndSelectionLength() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("navigator-edits.bin"));
    QByteArray bytes(256, '\0');
    for (int i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>(i);
    }
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    window.m_hexControlsPanel->showAsComboBox()->setCurrentIndex(4);
    window.m_hexControlsPanel->bytesPerLineComboBox()->setCurrentIndex(1);

    breco::ScanTarget target;
    target.filePath = filePath;
    target.fileSize = static_cast<quint64>(bytes.size());
    window.m_scanTargets = {target};
    window.m_sourceMode = breco::MainWindow::SourceMode::SingleFile;

    breco::ResultBuffer buffer;
    buffer.scanTargetIdx = 0;
    buffer.fileOffset = 0;
    buffer.bytes = bytes;
    window.m_resultBuffers = {buffer};
    window.m_matchBufferIndices = {0};

    breco::MatchRecord match;
    match.scanTargetIdx = 0;
    match.threadId = 1;
    match.offset = 0;
    match.searchTimeNs = 1;
    window.m_resultModel.clear();
    window.m_resultModel.appendBatch({match});
    window.rebuildTargetMatchIntervals();
    window.showMatchPreview(0, match);
    QVERIFY(window.m_textView->setSelectionRange(10, 13));
    QCoreApplication::processEvents();

    QLineEdit* offset = window.m_hexControlsPanel->offsetValueEdit();
    QLineEdit* selected = window.m_hexControlsPanel->selectedValueEdit();
    QLineEdit* selectTo = window.m_hexControlsPanel->selectToValueEdit();
    QCOMPARE(offset->text(), QStringLiteral("0X0"));
    QCOMPARE(selected->text(), QStringLiteral("0XA (+3 bytes)"));
    QCOMPARE(selectTo->text(), QStringLiteral("0XC"));

    offset->setFocus();
    offset->selectAll();
    QTest::keyClicks(offset, QStringLiteral("20"));
    QTest::keyClick(offset, Qt::Key_Return);
    QCoreApplication::processEvents();
    QCOMPARE(offset->text(), QStringLiteral("0X14"));
    QCOMPARE(selected->text(), QStringLiteral("0X1E (+3 bytes)"));
    QCOMPARE(selectTo->text(), QStringLiteral("0X20"));

    selected->setFocus();
    selected->selectAll();
    QTest::keyClicks(selected, QStringLiteral("0x42"));
    QTest::keyClick(selected, Qt::Key_Return);
    QCoreApplication::processEvents();
    QCOMPARE(offset->text(), QStringLiteral("0X38"));
    QCOMPARE(selected->text(), QStringLiteral("0X42 (+3 bytes)"));
    QCOMPARE(selectTo->text(), QStringLiteral("0X44"));

    selectTo->setFocus();
    selectTo->selectAll();
    QTest::keyClicks(selectTo, QStringLiteral("0x48"));
    QTest::keyClick(selectTo, Qt::Key_Return);
    QCoreApplication::processEvents();
    QCOMPARE(offset->text(), QStringLiteral("0X38"));
    QCOMPARE(selected->text(), QStringLiteral("0X42 (+7 bytes)"));
    QCOMPARE(selectTo->text(), QStringLiteral("0X48"));

    offset->setFocus();
    offset->selectAll();
    QTest::keyClicks(offset, QStringLiteral("99"));
    window.m_hexControlsPanel->showAsComboBox()->setFocus();
    QCoreApplication::processEvents();
    QCOMPARE(offset->text(), QStringLiteral("0X38"));
    QCOMPARE(selected->text(), QStringLiteral("0X42 (+7 bytes)"));

    QVERIFY(window.navigateHexView(5, std::nullopt));
    QVERIFY(selected->text().isEmpty());
    QVERIFY(selectTo->text().isEmpty());
    QVERIFY(!selected->isEnabled());
    QVERIFY(!selectTo->isEnabled());
    offset->setFocus();
    offset->selectAll();
    QTest::keyClicks(offset, QStringLiteral("25"));
    QTest::keyClick(offset, Qt::Key_Return);
    QCoreApplication::processEvents();
    QCOMPARE(offset->text(), QStringLiteral("0X19"));
    QVERIFY(selected->text().isEmpty());
    QVERIFY(selectTo->text().isEmpty());

    QVERIFY(window.navigateHexView(30, qMakePair<quint64, quint64>(35, 36)));
    QCOMPARE(selected->text(), QStringLiteral("0X23"));
    QCOMPARE(selectTo->text(), QStringLiteral("0X23"));
    selectTo->setFocus();
    selectTo->selectAll();
    QTest::keyClicks(selectTo, QStringLiteral("0x22"));
    QTest::keyClick(selectTo, Qt::Key_Return);
    QCoreApplication::processEvents();
    QCOMPARE(selected->text(), QStringLiteral("0X23"));
    QCOMPARE(selectTo->text(), QStringLiteral("0X23"));
}

void MainWindowIntegrationTests::currentBytePanelShowsEndianAndWidthAwareValues() {
    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    breco::MainWindow::HoverBuffer hover;
    hover.filePath = QStringLiteral("in-memory");
    hover.baseOffset = 100;
    hover.data = QByteArray::fromHex("4100FF");

    window.m_currentByteInfoPanel->bigEndianCheckBox()->setChecked(true);
    window.m_currentByteInfoPanel->decimalModeRadioButton()->setChecked(true);
    QCoreApplication::processEvents();
    window.updateCurrentByteInfoFromHover(hover, 100);
    QCoreApplication::processEvents();

    QCOMPARE(window.m_currentByteInfoPanel->asciiValueLabel()->text(), QStringLiteral("A"));
    QCOMPARE(window.m_currentByteInfoPanel->u8ValueLabel()->text(), QStringLiteral("65"));
    QCOMPARE(window.m_currentByteInfoPanel->u16ValueLabel()->text(), QStringLiteral("16640"));
    QCOMPARE(window.m_currentByteInfoPanel->u32ValueLabel()->text(), QStringLiteral("n/a"));
    QCOMPARE(window.m_currentByteInfoPanel->u64ValueLabel()->text(), QStringLiteral("n/a"));
    QCOMPARE(window.m_currentByteInfoPanel->byteInterpretationLargeLabel()->text(),
             QString::fromUtf8("\xE4\x84\x80"));
    QCOMPARE(window.m_currentByteInfoPanel->hexStr8BytesValueLabel()->text(),
             QStringLiteral("0 x 41 00 FF -- -- -- -- --"));

    window.m_currentByteInfoPanel->bigEndianCheckBox()->setChecked(false);
    window.updateCurrentByteInfoFromHover(hover, 100);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_currentByteInfoPanel->byteInterpretationLargeLabel()->text(), QStringLiteral("A"));

    window.updateCurrentByteInfoFromHover(hover, 101);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_currentByteInfoPanel->asciiValueLabel()->text(), QStringLiteral("."));
    QCOMPARE(window.m_currentByteInfoPanel->u16ValueLabel()->text(), QStringLiteral("65280"));

    hover.data = QByteArray::fromHex("0041D83DDE00");
    window.m_currentByteInfoPanel->bigEndianCheckBox()->setChecked(true);
    window.updateCurrentByteInfoFromHover(hover, 100);
    QCOMPARE(window.m_currentByteInfoPanel->byteInterpretationLargeLabel()->text(), QStringLiteral("A"));
    window.updateCurrentByteInfoFromHover(hover, 102);
    QCOMPARE(window.m_currentByteInfoPanel->byteInterpretationLargeLabel()->text(),
             QString::fromUtf8("\xF0\x9F\x98\x80"));
    QCOMPARE(window.m_currentByteInfoPanel->utf16ValueLabel()->text(),
             QString::fromUtf8("\xF0\x9F\x98\x80"));

    hover.data = QByteArray::fromHex("41003DD800DE00D8");
    window.m_currentByteInfoPanel->bigEndianCheckBox()->setChecked(false);
    window.updateCurrentByteInfoFromHover(hover, 100);
    QCOMPARE(window.m_currentByteInfoPanel->byteInterpretationLargeLabel()->text(), QStringLiteral("A"));
    window.updateCurrentByteInfoFromHover(hover, 102);
    QCOMPARE(window.m_currentByteInfoPanel->byteInterpretationLargeLabel()->text(),
             QString::fromUtf8("\xF0\x9F\x98\x80"));
    window.updateCurrentByteInfoFromHover(hover, 106);
    QCOMPARE(window.m_currentByteInfoPanel->byteInterpretationLargeLabel()->text(), QStringLiteral("-"));
    QCOMPARE(window.m_currentByteInfoPanel->utf16ValueLabel()->text(), QStringLiteral("n/a"));
}

void MainWindowIntegrationTests::shiftMarksCurrentBufferDirtyAndRestoresOnDeselect() {
    QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
    settings.remove(QStringLiteral("ui/hexShiftBitsValue"));
    settings.setValue(QStringLiteral("ui/hexShiftBitsValue"), 42);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("dirty-buffer.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray bytes = QByteArray::fromHex("112233445566");
    QCOMPARE(f.write(bytes), bytes.size());
    f.close();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    QCOMPARE(window.m_shiftValueSpin->value(), 0);

    breco::ScanTarget target;
    target.filePath = filePath;
    target.fileSize = static_cast<quint64>(bytes.size());
    window.m_scanTargets = {target};
    window.m_sourceMode = breco::MainWindow::SourceMode::SingleFile;

    breco::ResultBuffer buffer;
    buffer.scanTargetIdx = 0;
    buffer.fileOffset = 0;
    buffer.bytes = bytes;
    buffer.dirty = false;
    window.m_resultBuffers = {buffer};
    window.m_matchBufferIndices = {0};

    breco::MatchRecord match;
    match.scanTargetIdx = 0;
    match.threadId = 1;
    match.offset = 2;
    match.searchTimeNs = 1;
    window.m_resultModel.clear();
    window.m_resultModel.appendBatch({match});
    window.rebuildTargetMatchIntervals();

    QVERIFY(window.m_shiftValueSpin != nullptr);
    window.m_shiftValueSpin->setValue(1);
    QCoreApplication::processEvents();

    window.showMatchPreview(0, match);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_activePreviewRow, 0);
    QVERIFY(window.m_resultBuffers.at(0).dirty);
    QVERIFY(window.m_resultBuffers.at(0).bytes != bytes);

    window.onResultActivated(QModelIndex());
    QCoreApplication::processEvents();
    QCOMPARE(window.m_activePreviewRow, -1);
    QVERIFY(!window.m_resultBuffers.at(0).dirty);
    QCOMPARE(window.m_resultBuffers.at(0).bytes, bytes);
    settings.remove(QStringLiteral("ui/hexShiftBitsValue"));
}

void MainWindowIntegrationTests::binarySelectionMenuContainsBothFileActions() {
    breco::TextViewWidget view;
    view.resize(800, 320);
    view.setDisplayMode(breco::TextDisplayMode::ByteMode);
    view.setData(QByteArray::fromHex("0011223344556677"), 0);
    view.show();
    QCoreApplication::processEvents();

    QWidget* content = view.findChild<QWidget*>(QStringLiteral("textViewContent"));
    QVERIFY(content != nullptr);
    QTest::mouseClick(content, Qt::LeftButton, Qt::NoModifier, QPoint(12, 12));

    QStringList actionLabels;
    bool sawCopyMenu = false;
    QTimer::singleShot(0, &view, [&]() {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr) {
            return;
        }
        for (QAction* action : menu->actions()) {
            if (action->text() != QStringLiteral("Copy") || action->menu() == nullptr) {
                continue;
            }
            sawCopyMenu = true;
            for (QAction* childAction : action->menu()->actions()) {
                actionLabels.push_back(childAction->text());
            }
        }
        menu->close();
    });
    QTimer::singleShot(250, &view, []() {
        if (QWidget* popup = QApplication::activePopupWidget(); popup != nullptr) {
            popup->close();
        }
    });
    QTest::mouseClick(content, Qt::RightButton, Qt::NoModifier, QPoint(12, 12));

    QVERIFY(sawCopyMenu);
    const QStringList expectedLabels = {
        QStringLiteral("Text only"), QStringLiteral("Offset + Hex"), QStringLiteral("Hex"),
        QStringLiteral("C Header"), QStringLiteral("Binary"),
        QStringLiteral("Binary (from here)"),
    };
    QCOMPARE(actionLabels, expectedLabels);
}

void MainWindowIntegrationTests::binaryRangeDialogClampsNumericLength() {
    breco::MainWindow window;
    bool inspected = false;
    QString observedTitle;
    QString observedUntilEndText;
    QStringList observedUnits;
    bool untilEndWasChecked = false;
    QTimer::singleShot(0, &window, [&]() {
        QDialog* dialog =
            window.findChild<QDialog*>(QStringLiteral("saveRangeAsBinaryDialog"));
        if (dialog == nullptr) {
            return;
        }
        inspected = true;
        observedTitle = dialog->windowTitle();

        auto* untilEnd =
            dialog->findChild<QRadioButton*>(QStringLiteral("saveRangeUntilEndRadio"));
        auto* numeric =
            dialog->findChild<QRadioButton*>(QStringLiteral("saveRangeNumericRadio"));
        auto* value = dialog->findChild<QLineEdit*>(QStringLiteral("saveRangeValueInput"));
        auto* unit = dialog->findChild<QComboBox*>(QStringLiteral("saveRangeUnitCombo"));
        auto* buttons =
            dialog->findChild<QDialogButtonBox*>(QStringLiteral("saveRangeButtons"));
        if (untilEnd == nullptr || numeric == nullptr || value == nullptr || unit == nullptr ||
            buttons == nullptr) {
            dialog->reject();
            return;
        }
        untilEndWasChecked = untilEnd->isChecked();
        observedUntilEndText = untilEnd->text();
        for (int i = 0; i < unit->count(); ++i) {
            observedUnits.push_back(unit->itemText(i));
        }

        numeric->setChecked(true);
        value->setText(QStringLiteral("2"));
        unit->setCurrentIndex(1);
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    QTimer::singleShot(250, &window, [&window]() {
        if (QDialog* dialog =
                window.findChild<QDialog*>(QStringLiteral("saveRangeAsBinaryDialog"));
            dialog != nullptr) {
            dialog->reject();
        }
    });

    const std::optional<quint64> length = window.promptBinaryRangeLength(1536);
    QVERIFY(inspected);
    QCOMPARE(observedTitle, QStringLiteral("Save range as binary"));
    QVERIFY(untilEndWasChecked);
    QCOMPARE(observedUntilEndText, QStringLiteral("Until end of file (1.50 KiB)"));
    const QStringList expectedUnits = {
        QStringLiteral("Bytes"), QStringLiteral("KiB"), QStringLiteral("MiB"),
        QStringLiteral("GiB"), QStringLiteral("TiB"),
    };
    QCOMPARE(observedUnits, expectedUnits);
    QVERIFY(length.has_value());
    QCOMPARE(length.value(), 1536ULL);
    QCOMPARE(breco::MainWindow::binaryLengthFromInput(0.5, 1, 4096), 512ULL);
    QCOMPARE(breco::MainWindow::binaryProgressText(16ULL * 1024ULL * 1024ULL,
                                                   20ULL * 1024ULL * 1024ULL),
             QStringLiteral("16.00 MiB / 20.00 MiB"));
}

void MainWindowIntegrationTests::binaryRangeWriterUsesShiftAndSixteenMiBChunks() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    constexpr qsizetype chunkSize = 16 * 1024 * 1024;
    QByteArray sourceBytes(chunkSize + 4, '\0');
    for (qsizetype i = 0; i < sourceBytes.size(); ++i) {
        sourceBytes[i] = static_cast<char>((i * 37 + 11) & 0xFF);
    }

    const QString sourcePath = tempDir.filePath(QStringLiteral("binary-source.bin"));
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    QCOMPARE(sourceFile.write(sourceBytes), sourceBytes.size());
    sourceFile.close();

    const QString outputPath = tempDir.filePath(QStringLiteral("binary-output.bin"));
    breco::ScanTarget target;
    target.filePath = sourcePath;
    target.fileSize = static_cast<quint64>(sourceBytes.size());
    const quint64 startOffset = 1;
    const quint64 length = static_cast<quint64>(chunkSize) + 2ULL;
    QVector<quint64> updates;
    QString errorMessage;

    breco::MainWindow window;
    const bool saved = window.writeBinaryRangeToFile(
        outputPath, target, startOffset, length,
        breco::ShiftSettings{1, breco::ShiftUnit::Bits}, &errorMessage,
        [&](quint64 written) { updates.push_back(written); });
    QVERIFY2(saved, qPrintable(errorMessage));
    const QVector<quint64> expectedUpdates = {static_cast<quint64>(chunkSize), length};
    QCOMPARE(updates, expectedUpdates);

    QFile outputFile(outputPath);
    QVERIFY(outputFile.open(QIODevice::ReadOnly));
    QCOMPARE(static_cast<quint64>(outputFile.size()), length);
    const QByteArray outputBytes = outputFile.readAll();
    QCOMPARE(static_cast<quint64>(outputBytes.size()), length);

    const auto expectedByteAt = [&](quint64 outputRelativeOffset) {
        const quint64 sourceIndex = startOffset + outputRelativeOffset;
        const unsigned char current =
            static_cast<unsigned char>(sourceBytes.at(static_cast<qsizetype>(sourceIndex)));
        const unsigned char next =
            static_cast<unsigned char>(sourceBytes.at(static_cast<qsizetype>(sourceIndex + 1ULL)));
        return static_cast<char>((current << 1U) | (next >> 7U));
    };
    for (const quint64 sample : {0ULL, static_cast<quint64>(chunkSize) - 1ULL,
                                 static_cast<quint64>(chunkSize), length - 1ULL}) {
        QCOMPARE(outputBytes.at(static_cast<qsizetype>(sample)), expectedByteAt(sample));
    }
}

void MainWindowIntegrationTests::binarySaveProgressReportsCompletion() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QByteArray sourceBytes = QByteArray::fromHex("00112233445566778899AABBCCDDEEFF");
    const QString sourcePath = tempDir.filePath(QStringLiteral("progress-source.bin"));
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    QCOMPARE(sourceFile.write(sourceBytes), sourceBytes.size());
    sourceFile.close();

    breco::ScanTarget target;
    target.filePath = sourcePath;
    target.fileSize = static_cast<quint64>(sourceBytes.size());
    const QString outputPath = tempDir.filePath(QStringLiteral("progress-output.bin"));

    breco::MainWindow window;
    bool sawProgress = false;
    bool sawFileSaved = false;
    QTimer completionPoll;
    completionPoll.setInterval(5);
    QObject::connect(&completionPoll, &QTimer::timeout, &window, [&]() {
        QDialog* dialog =
            window.findChild<QDialog*>(QStringLiteral("binarySaveProgressDialog"));
        if (dialog == nullptr) {
            return;
        }
        auto* label =
            dialog->findChild<QLabel*>(QStringLiteral("binarySaveStatusLabel"));
        auto* buttons =
            dialog->findChild<QDialogButtonBox*>(QStringLiteral("binarySaveButtons"));
        if (label != nullptr && label->text() == QStringLiteral("File saved") &&
            buttons != nullptr) {
            sawFileSaved = true;
            completionPoll.stop();
            buttons->button(QDialogButtonBox::Ok)->click();
        }
    });
    QTimer::singleShot(0, &window, [&]() {
        QDialog* dialog =
            window.findChild<QDialog*>(QStringLiteral("binarySaveProgressDialog"));
        auto* progress = dialog != nullptr
                             ? dialog->findChild<QProgressBar*>(
                                   QStringLiteral("binarySaveProgressBar"))
                             : nullptr;
        sawProgress = progress != nullptr && progress->isVisible() &&
                      progress->format() == QStringLiteral("0 Bytes / 8 Bytes");
        completionPoll.start();
    });
    QTimer::singleShot(1000, &window, [&window]() {
        if (QDialog* dialog =
                window.findChild<QDialog*>(QStringLiteral("binarySaveProgressDialog"));
            dialog != nullptr) {
            dialog->reject();
        }
    });

    window.saveBinaryRangeWithProgress(outputPath, target, 4, 8);
    QVERIFY(sawProgress);
    QVERIFY(sawFileSaved);

    QFile outputFile(outputPath);
    QVERIFY(outputFile.open(QIODevice::ReadOnly));
    QCOMPARE(outputFile.readAll(), sourceBytes.mid(4, 8));
}

void MainWindowIntegrationTests::sourcePathInputValidatesAndOpensTargets() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("source.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray bytes("SOURCE");
    QCOMPARE(f.write(bytes), bytes.size());
    f.close();

    const QString childPath = tempDir.filePath(QStringLiteral("child.bin"));
    QFile child(childPath);
    QVERIFY(child.open(QIODevice::WriteOnly));
    QCOMPARE(child.write(QByteArray("DIR")), 3);
    child.close();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    auto noElevation = std::make_unique<FakeProtectedSourceOpener>();
    noElevation->available = false;
    window.setProtectedSourceOpenerForTests(std::move(noElevation));

    QLineEdit* sourcePathEdit = window.m_scanControlsPanel->sourcePathLineEdit();
    QLabel* sourceTypeIcon = window.m_scanControlsPanel->selectedSourceTypeIconLabel();
    QVERIFY(sourcePathEdit != nullptr);
    QVERIFY(sourceTypeIcon != nullptr);
    QSize expectedIconSize;
    for (const QString& iconPath :
         {QStringLiteral(":/res/none.png"), QStringLiteral(":/res/file.png"),
          QStringLiteral(":/res/dev.png"), QStringLiteral(":/res/dir.png")}) {
        expectedIconSize = expectedIconSize.expandedTo(QPixmap(iconPath).size());
    }
    QCOMPARE(sourceTypeIcon->minimumSize(), expectedIconSize);
    QCOMPARE(sourceTypeIcon->maximumSize(), expectedIconSize);
    QCOMPARE(window.m_scanControlsPanel->openFileButton()->toolTip(), QStringLiteral("Select file"));
    QCOMPARE(window.m_scanControlsPanel->openDirButton()->toolTip(), QStringLiteral("Select directory"));

    const QString missingPath = tempDir.filePath(QStringLiteral("missing.bin"));
    sourcePathEdit->setText(missingPath);
    window.validateSourcePathInput();
    QCoreApplication::processEvents();
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::None);
    QCOMPARE(window.m_scanTargets.size(), 0);
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Not found: %1").arg(missingPath));
    QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("#fff3a3")));
    QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("No source selected"));

    sourcePathEdit->setText(filePath);
    window.validateSourcePathInput();
    QCoreApplication::processEvents();
    const QString absoluteFilePath = QFileInfo(filePath).absoluteFilePath();
    QCOMPARE(sourcePathEdit->text(), filePath);
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::None);
    QCOMPARE(window.m_scanTargets.size(), 0);
    QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("background-color: white")));
    QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("File"));

    QVERIFY(window.applySourcePath(filePath, true));
    QCoreApplication::processEvents();
    QCOMPARE(sourcePathEdit->text(), absoluteFilePath);
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::SingleFile);
    QCOMPARE(window.m_scanTargets.size(), 1);
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Open: %1").arg(absoluteFilePath));
    QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("#c8f7c5")));
    QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("File"));

#ifdef Q_OS_UNIX
    QFile denied(filePath);
    QVERIFY(QFile::setPermissions(filePath, QFileDevice::Permissions()));
    const bool deniedCanOpen = denied.open(QIODevice::ReadOnly);
    if (deniedCanOpen) {
        denied.close();
    } else {
        sourcePathEdit->setText(filePath);
        window.validateSourcePathInput();
        QCoreApplication::processEvents();
        QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::None);
        QCOMPARE(window.m_scanTargets.size(), 0);
        QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("background-color: white")));
        QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("File"));

        QVERIFY(!window.applySourcePath(filePath, true));
        QCoreApplication::processEvents();
        QCOMPARE(window.statusBar()->currentMessage(),
                 QStringLiteral("Permission denied: %1").arg(absoluteFilePath));
        QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("#ffc9c9")));
        QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("File"));
    }
    QVERIFY(QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
#endif

    sourcePathEdit->setText(tempDir.path());
    window.validateSourcePathInput();
    QCoreApplication::processEvents();
    const QString absoluteDirPath = QFileInfo(tempDir.path()).absoluteFilePath();
    QCOMPARE(sourcePathEdit->text(), tempDir.path());
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::None);
    QCOMPARE(window.m_scanTargets.size(), 0);
    QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("background-color: white")));
    QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("Directory"));

    QVERIFY(window.applySourcePath(tempDir.path(), true));
    QCoreApplication::processEvents();
    QCOMPARE(sourcePathEdit->text(), absoluteDirPath);
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::Directory);
    QVERIFY(window.m_scanTargets.size() >= 1);
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Open: %1").arg(absoluteDirPath));
    QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("#c8f7c5")));
    QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("Directory"));
}

void MainWindowIntegrationTests::sourcePathAutocompleteKeepsTypingFocusAndLimitsSuggestions() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    for (const QString& name :
         {QStringLiteral("match"), QStringLiteral("matcher"), QStringLiteral("matchbox"),
          QStringLiteral("matching"), QStringLiteral("matchstick"), QStringLiteral("matchwork")}) {
        QFile file(tempDir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
    }
    const QString matchingDirectory = tempDir.filePath(QStringLiteral("matchdir"));
    QVERIFY(QDir().mkdir(matchingDirectory));

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    QLineEdit* sourcePathEdit = window.m_scanControlsPanel->sourcePathLineEdit();
    QVERIFY(sourcePathEdit != nullptr);
    QCompleter* completer = sourcePathEdit->completer();
    QVERIFY(completer != nullptr);
    QCOMPARE(completer->maxVisibleItems(), 5);
    QCOMPARE(completer->popup()->focusPolicy(), Qt::NoFocus);

    sourcePathEdit->setFocus();
    QTRY_VERIFY(sourcePathEdit->hasFocus());
    const QString prefix = QDir(tempDir.path()).filePath(QStringLiteral("mat"));
    sourcePathEdit->setText(prefix);
    emit sourcePathEdit->textEdited(prefix);
    QTRY_COMPARE(completer->completionModel()->rowCount(), 5);
    QTRY_VERIFY(completer->popup()->isVisible());
    QVERIFY(sourcePathEdit->hasFocus());

    const QString narrowedPrefix =
        QDir(tempDir.path()).filePath(QStringLiteral("matc"));
    sourcePathEdit->setText(narrowedPrefix);
    emit sourcePathEdit->textEdited(narrowedPrefix);
    QCOMPARE(sourcePathEdit->text(), narrowedPrefix);
    QTRY_COMPARE(completer->completionModel()->rowCount(), 5);
    QVERIFY(sourcePathEdit->hasFocus());
    QCOMPARE(completer->completionModel()->index(0, 0).data().toString(),
             QDir::toNativeSeparators(tempDir.filePath(QStringLiteral("match"))));
    bool includesDirectory = false;
    for (int row = 0; row < completer->completionModel()->rowCount(); ++row) {
        includesDirectory |=
            completer->completionModel()->index(row, 0).data().toString() ==
            QDir::toNativeSeparators(matchingDirectory + QStringLiteral("/"));
    }
    QVERIFY(includesDirectory);

    QTest::keyClick(sourcePathEdit, Qt::Key_Down);
    QCoreApplication::processEvents();
    const QModelIndex afterDown = completer->popup()->currentIndex();
    QVERIFY(afterDown.isValid());
    QTest::keyClick(sourcePathEdit, Qt::Key_Up);
    QCoreApplication::processEvents();
    const QModelIndex selectedIndex = completer->popup()->currentIndex();
    QVERIFY(selectedIndex.isValid());
    QVERIFY(selectedIndex.row() <= afterDown.row());
    const QString selectedPath = selectedIndex.data().toString();
    QTest::keyClick(sourcePathEdit, Qt::Key_Enter);
    QTRY_COMPARE(sourcePathEdit->text(), selectedPath);
    QVERIFY(sourcePathEdit->hasFocus());
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::None);
}

#ifdef Q_OS_UNIX
void MainWindowIntegrationTests::protectedSourceOpenElevatesAutomaticallyAndReportsFailures() {
    SettingsValueGuard sourcePathGuard(
        QStringLiteral("ui/rememberedSingleFilePath"));
    SettingsValueGuard sourceOffsetGuard(
        QStringLiteral("ui/rememberedSingleFileOffset"));

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("protected.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray bytes("PROTECTED");
    QCOMPARE(f.write(bytes), bytes.size());
    f.close();
    const QString absolutePath = QFileInfo(filePath).absoluteFilePath();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    auto unavailable = std::make_unique<FakeProtectedSourceOpener>();
    FakeProtectedSourceOpener* unavailablePtr = unavailable.get();
    unavailablePtr->available = false;
    window.setProtectedSourceOpenerForTests(std::move(unavailable));
    QVERIFY(!window.tryOpenProtectedSource(absolutePath, breco::MainWindow::SourceTargetKind::File));
    QCOMPARE(unavailablePtr->openCount, 0);

    auto failed = std::make_unique<FakeProtectedSourceOpener>();
    FakeProtectedSourceOpener* failedPtr = failed.get();
    failedPtr->available = true;
    failedPtr->result = breco::ProtectedOpenResult::failed(QStringLiteral("nope"));
    window.setProtectedSourceOpenerForTests(std::move(failed));
    QVERIFY(!window.tryOpenProtectedSource(absolutePath, breco::MainWindow::SourceTargetKind::File));
    QCOMPARE(failedPtr->openCount, 1);
    QCOMPARE(window.statusBar()->currentMessage(),
             QStringLiteral("Could not open %1 with elevated permissions: nope").arg(absolutePath));

    auto opaqueFailure = std::make_unique<FakeProtectedSourceOpener>();
    FakeProtectedSourceOpener* opaqueFailurePtr = opaqueFailure.get();
    opaqueFailurePtr->available = true;
    opaqueFailurePtr->result = breco::ProtectedOpenResult::failed(
        QStringLiteral("org.freedesktop.UDisks2.Error.NotAuthorized"));
    window.setProtectedSourceOpenerForTests(std::move(opaqueFailure));
    QVERIFY(!window.tryOpenProtectedSource(absolutePath, breco::MainWindow::SourceTargetKind::File));
    QCOMPARE(opaqueFailurePtr->openCount, 1);
    QCOMPARE(window.statusBar()->currentMessage(),
             QStringLiteral("Could not open %1 with elevated permissions: authorization was denied")
                 .arg(absolutePath));

    const int fd = ::open(absolutePath.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    QVERIFY(fd >= 0);
    auto opened = std::make_unique<FakeProtectedSourceOpener>();
    FakeProtectedSourceOpener* openedPtr = opened.get();
    openedPtr->available = true;
    openedPtr->result =
        breco::ProtectedOpenResult::opened(fd, static_cast<quint64>(bytes.size()));
    window.setProtectedSourceOpenerForTests(std::move(opened));
    QVERIFY(window.tryOpenProtectedSource(absolutePath, breco::MainWindow::SourceTargetKind::File));
    QCOMPARE(openedPtr->openCount, 1);
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::SingleFile);
    QCOMPARE(window.m_scanTargets.size(), 1);
    QCOMPARE(window.m_scanTargets.first().filePath, absolutePath);
    QCOMPARE(window.m_scanTargets.first().fileSize, static_cast<quint64>(bytes.size()));
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Open: %1").arg(absolutePath));

    const auto chunk = window.m_filePool.readChunk(absolutePath, 0, static_cast<quint64>(bytes.size()));
    QVERIFY(chunk.has_value());
    QCOMPARE(chunk.value(), bytes);

    QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
    QVERIFY(!settings.contains(QStringLiteral("ui/rememberedSingleFilePath")));
    QVERIFY(!settings.contains(QStringLiteral("ui/rememberedSingleFileOffset")));
}
#endif

void MainWindowIntegrationTests::brecoLangLibraryMigrationAndStartupRestore() {
    SettingsValueGuard schemaGuard(QStringLiteral("ui/lastBrecoLangSchemaPath"));
    SettingsValueGuard libraryGuard(
        QStringLiteral("ui/brecoLangLibraryDirectory"));
    QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
    settings.remove(QStringLiteral("ui/lastBrecoLangSchemaPath"));
    settings.remove(QStringLiteral("ui/brecoLangLibraryDirectory"));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString schemaPath = directory.filePath(QStringLiteral("packet.breco"));
    QFile schema(schemaPath);
    QVERIFY(schema.open(QIODevice::WriteOnly));
    const QByteArray source = QByteArrayLiteral(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Inspect from data { value: u8 }
default entry Inspect
)BRECO");
    QCOMPARE(schema.write(source), source.size());
    schema.close();
    const QString olderPath =
        directory.filePath(QStringLiteral("needs-migration.breco") +
                           QStringLiteral("struct"));
    QFile older(olderPath);
    QVERIFY(older.open(QIODevice::WriteOnly));
    QCOMPARE(older.write("kept"), 4LL);
    older.close();

    {
        breco::MainWindow window;
        window.m_brecoLangPanel->setLibraryDirectory(directory.path());
        QVERIFY(window.m_brecoLangPanel->migrationNoticeText().contains(
            QStringLiteral("manual migration")));
        QVERIFY(window.m_brecoLangPanel->migrationNoticeText().contains(
            QStringLiteral("needs-migration.breco") + QStringLiteral("struct")));
        QVERIFY(window.m_brecoLangPanel->loadSchemaFile(schemaPath));
    }
    QVERIFY(QFileInfo::exists(olderPath));
    QCOMPARE(settings.value(QStringLiteral("ui/lastBrecoLangSchemaPath"))
                 .toString(),
             QFileInfo(schemaPath).absoluteFilePath());
    QCOMPARE(settings.value(QStringLiteral("ui/brecoLangLibraryDirectory"))
                 .toString(),
             QFileInfo(directory.path()).absoluteFilePath());

    breco::MainWindow restored;
    QVERIFY(restored.m_brecoLangPanel->program() != nullptr);
    QCOMPARE(restored.m_brecoLangPanel->libraryDirectory(),
             QFileInfo(directory.path()).absoluteFilePath());
    QVERIFY(restored.m_brecoLangPanel->migrationNoticeText().contains(
        QStringLiteral("needs-migration.breco") + QStringLiteral("struct")));
}

void MainWindowIntegrationTests::imageModeScansAndJumpsToResult() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("embedded-image.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray png = makePngBytes();
    const quint64 pngOffset = 16ULL * 1024ULL * 1024ULL + 4096ULL;
    QByteArray prefix(1024 * 1024, '\0');
    quint64 written = 0;
    while (written < pngOffset) {
        const quint64 remaining = pngOffset - written;
        const qsizetype chunkSize =
            static_cast<qsizetype>(qMin<quint64>(remaining, static_cast<quint64>(prefix.size())));
        QCOMPARE(f.write(prefix.constData(), chunkSize), chunkSize);
        written += static_cast<quint64>(chunkSize);
    }
    QCOMPARE(f.write(png), png.size());
    f.close();
    const QString absolutePath = QFileInfo(filePath).absoluteFilePath();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    QVERIFY(window.selectSourcePath(absolutePath));
    QCoreApplication::processEvents();
    QVERIFY(window.m_activePreviewRow >= 0);
    QVERIFY(window.m_textHoverBuffer.baseOffset <= 1024);

    window.m_mainTabsPanel->activateTab(window.m_mainTabsPanel->imageDataTab());
    window.m_dataViewImagePanel->setSelectedFormats(breco::EmbeddedImageFormat::Png);
    window.m_dataViewImagePanel->setSelectedScope(breco::EmbeddedImageScope::FromStart);
    window.m_dataViewImagePanel->maxPixelsKSpinBox()->setValue(4);
    window.m_dataViewImagePanel->maxResultsSpinBox()->setValue(5);
    QCoreApplication::processEvents();

    window.m_dataViewImagePanel->scanButton()->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.m_dataViewImagePanel->resultCount(), 1, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(window.m_dataViewImagePanel->scanButton()->text(),
                              QStringLiteral("Scan"), 10000);
    QCOMPARE(window.m_dataViewImagePanel->fileProgressBar()->value(), 1000);
    QVERIFY(window.m_dataViewImagePanel->fileProgressBar()->format().contains(QStringLiteral(" / ")));
    QVERIFY(window.m_dataViewImagePanel->fileProgressBar()->format().contains(QStringLiteral("MiB")));
    QVERIFY(window.m_dataViewImagePanel->fileProgressBar()->format().contains(
        QStringLiteral("( Disk: ")));
    QVERIFY(window.m_dataViewImagePanel->fileProgressBar()->format().endsWith(
        QStringLiteral("100.00 %")));
    QVERIFY(!window.m_dataViewImagePanel->resultsProgressBar()->isHidden());
    QCOMPARE(window.m_dataViewImagePanel->resultsProgressBar()->format(), QStringLiteral("1 / 5"));

    QLabel* imageLabel = nullptr;
    bool sawNumberedTitle = false;
    const auto labels = window.m_dataViewImagePanel->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text().startsWith(QStringLiteral("Image: 1"))) {
            sawNumberedTitle = true;
        }
        if (imageLabel == nullptr && label->toolTip().contains(QStringLiteral("Click to jump"))) {
            imageLabel = label;
        }
    }
    QVERIFY(sawNumberedTitle);
    QVERIFY(imageLabel != nullptr);
    QTest::mouseClick(imageLabel, Qt::LeftButton, Qt::NoModifier, imageLabel->rect().center());
    QCoreApplication::processEvents();

    QCOMPARE(window.m_sharedCenterOffset, pngOffset);
    QVERIFY(window.m_textHoverBuffer.baseOffset <= pngOffset);
    QVERIFY(pngOffset < window.m_textHoverBuffer.baseOffset +
                           static_cast<quint64>(window.m_textHoverBuffer.data.size()));
}

void MainWindowIntegrationTests::imageModeStopPreservesPartialResults() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("stop-image-scan.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray png = makePngBytes();
    QCOMPARE(f.write(png), png.size());
    QVERIFY(f.resize(512LL * 1024LL * 1024LL));
    f.close();
    const QString absolutePath = QFileInfo(filePath).absoluteFilePath();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    QVERIFY(window.selectSourcePath(absolutePath));
    QCoreApplication::processEvents();

    window.m_mainTabsPanel->activateTab(window.m_mainTabsPanel->imageDataTab());
    window.m_dataViewImagePanel->setSelectedFormats(breco::EmbeddedImageFormat::Png);
    window.m_dataViewImagePanel->setSelectedScope(breco::EmbeddedImageScope::FromStart);
    window.m_dataViewImagePanel->jobsSpinBox()->setValue(1);
    window.m_dataViewImagePanel->maxPixelsKSpinBox()->setValue(4);
    window.m_dataViewImagePanel->maxResultsSpinBox()->setValue(0);
    QCoreApplication::processEvents();
    QVERIFY(window.m_dataViewImagePanel->resultsProgressBar()->isHidden());

    window.m_dataViewImagePanel->scanButton()->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.m_dataViewImagePanel->resultCount(), 1, 10000);
    QCOMPARE(window.m_dataViewImagePanel->scanButton()->text(), QStringLiteral("Stop"));

    window.m_dataViewImagePanel->scanButton()->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.m_dataViewImagePanel->scanButton()->text(),
                              QStringLiteral("Scan"), 10000);
    QCOMPARE(window.m_dataViewImagePanel->resultCount(), 1);
    QVERIFY(window.m_dataViewImagePanel->statusLabel()->text().contains(QStringLiteral("cancelled")));
}

void MainWindowIntegrationTests::imagePanelAnimatesGifAndHighlightsHover() {
    breco::DataViewImagePanel panel;
    panel.resize(420, 320);
    panel.show();

    QImage firstFrame(2, 2, QImage::Format_ARGB32);
    firstFrame.fill(Qt::red);
    QImage secondFrame(2, 2, QImage::Format_ARGB32);
    secondFrame.fill(Qt::blue);

    breco::EmbeddedImageResult result;
    result.offset = 42;
    result.format = breco::EmbeddedImageFormat::Gif;
    result.formatName = QStringLiteral("GIF");
    result.size = firstFrame.size();
    result.image = firstFrame;
    result.encodedData = QByteArray("GIF89a animated payload");
    result.animationFrames = {firstFrame, secondFrame};
    result.frameDelaysMs = {16, 16};
    panel.addResult(result);
    QCoreApplication::processEvents();

    QLabel* title = panel.findChild<QLabel*>(QStringLiteral("imageResultTitle"));
    QLabel* preview = panel.findChild<QLabel*>(QStringLiteral("imagePreviewLabel"));
    QFrame* card = panel.findChild<QFrame*>(QStringLiteral("imageResultCard"));
    QTimer* timer = panel.findChild<QTimer*>(QStringLiteral("imageAnimationTimer"));
    QVERIFY(title != nullptr);
    QVERIFY(preview != nullptr);
    QVERIFY(card != nullptr);
    QVERIFY(timer != nullptr);
    QVERIFY(title->text().contains(QStringLiteral("2 frames")));
    QVERIFY(preview->toolTip().contains(QStringLiteral("Right-click to save")));
    QVERIFY(timer->isActive());
    QCOMPARE(timer->interval(), 16);

    const QColor initialColor = preview->pixmap().toImage().pixelColor(0, 0);
    QTRY_VERIFY_WITH_TIMEOUT(preview->pixmap().toImage().pixelColor(0, 0) != initialColor, 1000);

    QEnterEvent enterEvent(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QCoreApplication::sendEvent(preview, &enterEvent);
    QVERIFY(card->autoFillBackground());
    QEvent leaveEvent(QEvent::Leave);
    QCoreApplication::sendEvent(preview, &leaveEvent);
    QVERIFY(!card->autoFillBackground());
}

}  // namespace

QTEST_MAIN(MainWindowIntegrationTests)
#include "mainwindow_integration_tests.moc"

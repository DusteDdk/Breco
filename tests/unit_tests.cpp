#include <QApplication>
#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QToolTip>

#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

#include "image/EmbeddedImageScanner.h"
#include "io/FileEnumerator.h"
#include "io/OpenFilePool.h"
#include "io/ShiftedWindowLoader.h"
#include "model/ResultModel.h"
#include "scan/MatchUtils.h"
#include "scan/ScanProgress.h"
#include "scan/SpscQueue.h"
#include "scan/ShiftTransform.h"
#include "text/StringModeRules.h"
#include "text/TextSequenceAnalyzer.h"
#include "view/BitmapViewWidget.h"
#include "view/TextViewWidget.h"

namespace {

int g_failures = 0;

void expectTrue(bool condition, const QString& message) {
    if (!condition) {
        qCritical().noquote() << QStringLiteral("FAIL: %1").arg(message);
        ++g_failures;
    }
}

void expectEqInt(int actual, int expected, const QString& message) {
    if (actual != expected) {
        qCritical().noquote()
            << QStringLiteral("FAIL: %1 (actual=%2 expected=%3)")
                   .arg(message)
                   .arg(actual)
                   .arg(expected);
        ++g_failures;
    }
}

void expectEqQString(const QString& actual, const QString& expected, const QString& message) {
    if (actual != expected) {
        qCritical().noquote()
            << QStringLiteral("FAIL: %1 (actual='%2' expected='%3')")
                   .arg(message)
                   .arg(actual)
                   .arg(expected);
        ++g_failures;
    }
}

void testMatchUtilsIndexOf() {
    const QByteArray haystack("abCDxy");
    const QByteArray needle("cd");

    expectEqInt(breco::MatchUtils::indexOf(haystack, needle, 0, breco::TextInterpretationMode::Ascii,
                                           false),
                -1, QStringLiteral("MatchUtils exact search should be case-sensitive"));

    expectEqInt(breco::MatchUtils::indexOf(haystack, needle, 0, breco::TextInterpretationMode::Ascii,
                                           true),
                2, QStringLiteral("MatchUtils ignoreCase should fold ASCII bytes"));

    expectEqInt(breco::MatchUtils::indexOf(haystack, needle, 0, breco::TextInterpretationMode::Utf16,
                                           true),
                -1, QStringLiteral("MatchUtils UTF-16 mode should bypass ignoreCase fold"));

    expectEqInt(breco::MatchUtils::indexOf(haystack, QByteArray(), 0,
                                           breco::TextInterpretationMode::Ascii, true),
                -1, QStringLiteral("MatchUtils ignoreCase path should reject empty needle"));
}

void testShiftReadPlan() {
    {
        const breco::ShiftSettings shift{0, breco::ShiftUnit::Bytes};
        const breco::ShiftReadPlan plan = breco::ShiftTransform::makeReadPlan(2, 4, 10, shift);
        expectEqInt(static_cast<int>(plan.readStart), 2,
                    QStringLiteral("ShiftReadPlan shift=0 readStart"));
        expectEqInt(static_cast<int>(plan.readSize), 4,
                    QStringLiteral("ShiftReadPlan shift=0 readSize"));
    }

    {
        const breco::ShiftSettings shift{3, breco::ShiftUnit::Bytes};
        const breco::ShiftReadPlan plan = breco::ShiftTransform::makeReadPlan(2, 4, 10, shift);
        expectEqInt(static_cast<int>(plan.readStart), 5,
                    QStringLiteral("ShiftReadPlan byte+shift readStart"));
        expectEqInt(static_cast<int>(plan.readSize), 4,
                    QStringLiteral("ShiftReadPlan byte+shift readSize"));
    }

    {
        const breco::ShiftSettings shift{-5, breco::ShiftUnit::Bytes};
        const breco::ShiftReadPlan plan = breco::ShiftTransform::makeReadPlan(0, 4, 10, shift);
        expectEqInt(static_cast<int>(plan.readSize), 0,
                    QStringLiteral("ShiftReadPlan fully out-of-range byte shift should read zero"));
    }

    {
        const breco::ShiftSettings shift{-1, breco::ShiftUnit::Bits};
        const breco::ShiftReadPlan plan = breco::ShiftTransform::makeReadPlan(0, 1, 2, shift);
        expectEqInt(static_cast<int>(plan.readStart), 0,
                    QStringLiteral("ShiftReadPlan bit-shift should clamp negative min byte"));
        expectEqInt(static_cast<int>(plan.readSize), 1,
                    QStringLiteral("ShiftReadPlan bit-shift clamp size"));
    }
}

void testShiftTransformWindow() {
    {
        const QByteArray raw = QByteArray::fromHex("112233");
        const breco::ShiftSettings shift{1, breco::ShiftUnit::Bytes};
        const QByteArray out =
            breco::ShiftTransform::transformWindow(raw, 0, 0, 3, 3, shift);
        expectEqInt(static_cast<unsigned char>(out.at(0)), 0x22,
                    QStringLiteral("ShiftTransform byte shift +1 first byte"));
        expectEqInt(static_cast<unsigned char>(out.at(1)), 0x33,
                    QStringLiteral("ShiftTransform byte shift +1 second byte"));
        expectEqInt(static_cast<unsigned char>(out.at(2)), 0x00,
                    QStringLiteral("ShiftTransform byte shift +1 zero fill"));
    }

    {
        const QByteArray raw = QByteArray::fromHex("8000");
        const breco::ShiftSettings shift{-1, breco::ShiftUnit::Bits};
        const QByteArray out =
            breco::ShiftTransform::transformWindow(raw, 0, 0, 1, 2, shift);
        expectEqInt(static_cast<unsigned char>(out.at(0)), 0x40,
                    QStringLiteral("ShiftTransform bit shift -1 should move top bit right"));
    }
}

void testTextSequenceAnalyzer() {
    {
        const QByteArray bytes("HELLO");
        const breco::TextAnalysisResult result =
            breco::TextSequenceAnalyzer::analyze(bytes, breco::TextInterpretationMode::Ascii);
        expectEqInt(result.sequences.size(), 1,
                    QStringLiteral("Text analyzer should detect >=5-byte valid sequence"));
        expectEqInt(result.sequences.at(0).startIndex, 0,
                    QStringLiteral("Text analyzer sequence start"));
        expectEqInt(result.sequences.at(0).endIndex, 5,
                    QStringLiteral("Text analyzer sequence end"));
        expectTrue(result.classes.at(0) == breco::TextByteClass::Printable,
                   QStringLiteral("Text analyzer printable class"));
    }

    {
        const QByteArray bytes = QByteArray::fromHex("414200");  // AB\0
        const breco::TextAnalysisResult result =
            breco::TextSequenceAnalyzer::analyze(bytes, breco::TextInterpretationMode::Ascii);
        expectEqInt(result.sequences.size(), 1,
                    QStringLiteral("Text analyzer should detect 2-byte sequence followed by NUL"));
    }

    {
        const QByteArray bytes = QByteArray::fromHex("01");
        const breco::TextAnalysisResult result =
            breco::TextSequenceAnalyzer::analyze(bytes, breco::TextInterpretationMode::Ascii);
        expectTrue(result.classes.at(0) == breco::TextByteClass::Invalid,
                   QStringLiteral("Text analyzer should mark non-printable byte invalid"));
    }

    {
        const QByteArray bytes = QByteArray::fromHex("C3A400");  // UTF-8 'ä' followed by NUL
        const breco::TextAnalysisResult result =
            breco::TextSequenceAnalyzer::analyze(bytes, breco::TextInterpretationMode::Utf8);
        expectEqInt(result.sequences.size(), 1,
                    QStringLiteral("UTF-8 valid 2-byte sequence followed by NUL should qualify"));
    }

    {
        const QByteArray beUtf16 =
            QByteArray::fromHex("D83DDE00D83DDE00D83DDE00D83DDE00D83DDE00");
        const breco::TextAnalysisResult big =
            breco::TextSequenceAnalyzer::analyze(beUtf16, breco::TextInterpretationMode::Utf16,
                                                 false);
        expectEqInt(big.sequences.size(), 1,
                    QStringLiteral("Explicit big-endian UTF-16 should decode BE text"));
        const QString decodedBig = breco::TextSequenceAnalyzer::decodeRange(
            QByteArray::fromHex("0041"), 0, 2, breco::TextInterpretationMode::Utf16, false);
        const QString decodedLittle = breco::TextSequenceAnalyzer::decodeRange(
            QByteArray::fromHex("0041"), 0, 2, breco::TextInterpretationMode::Utf16, true);
        expectEqQString(decodedBig, QStringLiteral("A"),
                        QStringLiteral("Explicit big-endian UTF-16 decodeRange"));
        expectTrue(decodedLittle != QStringLiteral("A"),
                   QStringLiteral("Explicit little-endian UTF-16 decodeRange should differ for BE bytes"));
    }
}

void testTextViewStringsOnlyFiltering() {
    breco::TextViewWidget widget;
    widget.resize(320, 160);
    widget.show();
    QCoreApplication::processEvents();

    widget.setMode(breco::TextInterpretationMode::Ascii);
    widget.setDisplayMode(breco::TextDisplayMode::StringMode);
    widget.setStringsOnlyEnabled(true);
    widget.setData(QByteArray::fromHex("010248454C4C4F0304"), 100);
    QCoreApplication::processEvents();

    expectEqInt(widget.visibleByteCount(), 5,
                QStringLiteral("Strings-only text view should hide bytes outside sequences"));
    const std::optional<quint64> firstVisible = widget.firstVisibleByteOffset();
    expectTrue(firstVisible.has_value() && firstVisible.value() == 102ULL,
               QStringLiteral("Strings-only filtering should preserve absolute offsets"));
}

void testStringModeNullVisibilityRule() {
    const QString guardNote = QStringLiteral(
        "Behavior guard: changing StringMode NUL visibility requires asking the supervisor and "
        "getting explicit permission first.");

    expectTrue(!breco::shouldRenderStringModeNull(std::nullopt),
               QStringLiteral("StringMode NUL requires previous byte. %1").arg(guardNote));
    expectTrue(!breco::shouldRenderStringModeNull(static_cast<unsigned char>(0x00U)),
               QStringLiteral("StringMode NUL must not follow NUL. %1").arg(guardNote));
    expectTrue(!breco::shouldRenderStringModeNull(static_cast<unsigned char>(0x01U)),
               QStringLiteral("StringMode NUL must not follow non-printed byte. %1").arg(guardNote));
    expectTrue(breco::shouldRenderStringModeNull(static_cast<unsigned char>('A')),
               QStringLiteral("StringMode NUL must render after printable ASCII. %1").arg(guardNote));
    expectTrue(breco::shouldRenderStringModeNull(static_cast<unsigned char>('\n')),
               QStringLiteral("StringMode NUL must render after LF. %1").arg(guardNote));
    expectTrue(breco::shouldRenderStringModeNull(static_cast<unsigned char>('\r')),
               QStringLiteral("StringMode NUL must render after CR. %1").arg(guardNote));

    const QByteArray bytes = QByteArray::fromHex("410000420043");
    const QVector<bool> mask = breco::buildStringModeVisibilityMask(
        bytes, static_cast<unsigned char>('\n'));
    expectEqInt(mask.size(), bytes.size(),
                QStringLiteral("Visibility mask size must match bytes. %1").arg(guardNote));
    if (mask.size() == bytes.size()) {
        expectTrue(mask.at(0), QStringLiteral("41 visible. %1").arg(guardNote));
        expectTrue(mask.at(1), QStringLiteral("first 00 visible after printable. %1").arg(guardNote));
        expectTrue(!mask.at(2), QStringLiteral("second 00 hidden after 00. %1").arg(guardNote));
        expectTrue(mask.at(3), QStringLiteral("42 visible. %1").arg(guardNote));
        expectTrue(mask.at(4), QStringLiteral("00 visible after 42. %1").arg(guardNote));
        expectTrue(mask.at(5), QStringLiteral("43 visible. %1").arg(guardNote));
    }

    const QByteArray leadingNull = QByteArray::fromHex("0041");
    const QVector<bool> maskNoPrev =
        breco::buildStringModeVisibilityMask(leadingNull, std::nullopt);
    const QVector<bool> maskWithPrintedPrev = breco::buildStringModeVisibilityMask(
        leadingNull, static_cast<unsigned char>('Z'));
    expectTrue(maskNoPrev.size() == 2 && !maskNoPrev.at(0),
               QStringLiteral("Leading 00 hidden without previous backing byte. %1")
                   .arg(guardNote));
    expectTrue(maskWithPrintedPrev.size() == 2 && maskWithPrintedPrev.at(0),
               QStringLiteral("Leading 00 visible when previous backing byte is printed. %1")
                   .arg(guardNote));
}

void testBitmapTooltipForValidSequenceInAllModes() {
    class BitmapViewWidgetProbe : public breco::BitmapViewWidget {
    public:
        using breco::BitmapViewWidget::BitmapViewWidget;
        using breco::BitmapViewWidget::mouseMoveEvent;
    };

    BitmapViewWidgetProbe widget;
    widget.resize(320, 180);
    widget.show();
    QCoreApplication::processEvents();

    widget.setTextMode(breco::TextInterpretationMode::Ascii);
    widget.setCenterAnchorOffset(0);
    widget.setData(QByteArray("HELLO world"));

    const QPoint local(widget.width() / 2, widget.height() / 2);
    const QPoint global = widget.mapToGlobal(local);

    const std::array<std::pair<breco::BitmapMode, QString>, 6> modes = {
        std::make_pair(breco::BitmapMode::Rgb24, QStringLiteral("RGB24")),
        std::make_pair(breco::BitmapMode::Grey8, QStringLiteral("Grey8")),
        std::make_pair(breco::BitmapMode::Grey24, QStringLiteral("Grey24")),
        std::make_pair(breco::BitmapMode::Rgbi256, QStringLiteral("RGBi256")),
        std::make_pair(breco::BitmapMode::Binary, QStringLiteral("Binary")),
        std::make_pair(breco::BitmapMode::Text, QStringLiteral("Text"))};

    for (const auto& [mode, label] : modes) {
        QToolTip::hideText();
        QCoreApplication::processEvents();
        widget.setMode(mode);

        QMouseEvent moveEvent(QEvent::MouseMove, QPointF(local), QPointF(global),
                              Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        widget.mouseMoveEvent(&moveEvent);
        QCoreApplication::processEvents();

        const QString tooltip = QToolTip::text();
        expectTrue(!tooltip.isEmpty(),
                   QStringLiteral("Bitmap tooltip should show in mode %1").arg(label));
        expectTrue(tooltip.contains(QStringLiteral("HELLO")),
                   QStringLiteral("Bitmap tooltip should contain decoded content in mode %1")
                       .arg(label));
    }

    QToolTip::hideText();
}

void testBitmapTooltipWindowIsCappedAndCentered() {
    class BitmapViewWidgetProbe : public breco::BitmapViewWidget {
    public:
        using breco::BitmapViewWidget::BitmapViewWidget;
        using breco::BitmapViewWidget::mouseMoveEvent;
    };

    BitmapViewWidgetProbe widget;
    widget.resize(320, 180);
    widget.show();
    QCoreApplication::processEvents();

    widget.setTextMode(breco::TextInterpretationMode::Ascii);
    widget.setMode(breco::BitmapMode::Text);
    widget.setData(QByteArray(300, 'A'));
    widget.setCenterAnchorOffset(150);

    const QPoint local(widget.width() / 2, widget.height() / 2);
    const QPoint global = widget.mapToGlobal(local);
    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(local), QPointF(global),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    widget.mouseMoveEvent(&moveEvent);
    QCoreApplication::processEvents();

    const QString tooltip = QToolTip::text();
    expectTrue(!tooltip.isEmpty(), QStringLiteral("Bitmap tooltip should not be empty for long sequence"));

    const QRegularExpression rx(QStringLiteral("^(\\d+) bytes at offset: (\\d+)"));
    const QRegularExpressionMatch match = rx.match(tooltip);
    expectTrue(match.hasMatch(), QStringLiteral("Bitmap tooltip should include byte-count and offset header"));
    if (match.hasMatch()) {
        const int shownBytes = match.captured(1).toInt();
        const int shownOffset = match.captured(2).toInt();
        expectEqInt(shownBytes, 300,
                    QStringLiteral("Bitmap tooltip header should report full valid-sequence length"));
        expectEqInt(shownOffset, 0,
                    QStringLiteral("Bitmap tooltip header offset should report sequence start"));
    }

    const QStringList tooltipParts = tooltip.split(QStringLiteral("\n---\n"));
    expectTrue(tooltipParts.size() == 2,
               QStringLiteral("Bitmap tooltip should contain header and decoded payload sections"));
    if (tooltipParts.size() == 2) {
        expectTrue(tooltipParts.at(1).size() <= 128,
                   QStringLiteral("Bitmap tooltip decoded payload should be capped to <=128 bytes"));
    }

    QToolTip::hideText();
}

void testBitmapClickEmitsByteOffset() {
    class BitmapViewWidgetProbe : public breco::BitmapViewWidget {
    public:
        using breco::BitmapViewWidget::BitmapViewWidget;
        using breco::BitmapViewWidget::mousePressEvent;
        using breco::BitmapViewWidget::mouseReleaseEvent;
    };

    BitmapViewWidgetProbe widget;
    widget.resize(320, 180);
    widget.show();
    QCoreApplication::processEvents();
    widget.setMode(breco::BitmapMode::Text);
    widget.setData(QByteArray(300, 'A'));
    widget.setCenterAnchorOffset(150);

    bool clicked = false;
    quint64 clickedOffset = 0;
    QObject::connect(&widget, &breco::BitmapViewWidget::byteClicked, &widget,
                     [&](quint64 absoluteOffset) {
                         clicked = true;
                         clickedOffset = absoluteOffset;
                     });

    const QPoint local(widget.width() / 2, widget.height() / 2);
    const QPoint global = widget.mapToGlobal(local);
    QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(local), QPointF(global),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(local), QPointF(global),
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    widget.mousePressEvent(&pressEvent);
    widget.mouseReleaseEvent(&releaseEvent);
    QCoreApplication::processEvents();

    expectTrue(clicked, QStringLiteral("Bitmap left-click should emit byteClicked"));
    expectEqInt(static_cast<int>(clickedOffset), 150,
                QStringLiteral("Bitmap click at center should emit centered byte offset"));
}

void testResultModelColumnOrder() {
    breco::ResultModel model;
    QVector<breco::ScanTarget> scanTargets;
    scanTargets.push_back({QStringLiteral("/tmp/a.bin"), 1024});
    model.setScanTargets(&scanTargets);

    breco::MatchRecord m;
    m.scanTargetIdx = 0;
    m.threadId = 1;
    m.offset = (2ULL * 1024ULL * 1024ULL) + 12ULL;
    m.searchTimeNs = 2000000ULL;

    model.appendBatch({m});

    expectEqQString(model.headerData(2, Qt::Horizontal, Qt::DisplayRole).toString(),
                    QStringLiteral("Offset"),
                    QStringLiteral("ResultModel column 2 header should be Offset"));
    expectEqQString(model.data(model.index(0, 2), Qt::DisplayRole).toString(),
                    QStringLiteral("2 MiB"),
                    QStringLiteral("ResultModel column 2 should show approximate offset"));
    expectEqQString(model.headerData(3, Qt::Horizontal, Qt::DisplayRole).toString(),
                    QStringLiteral("Search time"),
                    QStringLiteral("ResultModel column 3 header should be Search time"));
    expectEqQString(model.data(model.index(0, 3), Qt::DisplayRole).toString(),
                    QStringLiteral("2 ms"),
                    QStringLiteral("ResultModel column 3 should show search time in ms"));
}

void testSpscQueueMechanics() {
    breco::SpscQueue<int, 4> queue;
    expectTrue(queue.tryPush(1), QStringLiteral("SpscQueue push 1"));
    expectTrue(queue.tryPush(2), QStringLiteral("SpscQueue push 2"));
    expectTrue(queue.tryPush(3), QStringLiteral("SpscQueue push 3 (capacity-1)"));
    expectTrue(!queue.tryPush(4), QStringLiteral("SpscQueue should report full at capacity-1"));

    int value = 0;
    expectTrue(queue.tryPop(value), QStringLiteral("SpscQueue pop after full"));
    expectEqInt(value, 1, QStringLiteral("SpscQueue FIFO first value"));

    expectTrue(queue.tryPush(4), QStringLiteral("SpscQueue push after pop (wrap)"));
    expectTrue(queue.tryPop(value), QStringLiteral("SpscQueue pop second value"));
    expectEqInt(value, 2, QStringLiteral("SpscQueue FIFO second value"));
    expectTrue(queue.tryPop(value), QStringLiteral("SpscQueue pop third value"));
    expectEqInt(value, 3, QStringLiteral("SpscQueue FIFO third value"));
    expectTrue(queue.tryPop(value), QStringLiteral("SpscQueue pop wrapped value"));
    expectEqInt(value, 4, QStringLiteral("SpscQueue FIFO wrapped value"));
    expectTrue(!queue.tryPop(value), QStringLiteral("SpscQueue should be empty"));
}

void testFileEnumerator() {
    QTemporaryDir tempDir;
    expectTrue(tempDir.isValid(), QStringLiteral("FileEnumerator temp dir should be valid"));
    if (!tempDir.isValid()) {
        return;
    }

    const QString rootFile = tempDir.filePath(QStringLiteral("root.bin"));
    const QString nestedDirPath = tempDir.filePath(QStringLiteral("nested"));
    QDir().mkpath(nestedDirPath);
    const QString nestedFile = QDir(nestedDirPath).filePath(QStringLiteral("inner.bin"));

    {
        QFile f(rootFile);
        expectTrue(f.open(QIODevice::WriteOnly), QStringLiteral("FileEnumerator create root file"));
        f.write("abc", 3);
    }
    {
        QFile f(nestedFile);
        expectTrue(f.open(QIODevice::WriteOnly), QStringLiteral("FileEnumerator create nested file"));
        f.write("xyz", 3);
    }

    const QVector<QString> single = breco::FileEnumerator::enumerateSingleFile(rootFile);
    expectEqInt(single.size(), 1, QStringLiteral("FileEnumerator single file should return one entry"));
    if (single.size() == 1) {
        expectEqQString(single.first(), QFileInfo(rootFile).absoluteFilePath(),
                        QStringLiteral("FileEnumerator single file absolute path"));
    }

    const QVector<QString> invalidSingle = breco::FileEnumerator::enumerateSingleFile(tempDir.path());
    expectEqInt(invalidSingle.size(), 0,
                QStringLiteral("FileEnumerator single file should reject directories"));

    const QVector<QString> recursive = breco::FileEnumerator::enumerateRecursive(tempDir.path());
    expectEqInt(recursive.size(), 2, QStringLiteral("FileEnumerator recursive should include nested files"));
}

void testWindowLoader() {
    QTemporaryDir tempDir;
    expectTrue(tempDir.isValid(), QStringLiteral("WindowLoader temp dir should be valid"));
    if (!tempDir.isValid()) {
        return;
    }

    const QString filePath = tempDir.filePath(QStringLiteral("chunk.bin"));
    {
        QFile f(filePath);
        expectTrue(f.open(QIODevice::WriteOnly), QStringLiteral("WindowLoader create file"));
        f.write("abcdef", 6);
    }

    breco::OpenFilePool pool;
    const auto direct = pool.readChunk(filePath, 2, 3);
    expectTrue(direct.has_value(), QStringLiteral("OpenFilePool valid read should succeed"));
    if (direct.has_value()) {
        expectEqQString(QString::fromLatin1(*direct), QStringLiteral("cde"),
                        QStringLiteral("OpenFilePool valid read bytes"));
    }

    const auto missing = pool.readChunk(tempDir.filePath(QStringLiteral("missing.bin")), 0, 4);
    expectTrue(!missing.has_value(), QStringLiteral("OpenFilePool missing file should return nullopt"));

    const auto badSeek = pool.readChunk(filePath, std::numeric_limits<quint64>::max(), 1);
    expectTrue(!badSeek.has_value(), QStringLiteral("OpenFilePool invalid seek should return nullopt"));

    breco::ShiftedWindowLoader loader(&pool);
    const breco::ShiftSettings zeroShift{0, breco::ShiftUnit::Bytes};
    const auto identity = loader.loadTransformedWindow(filePath, 6, 1, 4, zeroShift);
    expectTrue(identity.has_value(), QStringLiteral("ShiftedWindowLoader identity load should succeed"));
    if (identity.has_value()) {
        expectEqQString(QString::fromLatin1(*identity), QStringLiteral("bcde"),
                        QStringLiteral("ShiftedWindowLoader identity bytes"));
    }

    const breco::ShiftSettings byteShift{1, breco::ShiftUnit::Bytes};
    const auto shifted = loader.loadTransformedWindow(filePath, 6, 0, 4, byteShift);
    expectTrue(shifted.has_value(), QStringLiteral("ShiftedWindowLoader shifted load should succeed"));
    if (shifted.has_value()) {
        expectEqQString(QString::fromLatin1(*shifted), QStringLiteral("bcde"),
                        QStringLiteral("ShiftedWindowLoader shifted bytes"));
    }
}

#ifdef Q_OS_UNIX
void testOpenFilePoolExternalReadFd() {
    QTemporaryDir tempDir;
    expectTrue(tempDir.isValid(), QStringLiteral("External fd temp dir should be valid"));
    const QString filePath = tempDir.filePath(QStringLiteral("external.bin"));
    QFile file(filePath);
    expectTrue(file.open(QIODevice::WriteOnly), QStringLiteral("External fd test file open"));
    const QByteArray bytes("EXTERNAL-FD");
    expectEqInt(static_cast<int>(file.write(bytes)), bytes.size(),
                QStringLiteral("External fd test file write"));
    file.close();

    const int fd = ::open(filePath.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    expectTrue(fd >= 0, QStringLiteral("External fd should open"));
    if (fd < 0) {
        return;
    }

    breco::OpenFilePool pool;
    expectTrue(pool.registerExternalReadFd(QFileInfo(filePath).absoluteFilePath(), fd,
                                           static_cast<quint64>(bytes.size())),
               QStringLiteral("External fd should register"));
    const auto chunk = pool.readChunk(QFileInfo(filePath).absoluteFilePath(), 3, 5);
    expectTrue(chunk.has_value(), QStringLiteral("External fd read should succeed"));
    if (chunk.has_value()) {
        expectEqQString(QString::fromLatin1(chunk.value()), QStringLiteral("ERNAL"),
                        QStringLiteral("External fd read should use pread offset"));
    }
    pool.clearAll();
    expectTrue(pool.hasExternalReadFd(QFileInfo(filePath).absoluteFilePath()),
               QStringLiteral("External fd should survive path cache clear"));
    const auto afterClearChunk = pool.readChunk(QFileInfo(filePath).absoluteFilePath(), 0, 8);
    expectTrue(afterClearChunk.has_value(),
               QStringLiteral("External fd read should survive path cache clear"));
    if (afterClearChunk.has_value()) {
        expectEqQString(QString::fromLatin1(afterClearChunk.value()), QStringLiteral("EXTERNAL"),
                        QStringLiteral("External fd should still serve bytes after cache clear"));
    }
    pool.clearExternalReadFds();
    expectTrue(!pool.hasExternalReadFd(QFileInfo(filePath).absoluteFilePath()),
               QStringLiteral("External fd should clear explicitly"));
}
#endif

QByteArray makePngBytes(int width = 2, int height = 2) {
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(QColor(0x22, 0x66, 0xAA));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QByteArray makeAnimatedGifBytes() {
    return QByteArray::fromBase64(
        "R0lGODlhAgACAIEAAAAAAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQAAQAAACwAAAAA"
        "AgACAAAIBgABCAQQEAAh+QQBAQABACwAAAAAAgACAIH///8AAAAAAAAAAAAIBgABCAQQEAA7");
}

breco::EmbeddedImageScanSource sourceForBytes(const QByteArray& bytes) {
    breco::EmbeddedImageScanSource source;
    source.filePath = QStringLiteral("memory.bin");
    source.fileSize = static_cast<quint64>(bytes.size());
    source.read = [bytes](quint64 offset, quint64 size) -> std::optional<QByteArray> {
        if (offset >= static_cast<quint64>(bytes.size())) {
            return QByteArray();
        }
        const quint64 available = static_cast<quint64>(bytes.size()) - offset;
        const quint64 readSize = qMin(size, available);
        return bytes.mid(static_cast<qsizetype>(offset),
                         static_cast<qsizetype>(readSize));
    };
    return source;
}

void testEmbeddedImageScanner() {
    const QByteArray png = makePngBytes();
    QByteArray haystack(4094, '\x7F');
    const quint64 pngOffset = static_cast<quint64>(haystack.size());
    haystack += png;
    haystack += QByteArray("tail");

    breco::EmbeddedImageScanOptions options;
    options.formats = breco::EmbeddedImageFormat::Png;
    options.startOffset = 0;
    options.endOffsetExclusive = static_cast<quint64>(haystack.size());
    options.chunkSize = 4096;
    options.maxPixelsK = 1;
    options.maxResults = 5;
    breco::EmbeddedImageScanSummary summary;
    QVector<breco::EmbeddedImageResult> results =
        breco::scanEmbeddedImages(sourceForBytes(haystack), options, &summary);
    expectEqInt(results.size(), 1,
                QStringLiteral("PNG crossing chunk boundary should be found"));
    if (!results.isEmpty()) {
        expectEqInt(static_cast<int>(results.first().offset), static_cast<int>(pngOffset),
                    QStringLiteral("PNG result should report absolute offset"));
        expectEqQString(results.first().formatName, QStringLiteral("PNG"),
                        QStringLiteral("PNG result should report format"));
        expectTrue(results.first().encodedData == png,
                   QStringLiteral("PNG save payload should exclude following source bytes"));
    }

    QByteArray twoPngs = png + QByteArray("gap") + png;
    options.endOffsetExclusive = static_cast<quint64>(twoPngs.size());
    options.maxResults = 1;
    results = breco::scanEmbeddedImages(sourceForBytes(twoPngs), options, &summary);
    expectEqInt(results.size(), 1,
                QStringLiteral("maxResults should stop accepted image search"));

    QByteArray threePngs = png + QByteArray("gap") + png + QByteArray("pad") + png;
    options.endOffsetExclusive = static_cast<quint64>(threePngs.size());
    options.maxResults = 0;
    options.maxPixelsK = 4;
    options.chunkSize = 64;
    options.workerCount = 1;
    const QVector<breco::EmbeddedImageResult> oneJobResults =
        breco::scanEmbeddedImages(sourceForBytes(threePngs), options, &summary);
    options.workerCount = 4;
    QVector<quint64> byteProgress;
    QVector<quint64> rawProgress;
    QVector<int> resultProgress;
    results = breco::scanEmbeddedImages(
        sourceForBytes(threePngs), options, &summary, {},
        [&byteProgress, &rawProgress, &resultProgress](quint64 scanned, quint64, quint64 raw,
                                                       int found, int) {
            byteProgress.push_back(scanned);
            rawProgress.push_back(raw);
            resultProgress.push_back(found);
        });
    expectEqInt(results.size(), oneJobResults.size(),
                QStringLiteral("multi-job scan should match one-job result count"));
    for (int i = 0; i < qMin(results.size(), oneJobResults.size()); ++i) {
        expectEqInt(static_cast<int>(results.at(i).offset),
                    static_cast<int>(oneJobResults.at(i).offset),
                    QStringLiteral("multi-job scan should preserve deterministic result order"));
    }
    expectEqInt(results.size(), 3, QStringLiteral("maxResults 0 should scan all accepted images"));
    for (int i = 1; i < byteProgress.size(); ++i) {
        expectTrue(byteProgress.at(i) >= byteProgress.at(i - 1),
                   QStringLiteral("image byte progress should be monotonic"));
    }
    for (int i = 1; i < resultProgress.size(); ++i) {
        expectTrue(resultProgress.at(i) >= resultProgress.at(i - 1),
                   QStringLiteral("image result progress should be monotonic"));
    }
    for (int i = 1; i < rawProgress.size(); ++i) {
        expectTrue(rawProgress.at(i) >= rawProgress.at(i - 1),
                   QStringLiteral("image raw-read progress should be monotonic"));
    }
    expectTrue(summary.rawBytesRead >= summary.bytesScanned,
               QStringLiteral("image summary retains raw source bytes read"));

    std::atomic_bool cancelAfterFirstResult = false;
    options.maxResults = 0;
    results = breco::scanEmbeddedImages(
        sourceForBytes(threePngs), options, &summary,
        [&cancelAfterFirstResult]() { return cancelAfterFirstResult.load(); }, {},
        [&cancelAfterFirstResult](const breco::EmbeddedImageResult&) {
            cancelAfterFirstResult.store(true);
        });
    expectEqInt(results.size(), 1,
                QStringLiteral("cancelled image scan should retain accepted partial results"));
    expectTrue(summary.cancelled, QStringLiteral("cancelled image scan should mark summary"));

    std::mutex readMutex;
    std::optional<std::thread::id> readThread;
    bool readsStayOnOneThread = true;
    int readCount = 0;
    breco::EmbeddedImageScanSource instrumentedSource;
    instrumentedSource.filePath = QStringLiteral("instrumented.bin");
    instrumentedSource.fileSize = static_cast<quint64>(threePngs.size());
    instrumentedSource.read = [&threePngs, &readMutex, &readThread, &readsStayOnOneThread,
                               &readCount](quint64 offset,
                                           quint64 size) -> std::optional<QByteArray> {
        {
            std::lock_guard<std::mutex> lock(readMutex);
            ++readCount;
            const std::thread::id current = std::this_thread::get_id();
            if (!readThread.has_value()) {
                readThread = current;
            } else if (readThread.value() != current) {
                readsStayOnOneThread = false;
            }
        }
        if (offset >= static_cast<quint64>(threePngs.size())) {
            return QByteArray();
        }
        const quint64 available = static_cast<quint64>(threePngs.size()) - offset;
        const quint64 readSize = qMin(size, available);
        return threePngs.mid(static_cast<qsizetype>(offset),
                             static_cast<qsizetype>(readSize));
    };
    options.workerCount = 8;
    results = breco::scanEmbeddedImages(instrumentedSource, options, &summary);
    expectEqInt(results.size(), 3, QStringLiteral("instrumented multi-job scan should find images"));
    expectTrue(readCount > 0, QStringLiteral("instrumented image scan should read source bytes"));
    expectTrue(readsStayOnOneThread,
               QStringLiteral("image scan workers should not call source.read"));

    if (breco::embeddedImageFormatHasReader(breco::EmbeddedImageFormat::Gif)) {
        const QByteArray animatedGif = makeAnimatedGifBytes();
        const QByteArray animatedGifWithTail = animatedGif + QByteArray("unrelated tail bytes");
        options.formats = breco::EmbeddedImageFormat::Gif;
        options.startOffset = 0;
        options.endOffsetExclusive = static_cast<quint64>(animatedGifWithTail.size());
        options.maxResults = 1;
        options.workerCount = 2;
        results =
            breco::scanEmbeddedImages(sourceForBytes(animatedGifWithTail), options, &summary);
        expectEqInt(results.size(), 1, QStringLiteral("Animated GIF should be detected"));
        if (results.size() == 1) {
            expectEqInt(results.first().frameCount(), 2,
                        QStringLiteral("Animated GIF should retain every frame"));
            expectEqInt(results.first().frameDelaysMs.size(), 2,
                        QStringLiteral("Animated GIF should retain every frame delay"));
            for (const int delay : results.first().frameDelaysMs) {
                expectEqInt(delay, 16,
                            QStringLiteral("GIF frame delays below 16 ms should be clamped"));
            }
            expectTrue(results.first().encodedData == animatedGif,
                       QStringLiteral("GIF payload should include all frames but exclude tail bytes"));
        }
        expectEqQString(
            breco::embeddedImageFileExtension(breco::EmbeddedImageFormat::Gif),
            QStringLiteral("gif"), QStringLiteral("GIF save extension should be seeded correctly"));
        expectEqQString(
            breco::embeddedImageFileExtension(breco::EmbeddedImageFormat::Jpeg),
            QStringLiteral("jpg"), QStringLiteral("JPEG save extension should be seeded correctly"));
    }

    QByteArray hugePngHeader = QByteArray::fromHex("89504e470d0a1a0a0000000d49484452");
    hugePngHeader += QByteArray::fromHex("000F4240000F4240");
    hugePngHeader += QByteArray(16, '\0');
    QByteArray oversizedThenValid = hugePngHeader + QByteArray("noise") + png;
    options.formats = breco::EmbeddedImageFormat::Png;
    options.endOffsetExclusive = static_cast<quint64>(oversizedThenValid.size());
    options.maxResults = 5;
    options.maxPixelsK = 4;
    results = breco::scanEmbeddedImages(sourceForBytes(oversizedThenValid), options, &summary);
    expectEqInt(results.size(), 1,
                QStringLiteral("Oversized PNG candidate should be rejected before valid PNG"));

    bool cancelCalled = false;
    results = breco::scanEmbeddedImages(sourceForBytes(haystack), options, &summary, [&cancelCalled]() {
        cancelCalled = true;
        return true;
    });
    expectTrue(cancelCalled, QStringLiteral("Image scan cancellation callback should run"));
    expectTrue(summary.cancelled, QStringLiteral("Image scan summary should report cancellation"));

    if (breco::embeddedImageFormatHasReader(breco::EmbeddedImageFormat::Xbm)) {
        const QByteArray xbm =
            "#define test_width 1\n#define test_height 1\n"
            "static unsigned char test_bits[] = { 0x01 };\n";
        const QByteArray prefixed = QByteArray("xxxx") + xbm;
        options.formats = breco::EmbeddedImageFormat::Xbm;
        options.startOffset = 0;
        options.endOffsetExclusive = static_cast<quint64>(prefixed.size());
        results = breco::scanEmbeddedImages(sourceForBytes(prefixed), options, &summary);
        expectEqInt(results.size(), 0,
                    QStringLiteral("XBM should not be scanned away from the anchor"));
        options.startOffset = 4;
        results = breco::scanEmbeddedImages(sourceForBytes(prefixed), options, &summary);
        expectEqInt(results.size(), 1,
                    QStringLiteral("XBM should decode when the anchor is its first byte"));
    }
}

void testScanProgressFormatting();

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    QApplication app(argc, argv);

    testMatchUtilsIndexOf();
    testShiftReadPlan();
    testShiftTransformWindow();
    testTextSequenceAnalyzer();
    testTextViewStringsOnlyFiltering();
    testStringModeNullVisibilityRule();
    testBitmapTooltipForValidSequenceInAllModes();
    testBitmapTooltipWindowIsCappedAndCentered();
    testBitmapClickEmitsByteOffset();
    testResultModelColumnOrder();
    testSpscQueueMechanics();
    testFileEnumerator();
    testWindowLoader();
    testScanProgressFormatting();
#ifdef Q_OS_UNIX
    testOpenFilePoolExternalReadFd();
#endif
    testEmbeddedImageScanner();

    if (g_failures == 0) {
        qInfo() << "All unit tests passed";
        return 0;
    }

    qCritical() << g_failures << "unit test(s) failed";
    return 1;
}
namespace {

void testScanProgressFormatting() {
    using namespace std::chrono_literals;
    breco::ScanProgressTracker tracker;
    const auto start = breco::ScanProgressTracker::Clock::time_point{} + 1s;
    tracker.reset(0, 0, start);
    const breco::ScanProgressSnapshot first = tracker.sample(1024, 2048, 2048, start + 2s);
    expectEqQString(
        breco::formatScanProgress(first),
        QStringLiteral("1.00 KiB / 2.00 KiB @ 0.50 KiB/s ( Disk: 1.00 KiB/s )  - 50.00 %"),
        QStringLiteral("Scan progress formatter includes bytes, rates, and percentage"));

    tracker.sample(3072, 8192, 4096, start + 4s);
    tracker.sample(6144, 8192, 6144, start + 6s);
    tracker.sample(10240, 12288, 8192, start + 8s);
    const breco::ScanProgressSnapshot rolled =
        tracker.sample(15360, 16384, 10240, start + 10s);
    expectTrue(rolled.scanBytesPerSecond == 1792.0,
               QStringLiteral("Scan speed averages only the latest four update intervals"));
    expectTrue(rolled.rawBytesPerSecond == 1024.0,
               QStringLiteral("Raw speed averages the latest four update intervals"));
    expectTrue(breco::formatScanProgress({2048, 1024, 0, 0.0, 0.0})
                   .endsWith(QStringLiteral("100.00 %")),
               QStringLiteral("Progress formatting clamps scanned bytes and percentage"));
}

}  // namespace

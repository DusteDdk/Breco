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
#include "panel/StructModeLeftPanel.h"
#include "scan/MatchUtils.h"
#include "scan/ScanProgress.h"
#include "scan/SpscQueue.h"
#include "scan/ShiftTransform.h"
#include "struct/StructDeclarationParser.h"
#include "struct/StructExport.h"
#include "struct/StructureLibrary.h"
#include "struct/StructureScanner.h"
#include "struct/StructVisualizedTreeModel.h"
#include "struct/StructVisualizer.h"
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

void testStructDeclarationParser() {
    const QString valid = QStringLiteral(
        "int32_t standaloneTopLevelEntry;\n"
        "typedef <be>uint32_t be32;\n"
        "struct MyStructName {\n"
        "  be32 someBeInt32MemberName;\n"
        "  uint16_t<be> someBeUInt16MemberName;\n"
        "  int64_t someNativeInt64Name;\n"
        "}\n"
        "struct NameB {\n"
        "  uint8_t innerByte;\n"
        "}\n"
        "struct NameA {\n"
        "  NameB memberInStructAName;\n"
        "}");
    const breco::ParseResult ok = breco::parseStructDeclaration(valid);
    expectTrue(ok.valid, QStringLiteral("Struct parser valid declaration"));
    expectEqInt(ok.graph.entryNames().size(), 5,
                QStringLiteral("Struct parser entry count (standalone + 3 structs + be32 typedef)"));
    expectEqQString(ok.graph.defaultEntryName(), QString(),
                    QStringLiteral("Struct parser default entry should be optional"));

    const breco::ParseResult defaultOk = breco::parseStructDeclaration(
        QStringLiteral("/default Second;\n"
                       "struct First { uint8 value; }\n"
                       "struct Second { uint8 value; }"));
    expectTrue(defaultOk.valid,
               QStringLiteral("Struct parser should accept one /default directive"));
    expectEqQString(defaultOk.graph.defaultEntryName(), QStringLiteral("Second"),
                    QStringLiteral("Struct parser should retain the default entry"));

    const breco::ParseResult unknownDefaultFail =
        breco::parseStructDeclaration(
            QStringLiteral("/default Missing\nstruct Present { uint8 value; }"));
    expectTrue(!unknownDefaultFail.valid,
               QStringLiteral("Struct parser should reject an unavailable default entry"));
    expectEqQString(
        unknownDefaultFail.errorMessage,
        QStringLiteral("Default entry 'Missing' is not available"),
        QStringLiteral("Struct parser should identify the unavailable default entry"));

    const breco::ParseResult duplicateDefaultFail =
        breco::parseStructDeclaration(
            QStringLiteral("/default First\n/default First\n"
                           "struct First { uint8 value; }"));
    expectTrue(!duplicateDefaultFail.valid,
               QStringLiteral("Struct parser should reject duplicate /default directives"));
    expectEqQString(
        duplicateDefaultFail.errorMessage,
        QStringLiteral("/default may appear only once per file"),
        QStringLiteral("Struct parser should identify duplicate /default directives"));

    const QString typedefStruct = QStringLiteral(
        "typedef struct test {\n"
        "    uint32_t magic_word;\n"
        "    uint32_t frame_length;\n"
        "} anotherName;");
    const breco::ParseResult typedefStructOk = breco::parseStructDeclaration(typedefStruct);
    expectTrue(typedefStructOk.valid, QStringLiteral("typedef struct { } alias"));
    expectTrue(typedefStructOk.graph.entryNames().contains(QStringLiteral("test")),
               QStringLiteral("typedef struct tag in entry list"));
    expectTrue(typedefStructOk.graph.entryNames().contains(QStringLiteral("anotherName")),
               QStringLiteral("typedef struct alias in entry list"));

    const QString typedefAlias = QStringLiteral(
        "struct test {\n"
        "    uint32_t magic_word;\n"
        "    uint32_t frame_length;\n"
        "};\n"
        "typedef test fisk;");
    const breco::ParseResult aliasOk = breco::parseStructDeclaration(typedefAlias);
    expectTrue(aliasOk.valid, QStringLiteral("typedef struct name alias"));
    expectTrue(aliasOk.graph.entryNames().contains(QStringLiteral("fisk")),
               QStringLiteral("typedef alias fisk in entry list"));

    const QString integerAliasSpellings = QStringLiteral(
        "struct AliasSpellings {\n"
        "  uint_8 u8a;\n"
        "  uint8 u8b;\n"
        "  uint_16 u16a;\n"
        "  uint16 u16b;\n"
        "  uint_32 u32a;\n"
        "  uint32 u32b;\n"
        "  uint_64 u64a;\n"
        "  uint64 u64b;\n"
        "  int_8 i8a;\n"
        "  int8 i8b;\n"
        "  int_16 i16a;\n"
        "  int16 i16b;\n"
        "  int_32 i32a;\n"
        "  int32 i32b;\n"
        "  int_64 i64a;\n"
        "  int64 i64b;\n"
        "}");
    const breco::ParseResult integerAliasOk =
        breco::parseStructDeclaration(integerAliasSpellings);
    expectTrue(integerAliasOk.valid,
               QStringLiteral("Struct parser should accept integer aliases without _t"));

    const QString invalidAnonymousType = QStringLiteral(
        "struct First {\n"
        "  uint32_t magic_word;\n"
        "};\n"
        "typedef struct {\n"
        "  uint_99 broken;\n"
        "} Alias;");
    const breco::ParseResult invalidTypeFail =
        breco::parseStructDeclaration(invalidAnonymousType);
    const int invalidTypeOffset = invalidAnonymousType.indexOf(QStringLiteral("uint_99"));
    expectTrue(!invalidTypeFail.valid, QStringLiteral("Struct parser should reject bad member type"));
    expectEqQString(
        invalidTypeFail.errorMessage,
        QStringLiteral("Invalid member type 'uint_99' in 'anonymous struct'"),
        QStringLiteral("Struct parser invalid member type message should name type and struct"));
    expectEqInt(invalidTypeFail.errorRange.start, invalidTypeOffset,
                QStringLiteral("Struct parser error range should start at invalid type"));
    expectEqInt(invalidTypeFail.errorRange.end,
                invalidTypeOffset + QStringLiteral("uint_99").size(),
                QStringLiteral("Struct parser error range should end after invalid type"));
    expectTrue(invalidTypeFail.errorRange.start > 0,
               QStringLiteral("Struct parser error range should not point at first line"));

    const breco::ParseResult nestedOrderFail = breco::parseStructDeclaration(
        QStringLiteral("struct NameA { NameB m; } struct NameB { uint8_t b; }"));
    expectTrue(!nestedOrderFail.valid,
               QStringLiteral("Struct parser should reject forward struct reference"));

    const breco::ParseResult duplicateFail = breco::parseStructDeclaration(
        QStringLiteral("uint8_t a; uint8_t a;"));
    expectTrue(!duplicateFail.valid, QStringLiteral("Struct parser duplicate name"));
}

void testStructVisualizer() {
    const QString decl = QStringLiteral(
        "struct Inner { uint16_t x; uint8_t y; }\n"
        "struct Outer { Inner nested; uint8_t tail; }");
    const breco::ParseResult parsed = breco::parseStructDeclaration(decl);
    if (!parsed.valid) {
        expectTrue(false, QStringLiteral("Struct visualizer setup parse"));
        return;
    }

    const QByteArray buffer = QByteArray::fromHex("01020304");
    const breco::VisualizedNode one =
        breco::visualize(parsed.graph, QStringLiteral("Outer"), buffer, 0, 1);
    expectEqInt(one.children.size(), 1, QStringLiteral("Visualizer one outer chunk"));
    expectEqInt(one.children.first().children.size(), 2,
                QStringLiteral("Visualizer outer struct has nested + tail"));
    if (!one.children.isEmpty() &&
        one.children.first().children.size() == 2) {
        const breco::VisualizedNode& outer = one.children.first();
        const breco::VisualizedNode& inner = outer.children.first();
        expectTrue(outer.hasSourceOffset && outer.sourceOffset == 0,
                   QStringLiteral("Outer struct should start at its first natural byte"));
        expectTrue(outer.sourceLength == 4,
                   QStringLiteral("Outer struct should cover all member bytes"));
        expectTrue(inner.hasSourceOffset && inner.sourceOffset == 0,
                   QStringLiteral("Nested struct should retain its natural start byte"));
        expectTrue(inner.sourceLength == 3,
                   QStringLiteral("Nested struct should cover its member byte range"));
        expectTrue(inner.children.size() == 2 &&
                       inner.children.at(0).sourceOffset == 0 &&
                       inner.children.at(0).sourceLength == 2 &&
                       inner.children.at(1).sourceOffset == 2 &&
                       inner.children.at(1).sourceLength == 1,
                   QStringLiteral("Nested members should expose byte-order-independent offsets"));
        expectTrue(outer.children.at(1).sourceOffset == 3,
                   QStringLiteral("Following member should expose its first byte offset"));
        expectEqQString(
            decl.mid(outer.declarationRange.start,
                     outer.declarationRange.end - outer.declarationRange.start),
            QStringLiteral("Outer"),
            QStringLiteral("Top-level struct should retain its declaration range"));
        expectEqQString(
            decl.mid(inner.declarationRange.start,
                     inner.declarationRange.end - inner.declarationRange.start),
            QStringLiteral("nested"),
            QStringLiteral("Nested struct field should retain its field range"));
        expectEqQString(
            decl.mid(inner.children.first().declarationRange.start,
                     inner.children.first().declarationRange.end -
                         inner.children.first().declarationRange.start),
            QStringLiteral("x"),
            QStringLiteral("Scalar field should retain its field range"));
    }

    const breco::VisualizedNode offsetBuffer =
        breco::visualize(parsed.graph, QStringLiteral("Outer"),
                         QByteArray::fromHex("FFFF01020304"), 2, 1);
    expectTrue(!offsetBuffer.children.isEmpty() &&
                   offsetBuffer.children.first().sourceOffset == 2 &&
                   offsetBuffer.children.first().children.at(1).sourceOffset == 5,
               QStringLiteral("Visualizer offsets should include the input-buffer start index"));

    const QByteArray threeChunks = QByteArray::fromHex("0102030401020304");
    const breco::VisualizedNode upToThree =
        breco::visualize(parsed.graph, QStringLiteral("Outer"), threeChunks, 0, 3);
    expectEqInt(upToThree.children.size(), 2,
                QStringLiteral("Visualizer chunk cap with exact buffer for two outers"));

    const QByteArray partial = QByteArray::fromHex("010203");
    const breco::VisualizedNode partialOuter =
        breco::visualize(parsed.graph, QStringLiteral("Outer"), partial, 0, 3);
    expectEqInt(partialOuter.children.size(), 1,
                QStringLiteral("Visualizer partial buffer yields one outer chunk"));
    expectTrue(partialOuter.children.first().bytesMissing > 0,
               QStringLiteral("Visualizer partial outer reports bytes missing"));

    const QByteArray beBytes = QByteArray::fromHex("00000039");
    const breco::ParseResult beDecl =
        breco::parseStructDeclaration(QStringLiteral("typedef <be>uint32_t be32; be32 field;"));
    expectTrue(beDecl.valid, QStringLiteral("BE typedef parse"));
    const breco::VisualizedNode beNode =
        breco::visualize(beDecl.graph, QStringLiteral("field"), beBytes, 0, 1);
    expectEqInt(beNode.children.size(), 1, QStringLiteral("BE standalone chunk count"));
    expectEqQString(beNode.children.first().valueText, QStringLiteral("57 (0X39)"),
                    QStringLiteral("BE uint32 decode"));
    expectEqQString(beNode.children.first().rawBytes.toHex().toUpper(), QStringLiteral("00000039"),
                    QStringLiteral("Raw bytes preserved in buffer order"));

    breco::StructVisualizedTreeModel model;
    model.setRoot(beNode);
    const QModelIndex bytesIndex =
        model.index(0, breco::StructVisualizedTreeModel::Bytes);
    expectEqQString(model.data(bytesIndex).toString(), QStringLiteral("00 00 00 39"),
                    QStringLiteral("Struct tree model should space displayed bytes"));
    expectTrue(
        model.data(model.index(0, breco::StructVisualizedTreeModel::Name),
                   breco::StructVisualizedTreeModel::SourceOffsetRole)
                .isValid() &&
            model.data(model.index(0, breco::StructVisualizedTreeModel::Name),
                       breco::StructVisualizedTreeModel::SourceOffsetRole)
                    .toULongLong() == 0,
        QStringLiteral("Struct tree model should expose a zero source offset"));
    expectEqInt(model.columnCount(), 5,
                QStringLiteral("Struct tree should expose only its five visible columns"));
    expectEqQString(
        model.headerData(breco::StructVisualizedTreeModel::Valid,
                         Qt::Horizontal).toString(),
        QStringLiteral("Valid"),
        QStringLiteral("Struct tree validity column should be named Valid"));

    breco::VisualizedNode statusRoot;
    statusRoot.name = QStringLiteral("root");
    breco::VisualizedNode normal;
    normal.name = QStringLiteral("normal");
    breco::VisualizedNode conditionPass;
    conditionPass.name = QStringLiteral("condition-pass");
    conditionPass.hasCondition = true;
    breco::VisualizedNode conditionPassEven = conditionPass;
    conditionPassEven.name = QStringLiteral("condition-pass-even");
    breco::VisualizedNode conditionFail = conditionPass;
    conditionFail.name = QStringLiteral("condition-fail");
    conditionFail.valid = false;
    breco::VisualizedNode missing;
    missing.name = QStringLiteral("missing");
    missing.bytesMissing = 3;
    breco::VisualizedNode conditionMissing = conditionFail;
    conditionMissing.name = QStringLiteral("condition-missing");
    conditionMissing.bytesMissing = 1;
    statusRoot.children = {normal, conditionPass, conditionPassEven,
                           conditionFail, missing, conditionMissing};
    model.setRoot(statusRoot);
    expectEqQString(
        model.data(model.index(0, breco::StructVisualizedTreeModel::Valid))
            .toString(),
        QString(),
        QStringLiteral("Ordinary complete nodes should leave Valid empty"));
    expectEqQString(
        model.data(model.index(1, breco::StructVisualizedTreeModel::Valid))
            .toString(),
        QStringLiteral("true"),
        QStringLiteral("Passing /cond nodes should show true"));
    expectEqQString(
        model.data(model.index(3, breco::StructVisualizedTreeModel::Valid))
            .toString(),
        QStringLiteral("false"),
        QStringLiteral("Failing /cond nodes should show false"));
    expectEqQString(
        model.data(model.index(4, breco::StructVisualizedTreeModel::Valid))
            .toString(),
        QStringLiteral("3 missing bytes"),
        QStringLiteral("Truncated nodes should show their missing byte count"));
    expectEqQString(
        model.data(model.index(5, breco::StructVisualizedTreeModel::Valid))
            .toString(),
        QStringLiteral("false, 1 missing byte"),
        QStringLiteral("Conditional truncated nodes should show both states"));
    expectTrue(
        model.data(model.index(0, breco::StructVisualizedTreeModel::Name),
                   Qt::BackgroundRole)
            .isNull(),
        QStringLiteral("Ordinary complete rows should keep the normal background"));
    expectTrue(
        model.data(model.index(1, breco::StructVisualizedTreeModel::Name),
                   Qt::BackgroundRole)
                .value<QColor>() == QColor(208, 240, 208),
        QStringLiteral("Passing /cond odd rows should use darker light green"));
    expectTrue(
        model.data(
                 model.index(1, breco::StructVisualizedTreeModel::Name),
                 breco::StructVisualizedTreeModel::EvenRowBackgroundRole)
                .value<QColor>() == QColor(235, 255, 235),
        QStringLiteral("Passing /cond visual even rows should use light green"));
    expectTrue(
        model.data(model.index(2, breco::StructVisualizedTreeModel::Name),
                   Qt::BackgroundRole)
                .value<QColor>() == QColor(235, 255, 235),
        QStringLiteral("Passing /cond even rows should use light green"));
    expectTrue(
        model.data(model.index(3, breco::StructVisualizedTreeModel::Name),
                   Qt::BackgroundRole)
                .value<QColor>() == QColor(255, 208, 208),
        QStringLiteral("Failing /cond odd rows should use darker light red"));
    expectTrue(
        model.data(
                 model.index(3, breco::StructVisualizedTreeModel::Name),
                 breco::StructVisualizedTreeModel::EvenRowBackgroundRole)
                .value<QColor>() == QColor(255, 235, 235),
        QStringLiteral("Failing /cond visual even rows should use light red"));
    expectTrue(
        model.data(model.index(4, breco::StructVisualizedTreeModel::Name),
                   Qt::BackgroundRole)
                .value<QColor>() == QColor(255, 235, 235),
        QStringLiteral("Missing-byte even rows should use light red"));

    const breco::ParseResult nativeEndianDecl =
        breco::parseStructDeclaration(QStringLiteral("uint16_t field;"));
    expectTrue(nativeEndianDecl.valid, QStringLiteral("Native-endian field parse"));
    const QByteArray endianBytes = QByteArray::fromHex("0102");
    const breco::VisualizedNode littleDefault =
        breco::visualize(nativeEndianDecl.graph, QStringLiteral("field"), endianBytes, 0, 1,
                         breco::Endianness::Little);
    const breco::VisualizedNode bigDefault =
        breco::visualize(nativeEndianDecl.graph, QStringLiteral("field"), endianBytes, 0, 1,
                         breco::Endianness::Big);
    expectEqQString(littleDefault.children.first().valueText, QStringLiteral("513 (0X201)"),
                    QStringLiteral("Undecorated field should use little default endian"));
    expectEqQString(bigDefault.children.first().valueText, QStringLiteral("258 (0X102)"),
                    QStringLiteral("Undecorated field should use big default endian"));

    const breco::ParseResult decoratedEndianDecl =
        breco::parseStructDeclaration(QStringLiteral("uint16_t<be> field;"));
    expectTrue(decoratedEndianDecl.valid, QStringLiteral("Decorated-endian field parse"));
    const breco::VisualizedNode decoratedWithLittleDefault =
        breco::visualize(decoratedEndianDecl.graph, QStringLiteral("field"), endianBytes, 0, 1,
                         breco::Endianness::Little);
    expectEqQString(decoratedWithLittleDefault.children.first().valueText,
                    QStringLiteral("258 (0X102)"),
                    QStringLiteral("Explicit BE field should override little default endian"));

    const breco::ParseResult exportDecl = breco::parseStructDeclaration(
        QStringLiteral("struct Exported {"
                       " uint16<be> be;"
                       " uint16<le> le;"
                       " utf16str<be><len:4> title;"
                       " byte<len:2> raw;"
                       " }"));
    expectTrue(exportDecl.valid, QStringLiteral("Export fixture should parse"));
    breco::VisualizedNode exportRoot =
        breco::visualize(exportDecl.graph, QStringLiteral("Exported"),
                         QByteArray::fromHex("0102030400410042AABB"), 0, 1,
                         breco::Endianness::Little);
    breco::VisualizedNode& exported = exportRoot.children.first();
    exported.sourceFilePath = QStringLiteral("/tmp/export.bin");
    for (breco::VisualizedNode& child : exported.children) {
        child.sourceFilePath = exported.sourceFilePath;
    }
    expectTrue(breco::exportVisualizedBytes(
                   exported, breco::StructBinaryExportMode::SourceEndianness,
                   breco::Endianness::Little) ==
                   QByteArray::fromHex("0102030400410042AABB"),
               QStringLiteral("Source-endian binary export should preserve bytes"));
    expectTrue(breco::exportVisualizedBytes(
                   exported, breco::StructBinaryExportMode::DeclaredEndianness,
                   breco::Endianness::Little) ==
                   QByteArray::fromHex("0201030441004200AABB"),
               QStringLiteral("Declared-endian binary export should swap explicit opposite endian units"));
    expectEqQString(breco::formatScalarValue(exported.children.at(0),
                                             breco::StructScalarFormat::Default),
                    QStringLiteral("258"),
                    QStringLiteral("Integer default copy should be decimal only"));
    expectEqQString(breco::formatScalarValue(exported.children.at(3),
                                             breco::StructScalarFormat::Default),
                    QStringLiteral("AA BB"),
                    QStringLiteral("Byte field default copy should be spaced uppercase hex"));
    expectEqQString(breco::formatScalarValue(exported.children.at(3),
                                             breco::StructScalarFormat::Hex),
                    QStringLiteral("AA BB"),
                    QStringLiteral("HEX scalar copy should use spaced uppercase bytes"));
    expectEqQString(breco::formatScalarValue(exported.children.at(3),
                                             breco::StructScalarFormat::Utf8),
                    QStringLiteral("2 byte(s)"),
                    QStringLiteral("String-format byte copy should use visible Value text"));
    expectEqQString(breco::formatScalarValue(exported.children.at(2),
                                             breco::StructScalarFormat::Default),
                    QStringLiteral("AB"),
                    QStringLiteral("String default copy should use decoded text"));
    expectEqQString(breco::formatPrefixedScalarValue(
                        exported.children.at(0), breco::StructScalarFormat::Hex),
                    QStringLiteral("/tmp/export.bin:0:be:<be>uint16 > 01 02"),
                    QStringLiteral("Prefixed scalar copy should include path offset name type and value"));
    const QByteArray singleJson =
        breco::serializeVisualizedNode(exported.children.at(0));
    expectTrue(singleJson.startsWith("{\n    \"value\""),
               QStringLiteral("Single-node JSON should use brecodump formatting"));
    expectTrue(singleJson.contains("\"rawBytesHex\": \"01 02\""),
               QStringLiteral("Single-node JSON should contain brecodump rawBytesHex"));
    QVector<const breco::VisualizedNode*> jsonNodes = {
        &exported.children.at(0), &exported.children.at(1)};
    expectTrue(breco::serializeVisualizedNodes(jsonNodes).startsWith("[\n"),
               QStringLiteral("Multi-node JSON should serialize as an array"));
}

void testStructModePanelUtilities() {
    quint64 offset = 0;
    expectTrue(breco::StructModeLeftPanel::parseAbsoluteOffset(
                   QStringLiteral("42"), &offset) &&
                   offset == 42,
               QStringLiteral("Struct view offsets should accept decimal"));
    expectTrue(breco::StructModeLeftPanel::parseAbsoluteOffset(
                   QStringLiteral("0x2A"), &offset) &&
                   offset == 42,
               QStringLiteral("Struct view offsets should accept hexadecimal"));
    expectTrue(!breco::StructModeLeftPanel::parseAbsoluteOffset(
                   QStringLiteral("-1"), &offset),
               QStringLiteral("Struct view offsets should reject negatives"));
    expectTrue(!breco::StructModeLeftPanel::parseAbsoluteOffset(
                   QStringLiteral("0xnope"), &offset),
               QStringLiteral("Struct view offsets should reject invalid text"));

    const QStringList snippetObjects = {
        QStringLiteral("structKeywordLabel"),
        QStringLiteral("typedefKeywordLabel"),
        QStringLiteral("bigEndianModifierLabel"),
        QStringLiteral("littleEndianModifierLabel"),
        QStringLiteral("uint8TypeLabel"),
        QStringLiteral("uint16TypeLabel"),
        QStringLiteral("uint32TypeLabel"),
        QStringLiteral("uint64TypeLabel"),
        QStringLiteral("int8TypeLabel"),
        QStringLiteral("int16TypeLabel"),
        QStringLiteral("int32TypeLabel"),
        QStringLiteral("int64TypeLabel"),
        QStringLiteral("asciiStringTypeLabel"),
        QStringLiteral("utf8StringTypeLabel"),
        QStringLiteral("utf16StringTypeLabel"),
        QStringLiteral("byteTypeLabel"),
        QStringLiteral("maxLengthModifierLabel"),
        QStringLiteral("fixedLengthModifierLabel"),
        QStringLiteral("untilExpressionModifierLabel"),
        QStringLiteral("variableDirectiveLabel"),
        QStringLiteral("repeatDirectiveLabel"),
        QStringLiteral("conditionDirectiveLabel"),
        QStringLiteral("whenDirectiveLabel"),
        QStringLiteral("assertDirectiveLabel"),
        QStringLiteral("defaultDirectiveLabel"),
        QStringLiteral("bitfieldBlockLabel"),
    };
    for (const QString& objectName : snippetObjects) {
        const QString snippet =
            breco::StructModeLeftPanel::languageSnippetForObjectName(objectName);
        expectTrue(!snippet.isEmpty(),
                   QStringLiteral("Language snippet should exist for %1").arg(objectName));
        expectTrue(breco::parseStructDeclaration(snippet).valid,
                   QStringLiteral("Language snippet should parse for %1").arg(objectName));
    }

    breco::StructModeLeftPanel panel;
    panel.structDeclarationEdit()->setPlainText(
        QStringLiteral("uint8 first;\nuint8 second;"));
    QTextCursor cursor(panel.structDeclarationEdit()->document());
    cursor.setPosition(1);
    panel.structDeclarationEdit()->setTextCursor(cursor);
    panel.insertLanguageSnippet(QStringLiteral("int8 inserted;"));
    expectEqQString(panel.declarationText(),
                    QStringLiteral("uint8 first;\nint8 inserted;\nuint8 second;"),
                    QStringLiteral("Language snippets should insert after the cursor line"));

    breco::VisualizedNode mergedRoot;
    mergedRoot.name = QStringLiteral("root");
    breco::VisualizedNode preview;
    preview.name = QStringLiteral("Preview");
    preview.children.push_back(breco::VisualizedNode{});
    breco::VisualizedNode saved;
    saved.name = QStringLiteral("Saved header");
    saved.children.push_back(breco::VisualizedNode{});
    saved.children.push_back(breco::VisualizedNode{});
    mergedRoot.children = {preview, saved};
    breco::StructVisualizedTreeModel model;
    model.setRoot(mergedRoot);
    expectEqInt(model.rowCount(), 2,
                QStringLiteral("Merged struct tree should preserve wrapper count"));
    expectEqQString(
        model.data(model.index(0, breco::StructVisualizedTreeModel::Name)).toString(),
        QStringLiteral("Preview"),
        QStringLiteral("Preview view should remain first"));
    expectEqQString(
        model.data(model.index(1, breco::StructVisualizedTreeModel::Name)).toString(),
        QStringLiteral("Saved header"),
        QStringLiteral("Saved wrapper name should follow preview"));
    expectEqInt(model.rowCount(model.index(1, 0)), 2,
                QStringLiteral("Repeated entries should remain under one saved wrapper"));
    mergedRoot.children[1].name = QStringLiteral("root");
    model.setRoot(mergedRoot);
    expectEqInt(model.rowCount(), 2,
                QStringLiteral("Only the synthetic top-level root should be flattened"));
    expectEqQString(
        model.data(model.index(1, breco::StructVisualizedTreeModel::Name)).toString(),
        QStringLiteral("root"),
        QStringLiteral("A user saved view named root should keep its wrapper"));
    expectEqInt(model.rowCount(model.index(1, 0)), 2,
                QStringLiteral("A saved view named root should keep children nested"));
}

void testDynamicStructLanguage() {
    const QString declaration = QStringLiteral(
        "struct ExampleA {\n"
        "  /var(l) uint8 len;\n"
        "  byte<len:$l> byteArray;\n"
        "}\n"
        "struct ExampleB {\n"
        "  /var(l) uint8 len;\n"
        "  int16<len:$l> int16Array;\n"
        "}");
    const breco::ParseResult parsed = breco::parseStructDeclaration(declaration);
    expectTrue(parsed.valid, QStringLiteral("Dynamic length declarations should parse"));
    expectTrue(!parsed.graph.staticStructLayoutSizeBytes(QStringLiteral("ExampleA")).has_value(),
               QStringLiteral("Dynamic byte struct should not have a static size"));
    expectTrue(!parsed.graph.staticStructLayoutSizeBytes(QStringLiteral("ExampleB")).has_value(),
               QStringLiteral("Dynamic numeric struct should not have a static size"));

    const QByteArray exampleABytes = QByteArray::fromHex("03AABBCC02DDEE");
    const breco::VisualizedNode exampleA =
        breco::visualize(parsed.graph, QStringLiteral("ExampleA"), exampleABytes, 0, 2);
    expectEqInt(exampleA.children.size(), 2,
                QStringLiteral("Variable byte structs should iterate by decoded size"));
    if (exampleA.children.size() == 2) {
        expectEqInt(exampleA.children.at(0).children.at(1).rawBytes.size(), 3,
                    QStringLiteral("byte<len:$l> should consume l bytes"));
        expectEqInt(exampleA.children.at(1).children.at(1).rawBytes.size(), 2,
                    QStringLiteral("Second variable byte struct should start after first"));
    }

    const QByteArray exampleBBytes = QByteArray::fromHex("0201000200010300");
    const breco::VisualizedNode exampleB =
        breco::visualize(parsed.graph, QStringLiteral("ExampleB"), exampleBBytes, 0, 2);
    expectEqInt(exampleB.children.size(), 2,
                QStringLiteral("Variable POD structs should iterate by decoded size"));
    if (exampleB.children.size() == 2) {
        expectEqInt(exampleB.children.at(0).children.at(1).rawBytes.size(), 4,
                    QStringLiteral("int16<len:2> should consume two 2-byte elements"));
        expectEqInt(exampleB.children.at(0).children.at(1).children.size(), 2,
                    QStringLiteral("Numeric length should be an element count"));
        expectEqInt(exampleB.children.at(1).children.at(1).rawBytes.size(), 2,
                    QStringLiteral("Second numeric array should consume one element"));
    }

    const breco::ParseResult strings = breco::parseStructDeclaration(QStringLiteral(
        "struct Strings { asciistr<max:4> text; uint8 tail; }"));
    expectTrue(strings.valid, QStringLiteral("String max declaration should parse"));
    const QByteArray stringBytes = QByteArray::fromHex("41007F");
    const breco::VisualizedNode stringNode =
        breco::visualize(strings.graph, QStringLiteral("Strings"), stringBytes, 0, 1);
    if (!stringNode.children.isEmpty()) {
        const breco::VisualizedNode& item = stringNode.children.first();
        expectEqQString(item.children.at(0).valueText, QStringLiteral("A"),
                        QStringLiteral("<max> string should stop at NUL"));
        expectEqInt(item.children.at(0).rawBytes.size(), 2,
                    QStringLiteral("<max> string should consume its NUL"));
        expectEqQString(item.children.at(1).rawBytes.toHex().toUpper(),
                        QStringLiteral("7F"),
                        QStringLiteral("Field after max string should start after NUL"));
    }

    const breco::ParseResult utf16 = breco::parseStructDeclaration(QStringLiteral(
        "struct Utf16 { utf16str<le><len:4> text; uint8 tail; }"));
    expectTrue(utf16.valid, QStringLiteral("Endian fixed UTF-16 declaration should parse"));
    const breco::VisualizedNode utf16Node =
        breco::visualize(utf16.graph, QStringLiteral("Utf16"),
                         QByteArray::fromHex("410042007F"), 0, 1);
    if (!utf16Node.children.isEmpty()) {
        const breco::VisualizedNode& item = utf16Node.children.first();
        expectEqQString(item.children.at(0).valueText, QStringLiteral("AB"),
                        QStringLiteral("UTF-16 fixed length should count bytes"));
        expectEqQString(item.children.at(1).rawBytes.toHex().toUpper(),
                        QStringLiteral("7F"),
                        QStringLiteral("UTF-16 fixed length should advance by four bytes"));
    }

    const breco::ParseResult utf8 = breco::parseStructDeclaration(QStringLiteral(
        "struct Utf8 { utf8str text; uint8 tail; }"));
    expectTrue(utf8.valid, QStringLiteral("UTF-8 string declaration should parse"));
    const breco::VisualizedNode utf8Node =
        breco::visualize(utf8.graph, QStringLiteral("Utf8"),
                         QByteArray::fromHex("C3A4007F"), 0, 1);
    if (!utf8Node.children.isEmpty()) {
        const breco::VisualizedNode& item = utf8Node.children.first();
        expectEqQString(item.children.at(0).valueText, QString::fromUtf8("ä"),
                        QStringLiteral("UTF-8 string should decode through NUL"));
        expectEqQString(item.children.at(1).rawBytes.toHex().toUpper(),
                        QStringLiteral("7F"),
                        QStringLiteral("UTF-8 string should consume its NUL"));
    }

    const breco::ParseResult numericMax = breco::parseStructDeclaration(QStringLiteral(
        "struct NumericMax { uint16<max:2> values; uint8 tail; }"));
    expectTrue(numericMax.valid, QStringLiteral("Numeric max declaration should parse"));
    const breco::VisualizedNode numericMaxNode =
        breco::visualize(numericMax.graph, QStringLiteral("NumericMax"),
                         QByteArray::fromHex("01007F"), 0, 1);
    if (!numericMaxNode.children.isEmpty()) {
        const breco::VisualizedNode& item = numericMaxNode.children.first();
        expectEqInt(item.children.at(0).children.size(), 1,
                    QStringLiteral("Numeric max should decode only complete elements"));
        expectEqQString(item.children.at(1).rawBytes.toHex().toUpper(),
                        QStringLiteral("7F"),
                        QStringLiteral("Numeric max should leave an incomplete element for next field"));
    }

    const breco::ParseResult until = breco::parseStructDeclaration(QStringLiteral(
        "struct Until { byte<until:=\"END\"> prefix; asciistr marker; uint8 tail; }"));
    expectTrue(until.valid, QStringLiteral("Until sentinel declaration should parse"));
    const QByteArray untilBytes("abcEND\0", 7);
    QByteArray untilBuffer = untilBytes;
    untilBuffer.push_back(static_cast<char>(0x7f));
    const breco::VisualizedNode untilNode =
        breco::visualize(until.graph, QStringLiteral("Until"), untilBuffer, 0, 1);
    if (!untilNode.children.isEmpty()) {
        const breco::VisualizedNode& item = untilNode.children.first();
        expectEqQString(QString::fromLatin1(item.children.at(0).rawBytes),
                        QStringLiteral("abc"),
                        QStringLiteral("<until> should return bytes before sentinel"));
        expectEqQString(item.children.at(1).valueText, QStringLiteral("END"),
                        QStringLiteral("<until> should leave sentinel for next field"));
        expectEqQString(item.children.at(2).rawBytes.toHex().toUpper(),
                        QStringLiteral("7F"),
                        QStringLiteral("Read position should advance across until and string"));
    }

    const breco::ParseResult numericUntil = breco::parseStructDeclaration(QStringLiteral(
        "struct NumericUntil { uint16<until:=0xFFFF> values; uint16 sentinel; uint8 tail; }"));
    expectTrue(numericUntil.valid,
               QStringLiteral("Numeric until declaration should parse"));
    const breco::VisualizedNode numericUntilNode =
        breco::visualize(numericUntil.graph, QStringLiteral("NumericUntil"),
                         QByteArray::fromHex("01000200FFFF7F"), 0, 1);
    if (!numericUntilNode.children.isEmpty()) {
        const breco::VisualizedNode& item = numericUntilNode.children.first();
        expectEqInt(item.children.at(0).children.size(), 2,
                    QStringLiteral("Numeric until should collect preceding elements"));
        expectEqQString(item.children.at(1).rawBytes.toHex().toUpper(),
                        QStringLiteral("FFFF"),
                        QStringLiteral("Numeric until should not consume matching element"));
        expectEqQString(item.children.at(2).rawBytes.toHex().toUpper(),
                        QStringLiteral("7F"),
                        QStringLiteral("Numeric until struct should continue after sentinel"));
    }

    const QString repeatedDeclaration = QStringLiteral(
        "struct Header { /var(itemCount) uint8 count; }\n"
        "struct Item { /var(itemLen) uint8 len; byte<len:$itemLen> payload; }\n"
        "struct File { Header head; /repeat($head.itemCount) Item; }");
    const breco::ParseResult repeated =
        breco::parseStructDeclaration(repeatedDeclaration);
    expectTrue(repeated.valid, QStringLiteral("Nested variable repeat declaration should parse"));
    const QByteArray repeatedBytes = QByteArray::fromHex("0101AA0201BB02CCDD");
    const breco::VisualizedNode repeatedNode =
        breco::visualize(repeated.graph, QStringLiteral("File"), repeatedBytes, 0, 2);
    expectEqInt(repeatedNode.children.size(), 2,
                QStringLiteral("Variable repeated structs should support file iteration"));
    if (repeatedNode.children.size() == 2) {
        expectEqInt(repeatedNode.children.at(0).children.at(1).children.size(), 1,
                    QStringLiteral("First file should repeat one item"));
        expectEqInt(repeatedNode.children.at(1).children.at(1).children.size(), 2,
                    QStringLiteral("Second file should repeat two items"));

        breco::StructVisualizedTreeModel repeatedModel;
        repeatedModel.setRoot(repeatedNode);
        const QModelIndex fileStruct =
            repeatedModel.index(0, breco::StructVisualizedTreeModel::Name);
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    0, breco::StructVisualizedTreeModel::Type))
                .toString(),
            QStringLiteral("File"),
            QStringLiteral("Struct rows should display their Type"));
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    0, breco::StructVisualizedTreeModel::Value))
                .toString(),
            QString(),
            QStringLiteral("Struct rows should display an empty Value"));
        const QModelIndex nestedStruct =
            repeatedModel.index(0, breco::StructVisualizedTreeModel::Name,
                                fileStruct);
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    0, breco::StructVisualizedTreeModel::Type, fileStruct))
                .toString(),
            QStringLiteral("Header"),
            QStringLiteral("Nested struct rows should display their Type"));
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    0, breco::StructVisualizedTreeModel::Value, fileStruct))
                .toString(),
            QString(),
            QStringLiteral("Nested struct rows should display an empty Value"));
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    0, breco::StructVisualizedTreeModel::Type, nestedStruct))
                .toString(),
            QStringLiteral("uint8"),
            QStringLiteral("Scalar rows should keep their Type"));
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    0, breco::StructVisualizedTreeModel::Value, nestedStruct))
                .toString(),
            QStringLiteral("1 (0X1)"),
            QStringLiteral("Scalar rows should keep their Value"));
        const QModelIndex repeatedItems =
            repeatedModel.index(1, breco::StructVisualizedTreeModel::Name,
                                fileStruct);
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    1, breco::StructVisualizedTreeModel::Type, fileStruct))
                .toString(),
            QString(),
            QStringLiteral("Repeat container rows should display an empty Type"));
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    1, breco::StructVisualizedTreeModel::Value, fileStruct))
                .toString(),
            QStringLiteral("1 item"),
            QStringLiteral("Repeat container rows should keep their item count"));
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    0, breco::StructVisualizedTreeModel::Type, repeatedItems))
                .toString(),
            QStringLiteral("Item"),
            QStringLiteral("Repeated struct elements should display their Type"));
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    0, breco::StructVisualizedTreeModel::Value, repeatedItems))
                .toString(),
            QString(),
            QStringLiteral("Repeated struct elements should display an empty Value"));

        const QModelIndex secondFile =
            repeatedModel.index(1, breco::StructVisualizedTreeModel::Name);
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    1, breco::StructVisualizedTreeModel::Value, secondFile))
                .toString(),
            QStringLiteral("2 items"),
            QStringLiteral("Repeat containers should pluralize multiple items"));

        breco::VisualizedNode emptyRoot;
        emptyRoot.name = QStringLiteral("root");
        breco::VisualizedNode emptyRepeated =
            repeatedNode.children.at(0).children.at(1);
        emptyRepeated.children.clear();
        emptyRoot.children.push_back(emptyRepeated);
        repeatedModel.setRoot(emptyRoot);
        expectEqQString(
            repeatedModel
                .data(repeatedModel.index(
                    0, breco::StructVisualizedTreeModel::Value))
                .toString(),
            QStringLiteral("(empty)"),
            QStringLiteral("Empty repeat containers should display (empty)"));
    }

    const breco::ParseResult condition = breco::parseStructDeclaration(QStringLiteral(
        "struct Checked { /cond(= 0x42) uint8 magic; uint8 tail; }"));
    expectTrue(condition.valid, QStringLiteral("Condition declaration should parse"));
    const breco::VisualizedNode conditionFail =
        breco::visualize(condition.graph, QStringLiteral("Checked"),
                         QByteArray::fromHex("41FF"), 0, 1);
    if (!conditionFail.children.isEmpty()) {
        expectTrue(!conditionFail.children.first().valid,
                   QStringLiteral("Failed condition should invalidate containing struct"));
        expectEqInt(conditionFail.children.first().children.size(), 1,
                    QStringLiteral("Failed condition should stop containing struct"));

        breco::StructVisualizedTreeModel conditionModel;
        conditionModel.setRoot(conditionFail);
        const QModelIndex containingStruct =
            conditionModel.index(0, breco::StructVisualizedTreeModel::Name);
        expectEqQString(
            conditionModel
                .data(conditionModel.index(
                    0, breco::StructVisualizedTreeModel::Value))
                .toString(),
            QString(),
            QStringLiteral("Struct value should remain empty for member condition failures"));
        expectTrue(
            conditionModel
                .data(conditionModel.index(
                    0, breco::StructVisualizedTreeModel::Value,
                    containingStruct))
                .toString()
                .contains(QStringLiteral("/cond expression failed")),
            QStringLiteral("Invalid member value should retain its condition failure"));
    }
    const breco::VisualizedNode conditionPass =
        breco::visualize(condition.graph, QStringLiteral("Checked"),
                         QByteArray::fromHex("42FF"), 0, 1);
    if (!conditionPass.children.isEmpty()) {
        expectTrue(conditionPass.children.first().valid,
                   QStringLiteral("Matching condition should keep struct valid"));
        expectEqInt(conditionPass.children.first().children.size(), 2,
                    QStringLiteral("Matching condition should continue reading"));
    }

    const breco::ParseResult decoratedCondition =
        breco::parseStructDeclaration(QStringLiteral(
            "struct DecoratedChecked { "
            "/cond(=\"PNG\") asciistr<len:3> signature; uint8 tail; }"));
    expectTrue(decoratedCondition.valid,
               QStringLiteral("Condition on a fixed-length string should parse"));
    const breco::VisualizedNode decoratedConditionPass =
        breco::visualize(decoratedCondition.graph,
                         QStringLiteral("DecoratedChecked"),
                         QByteArray::fromHex("504E477F"), 0, 1);
    if (!decoratedConditionPass.children.isEmpty()) {
        const breco::VisualizedNode& item =
            decoratedConditionPass.children.first();
        expectTrue(item.valid,
                   QStringLiteral("Matching decorated field should keep struct valid"));
        expectEqInt(item.children.size(), 2,
                    QStringLiteral("Matching decorated field should continue reading"));
    }
    const breco::VisualizedNode decoratedConditionFail =
        breco::visualize(decoratedCondition.graph,
                         QStringLiteral("DecoratedChecked"),
                         QByteArray::fromHex("504E587F"), 0, 1);
    if (!decoratedConditionFail.children.isEmpty()) {
        const breco::VisualizedNode& item =
            decoratedConditionFail.children.first();
        expectTrue(!item.valid,
                   QStringLiteral("Failed decorated condition should invalidate struct"));
        expectEqInt(item.children.size(), 1,
                    QStringLiteral("Failed decorated condition should stop struct"));
    }
    const breco::VisualizedNode decoratedConditionShort =
        breco::visualize(decoratedCondition.graph,
                         QStringLiteral("DecoratedChecked"),
                         QByteArray::fromHex("504E"), 0, 1);
    if (!decoratedConditionShort.children.isEmpty()) {
        expectTrue(!decoratedConditionShort.children.first().valid,
                   QStringLiteral("Incomplete decorated condition should invalidate struct"));
    }

    const breco::ParseResult byteSequenceCondition =
        breco::parseStructDeclaration(QStringLiteral(
            "struct ByteChecked { "
            "/cond(=\"PNG\") byte<len:3> signature; uint8 tail; }"));
    expectTrue(byteSequenceCondition.valid,
               QStringLiteral("Condition on a fixed byte sequence should parse"));
    const breco::VisualizedNode byteSequenceConditionNode =
        breco::visualize(byteSequenceCondition.graph,
                         QStringLiteral("ByteChecked"),
                         QByteArray::fromHex("504E477F"), 0, 1);
    if (!byteSequenceConditionNode.children.isEmpty()) {
        expectTrue(byteSequenceConditionNode.children.first().valid,
                   QStringLiteral("Byte sequence should compare with UTF-8 string bytes"));
    }

    const breco::ParseResult booleanCondition =
        breco::parseStructDeclaration(QStringLiteral(
            "struct BooleanChecked { "
            "/cond(true) uint8 set; /cond(false) uint8 clear; uint8 tail; }"));
    expectTrue(booleanCondition.valid,
               QStringLiteral("Boolean conditions should parse"));
    const breco::VisualizedNode booleanConditionPass =
        breco::visualize(booleanCondition.graph,
                         QStringLiteral("BooleanChecked"),
                         QByteArray::fromHex("01007F"), 0, 1);
    if (!booleanConditionPass.children.isEmpty()) {
        expectTrue(booleanConditionPass.children.first().valid,
                   QStringLiteral("Boolean conditions should accept nonzero and zero"));
        expectEqInt(booleanConditionPass.children.first().children.size(), 3,
                    QStringLiteral("Matching Boolean conditions should continue reading"));
    }
    const breco::VisualizedNode trueConditionFail =
        breco::visualize(booleanCondition.graph,
                         QStringLiteral("BooleanChecked"),
                         QByteArray::fromHex("00007F"), 0, 1);
    if (!trueConditionFail.children.isEmpty()) {
        expectTrue(!trueConditionFail.children.first().valid,
                   QStringLiteral("/cond(true) should reject zero"));
        expectEqInt(trueConditionFail.children.first().children.size(), 1,
                    QStringLiteral("Failed true condition should stop struct"));
    }
    const breco::VisualizedNode falseConditionFail =
        breco::visualize(booleanCondition.graph,
                         QStringLiteral("BooleanChecked"),
                         QByteArray::fromHex("01017F"), 0, 1);
    if (!falseConditionFail.children.isEmpty()) {
        expectTrue(!falseConditionFail.children.first().valid,
                   QStringLiteral("/cond(false) should reject nonzero"));
        expectEqInt(falseConditionFail.children.first().children.size(), 2,
                    QStringLiteral("Failed false condition should stop struct"));
    }

    const breco::ParseResult nestedConditions =
        breco::parseStructDeclaration(QStringLiteral(
            "struct Probe { /cond(=0x42) uint8 magic; uint8 tail; }\n"
            "struct UncheckedProbe { Probe probe; uint8 outerTail; }\n"
            "struct CheckedProbe { /cond(true) Probe probe; uint8 outerTail; }\n"
            "struct ExpectedInvalidProbe { /cond(false) Probe probe; uint8 outerTail; }\n"
            "struct MiddleProbe { /cond(true) Probe probe; uint8 middleTail; }\n"
            "struct TopProbe { MiddleProbe middle; uint8 topTail; }"));
    expectTrue(nestedConditions.valid,
               QStringLiteral("Boolean struct conditions should parse"));
    const QByteArray invalidProbeBytes = QByteArray::fromHex("417F");
    const breco::VisualizedNode uncheckedProbe =
        breco::visualize(nestedConditions.graph,
                         QStringLiteral("UncheckedProbe"),
                         invalidProbeBytes, 0, 1);
    if (!uncheckedProbe.children.isEmpty()) {
        const breco::VisualizedNode& item = uncheckedProbe.children.first();
        expectTrue(item.valid,
                   QStringLiteral("Unconditioned invalid child should not invalidate parent"));
        expectTrue(!item.children.at(0).valid,
                   QStringLiteral("Contained child should remain visibly invalid"));
        expectEqInt(item.children.size(), 2,
                    QStringLiteral("Contained child failure should not stop parent"));
    }
    const breco::VisualizedNode checkedProbe =
        breco::visualize(nestedConditions.graph,
                         QStringLiteral("CheckedProbe"),
                         invalidProbeBytes, 0, 1);
    if (!checkedProbe.children.isEmpty()) {
        const breco::VisualizedNode& item = checkedProbe.children.first();
        expectTrue(!item.valid,
                   QStringLiteral("/cond(true) should carry child invalidity to parent"));
        expectEqInt(item.children.size(), 1,
                    QStringLiteral("Carried child invalidity should stop parent"));
    }
    const breco::VisualizedNode expectedInvalidProbe =
        breco::visualize(nestedConditions.graph,
                         QStringLiteral("ExpectedInvalidProbe"),
                         invalidProbeBytes, 0, 1);
    if (!expectedInvalidProbe.children.isEmpty()) {
        const breco::VisualizedNode& item =
            expectedInvalidProbe.children.first();
        expectTrue(item.valid,
                   QStringLiteral("/cond(false) should accept an invalid child"));
        expectEqInt(item.children.size(), 2,
                    QStringLiteral("Expected invalid child should not stop parent"));
    }
    const breco::VisualizedNode stoppedPropagation =
        breco::visualize(nestedConditions.graph,
                         QStringLiteral("TopProbe"),
                         invalidProbeBytes, 0, 1);
    if (!stoppedPropagation.children.isEmpty()) {
        const breco::VisualizedNode& item =
            stoppedPropagation.children.first();
        expectTrue(item.valid,
                   QStringLiteral("Nested invalidity should stop after one conditioned level"));
        expectTrue(!item.children.at(0).valid,
                   QStringLiteral("Invalid intermediate struct should remain visible"));
        expectEqInt(item.children.size(), 2,
                    QStringLiteral("Unconditioned outer struct should continue"));
    }

    const breco::ParseResult arithmetic = breco::parseStructDeclaration(QStringLiteral(
        "struct Arithmetic {"
        " /var(n) uint8 count;"
        " byte<len:$n + 1> payload;"
        " /repeat($n * 2) uint8 values;"
        " /cond(=$n * 2 + 1) uint8 check;"
        "}"));
    expectTrue(arithmetic.valid, QStringLiteral("Arithmetic expressions should parse"));
    const breco::VisualizedNode arithmeticNode =
        breco::visualize(arithmetic.graph, QStringLiteral("Arithmetic"),
                         QByteArray::fromHex("02AABBCC0102030405"), 0, 1);
    if (!arithmeticNode.children.isEmpty()) {
        const breco::VisualizedNode& item = arithmeticNode.children.first();
        expectTrue(item.valid,
                   QStringLiteral("Arithmetic condition should validate decoded value"));
        expectEqInt(item.children.at(1).rawBytes.size(), 3,
                    QStringLiteral("Arithmetic byte length should consume n + 1 bytes"));
        expectEqInt(item.children.at(2).children.size(), 4,
                    QStringLiteral("Arithmetic repeat should decode n * 2 values"));
    }

    const breco::ParseResult assertions = breco::parseStructDeclaration(QStringLiteral(
        "struct Asserted {"
        " /var(a) uint8 a;"
        " /var(b) uint8 b;"
        " uint8 tail;"
        " /assert($b = $a + 1);"
        "}\n"
        "struct DivZero {"
        " /var(a) uint8 a;"
        " /assert($a = 1 / 0);"
        "}"));
    expectTrue(assertions.valid, QStringLiteral("Struct assertions should parse"));
    const breco::VisualizedNode assertionPass =
        breco::visualize(assertions.graph, QStringLiteral("Asserted"),
                         QByteArray::fromHex("02037F"), 0, 1);
    if (!assertionPass.children.isEmpty()) {
        const breco::VisualizedNode& item = assertionPass.children.first();
        expectTrue(item.valid, QStringLiteral("Passing /assert should keep struct valid"));
        expectEqInt(item.children.size(), 4,
                    QStringLiteral("/assert should append a condition node"));
        expectTrue(item.children.last().hasCondition && item.children.last().valid,
                   QStringLiteral("Passing /assert node should be condition-valid"));
    }
    const breco::VisualizedNode assertionFail =
        breco::visualize(assertions.graph, QStringLiteral("Asserted"),
                         QByteArray::fromHex("02047F"), 0, 1);
    if (!assertionFail.children.isEmpty()) {
        const breco::VisualizedNode& item = assertionFail.children.first();
        expectTrue(!item.valid, QStringLiteral("Failing /assert should invalidate struct"));
        expectEqInt(item.children.size(), 4,
                    QStringLiteral("Failing /assert should leave decoded members visible"));
        expectTrue(!item.children.last().valid,
                   QStringLiteral("Failing /assert node should be invalid"));
    }
    const breco::VisualizedNode assertionDivZero =
        breco::visualize(assertions.graph, QStringLiteral("DivZero"),
                         QByteArray::fromHex("01"), 0, 1);
    if (!assertionDivZero.children.isEmpty()) {
        expectTrue(assertionDivZero.children.first().children.last().errorMessage
                       .contains(QStringLiteral("Division by zero")),
                   QStringLiteral("/assert should surface arithmetic errors"));
    }

    const breco::ParseResult whenDecl = breco::parseStructDeclaration(QStringLiteral(
        "struct Conditional {"
        " /var(kind) uint8 kind;"
        " /when($kind = 1) uint8 optional;"
        " uint8 tail;"
        "}"));
    expectTrue(whenDecl.valid, QStringLiteral("/when declaration should parse"));
    const breco::VisualizedNode whenTrue =
        breco::visualize(whenDecl.graph, QStringLiteral("Conditional"),
                         QByteArray::fromHex("01AA7F"), 0, 1);
    if (!whenTrue.children.isEmpty()) {
        const breco::VisualizedNode& item = whenTrue.children.first();
        expectEqInt(item.children.size(), 3,
                    QStringLiteral("True /when should decode optional field"));
        expectEqQString(item.children.at(1).name, QStringLiteral("optional"),
                        QStringLiteral("True /when field should appear in tree"));
    }
    const breco::VisualizedNode whenFalse =
        breco::visualize(whenDecl.graph, QStringLiteral("Conditional"),
                         QByteArray::fromHex("007F"), 0, 1);
    if (!whenFalse.children.isEmpty()) {
        const breco::VisualizedNode& item = whenFalse.children.first();
        expectEqInt(item.children.size(), 2,
                    QStringLiteral("False /when should omit optional field"));
        expectEqQString(item.children.at(1).name, QStringLiteral("tail"),
                        QStringLiteral("Field after false /when should decode at same offset"));
        expectEqQString(item.children.at(1).rawBytes.toHex().toUpper(),
                        QStringLiteral("7F"),
                        QStringLiteral("False /when should consume zero bytes"));
    }

    const breco::ParseResult bitfields = breco::parseStructDeclaration(QStringLiteral(
        "struct Flags {"
        " uint8 flags {"
        "  bit 0 low;"
        "  bits 3:1 mid;"
        "  bit 7 top;"
        " }"
        " uint8 tail;"
        "}"));
    expectTrue(bitfields.valid, QStringLiteral("Bitfield block should parse"));
    const breco::VisualizedNode bitfieldNode =
        breco::visualize(bitfields.graph, QStringLiteral("Flags"),
                         QByteArray::fromHex("8B7F"), 0, 1);
    if (!bitfieldNode.children.isEmpty()) {
        const breco::VisualizedNode& flags = bitfieldNode.children.first().children.first();
        expectEqInt(flags.children.size(), 3,
                    QStringLiteral("Bitfields should become display children"));
        expectEqQString(flags.children.at(0).valueText, QStringLiteral("1 (0X1)"),
                        QStringLiteral("Single bit should decode as unsigned value"));
        expectEqQString(flags.children.at(1).valueText, QStringLiteral("5 (0X5)"),
                        QStringLiteral("Bit range should decode inclusive high:low value"));
        expectEqQString(flags.children.at(2).valueText, QStringLiteral("1 (0X1)"),
                        QStringLiteral("High bit should decode as unsigned value"));
    }
    expectTrue(!breco::parseStructDeclaration(QStringLiteral(
                    "struct BadBits { uint8 flags { bits 2:0 a; bit 1 b; } }"))
                    .valid,
               QStringLiteral("Overlapping bitfields should be rejected"));
    expectTrue(!breco::parseStructDeclaration(QStringLiteral(
                    "struct WideBits { uint8 flags { bit 8 tooWide; } }"))
                    .valid,
               QStringLiteral("Out-of-width bitfields should be rejected"));

    const QString conditionalFrameDeclaration = QStringLiteral(
        "struct SampleWord16 { uint16<le> first; uint16<le> second; }\n"
        "struct DataBlock16 {\n"
        " uint32<le> statusWord { bit 0 invalidity; }\n"
        " /repeat($blockLengthWords) SampleWord16 words;\n"
        "}\n"
        "struct ExtendedHeaderTail {\n"
        " uint64<le> timestamp;\n"
        " uint32<le> metadata;\n"
        "}\n"
        "struct ConditionalFrame16 {\n"
        " /cond(=0xA1B2C3D4) uint32<le> magicWord;\n"
        " /var(frameLengthWords) uint32<le> frameLengthWords;\n"
        " uint32<le> frameCount;\n"
        " /cond(=2) uint32<le> frameType;\n"
        " /var(headerLengthWords) uint32<le> headerLengthWords;\n"
        " uint32<le> frameReserved;\n"
        " /var(blockCount) uint32<le> blockCount;\n"
        " /var(blockLengthWords) uint32<le> blockLengthWords;\n"
        " uint32<le> headerValue;\n"
        " /when($headerLengthWords = 6) ExtendedHeaderTail extended;\n"
        " /repeat($blockCount) DataBlock16 dataBlocks;\n"
        " /assert($frameLengthWords = 6 + $headerLengthWords +"
        " $blockCount * (1 + $blockLengthWords));\n"
        "}");
    const breco::ParseResult conditionalFrame =
        breco::parseStructDeclaration(conditionalFrameDeclaration);
    expectTrue(conditionalFrame.valid,
               QStringLiteral("Conditional frame declaration should parse"));

    const auto appendU16LE = [](QByteArray* bytes, quint16 value) {
        bytes->push_back(static_cast<char>(value & 0xffU));
        bytes->push_back(static_cast<char>((value >> 8U) & 0xffU));
    };
    const auto appendU32LE = [](QByteArray* bytes, quint32 value) {
        for (int i = 0; i < 4; ++i) {
            bytes->push_back(static_cast<char>((value >> (8U * i)) & 0xffU));
        }
    };
    const auto appendU64LE = [](QByteArray* bytes, quint64 value) {
        for (int i = 0; i < 8; ++i) {
            bytes->push_back(static_cast<char>((value >> (8U * i)) & 0xffU));
        }
    };
    const auto makeConditionalFrame = [&](quint32 headerLengthWords,
                                          quint32 frameLengthWords) {
        QByteArray bytes;
        appendU32LE(&bytes, 0xA1B2C3D4U);
        appendU32LE(&bytes, frameLengthWords);
        appendU32LE(&bytes, 1U);
        appendU32LE(&bytes, 2U);
        appendU32LE(&bytes, headerLengthWords);
        appendU32LE(&bytes, 0U);
        appendU32LE(&bytes, 1U);
        appendU32LE(&bytes, 2U);
        appendU32LE(&bytes, 123U);
        if (headerLengthWords == 6U) {
            appendU64LE(&bytes, 456U);
            appendU32LE(&bytes, 7U);
        }
        appendU32LE(&bytes, 1U);
        appendU16LE(&bytes, 1U);
        appendU16LE(&bytes, 0xffffU);
        appendU16LE(&bytes, 2U);
        appendU16LE(&bytes, 0xfffeU);
        return bytes;
    };
    const breco::VisualizedNode baseFrame =
        breco::visualize(conditionalFrame.graph,
                         QStringLiteral("ConditionalFrame16"),
                         makeConditionalFrame(3U, 12U), 0, 1);
    if (!baseFrame.children.isEmpty()) {
        const breco::VisualizedNode& frame = baseFrame.children.first();
        bool sawExtended = false;
        for (const breco::VisualizedNode& child : frame.children) {
            sawExtended = sawExtended || child.name == QStringLiteral("extended");
        }
        expectTrue(frame.valid,
                   QStringLiteral("Base conditional frame should validate"));
        expectTrue(!sawExtended,
                   QStringLiteral("Base frame should omit /when extended tail"));
        expectTrue(frame.children.last().hasCondition && frame.children.last().valid,
                   QStringLiteral("Conditional frame length /assert should pass"));
    }
    const breco::VisualizedNode extendedFrame =
        breco::visualize(conditionalFrame.graph,
                         QStringLiteral("ConditionalFrame16"),
                         makeConditionalFrame(6U, 15U), 0, 1);
    if (!extendedFrame.children.isEmpty()) {
        const breco::VisualizedNode& frame = extendedFrame.children.first();
        bool sawExtended = false;
        for (const breco::VisualizedNode& child : frame.children) {
            sawExtended = sawExtended || child.name == QStringLiteral("extended");
        }
        expectTrue(frame.valid,
                   QStringLiteral("Extended conditional frame should validate"));
        expectTrue(sawExtended,
                   QStringLiteral("Extended frame should decode /when tail"));
    }
    const breco::VisualizedNode badLengthFrame =
        breco::visualize(conditionalFrame.graph,
                         QStringLiteral("ConditionalFrame16"),
                         makeConditionalFrame(3U, 11U), 0, 1);
    if (!badLengthFrame.children.isEmpty()) {
        const breco::VisualizedNode& frame = badLengthFrame.children.first();
        expectTrue(!frame.valid,
                   QStringLiteral("Frame length mismatch should fail /assert"));
        expectTrue(!frame.children.last().valid,
                   QStringLiteral("Failing frame /assert node should be invalid"));
    }

    const breco::ParseResult invalidVar = breco::parseStructDeclaration(
        QStringLiteral("struct Bad { /var(s) asciistr text; }"));
    expectTrue(!invalidVar.valid,
               QStringLiteral("/var should reject non-scalar string fields"));
    const breco::ParseResult invalidStructLength = breco::parseStructDeclaration(
        QStringLiteral("struct Inner { uint8 x; } struct Bad { Inner<len:2> values; }"));
    expectTrue(!invalidStructLength.valid,
               QStringLiteral("Length modifiers should reject struct fields"));
    const breco::ParseResult invalidDuplicateLength = breco::parseStructDeclaration(
        QStringLiteral("byte<len:2><max:4> data;"));
    expectTrue(!invalidDuplicateLength.valid,
               QStringLiteral("Parser should reject duplicate length modifiers"));

    const breco::ParseResult missingVariable = breco::parseStructDeclaration(
        QStringLiteral("struct Missing { byte<len:$unknown> data; uint8 tail; }"));
    expectTrue(missingVariable.valid,
               QStringLiteral("Unresolved variables are a decode-time error"));
    const breco::VisualizedNode missingVariableNode =
        breco::visualize(missingVariable.graph, QStringLiteral("Missing"),
                         QByteArray::fromHex("0102"), 0, 1);
    if (!missingVariableNode.children.isEmpty()) {
        expectTrue(!missingVariableNode.children.first().valid,
                   QStringLiteral("Unknown runtime variable should invalidate struct"));
        expectEqInt(missingVariableNode.children.first().children.size(), 1,
                    QStringLiteral("Unknown runtime variable should stop struct"));
    }

    const breco::ParseResult byteVariable = breco::parseStructDeclaration(
        QStringLiteral("struct ByteVariable { /var(l) byte len; byte<len:$l> data; }"));
    expectTrue(byteVariable.valid,
               QStringLiteral("Scalar byte should be bindable with /var"));
    const breco::VisualizedNode byteVariableNode =
        breco::visualize(byteVariable.graph, QStringLiteral("ByteVariable"),
                         QByteArray::fromHex("02AABB"), 0, 1);
    if (!byteVariableNode.children.isEmpty()) {
        expectEqInt(byteVariableNode.children.first().children.at(1).rawBytes.size(), 2,
                    QStringLiteral("Bound byte should drive dynamic byte length"));
    }

    const breco::ParseResult zeroRepeat = breco::parseStructDeclaration(
        QStringLiteral("struct ZeroRepeat { /repeat(0) uint8; uint8 tail; }"));
    expectTrue(zeroRepeat.valid, QStringLiteral("Zero repeat declaration should parse"));
    const breco::VisualizedNode zeroRepeatNode =
        breco::visualize(zeroRepeat.graph, QStringLiteral("ZeroRepeat"),
                         QByteArray::fromHex("7F"), 0, 1);
    if (!zeroRepeatNode.children.isEmpty()) {
        expectEqQString(zeroRepeatNode.children.first().children.at(1).rawBytes.toHex().toUpper(),
                        QStringLiteral("7F"),
                        QStringLiteral("Zero repeat should consume no bytes"));
    }

    const breco::ParseResult truncatedRepeat = breco::parseStructDeclaration(
        QStringLiteral("struct Truncated { /repeat(2) uint16 values; }"));
    expectTrue(truncatedRepeat.valid,
               QStringLiteral("Truncated repeat declaration should parse"));
    const breco::VisualizedNode truncatedRepeatNode =
        breco::visualize(truncatedRepeat.graph, QStringLiteral("Truncated"),
                         QByteArray::fromHex("0100"), 0, 1);
    if (!truncatedRepeatNode.children.isEmpty()) {
        const breco::VisualizedNode& item = truncatedRepeatNode.children.first();
        expectTrue(!item.valid,
                   QStringLiteral("Short repeat buffer should invalidate struct"));
        expectEqInt(item.bytesMissing, 2,
                    QStringLiteral("Short repeat should report missing item bytes"));
    }

    const breco::ParseResult reservedTypeName = breco::parseStructDeclaration(
        QStringLiteral("struct byte { uint8 value; }"));
    expectTrue(!reservedTypeName.valid,
               QStringLiteral("Built-in type names should be reserved"));

    const breco::ParseResult uint64Condition = breco::parseStructDeclaration(
        QStringLiteral("struct U64 { /cond(=0xFFFFFFFFFFFFFFFF) uint64 value; }"));
    expectTrue(uint64Condition.valid,
               QStringLiteral("Full-width uint64 condition literal should parse"));
    const breco::VisualizedNode uint64ConditionNode =
        breco::visualize(uint64Condition.graph, QStringLiteral("U64"),
                         QByteArray::fromHex("FFFFFFFFFFFFFFFF"), 0, 1);
    if (!uint64ConditionNode.children.isEmpty()) {
        expectTrue(uint64ConditionNode.children.first().valid,
                   QStringLiteral("uint64 condition should preserve unsigned values"));
    }

    const breco::ParseResult oddUtf16 = breco::parseStructDeclaration(
        QStringLiteral("struct OddUtf16 { utf16str<len:3> text; }"));
    expectTrue(oddUtf16.valid, QStringLiteral("Odd UTF-16 byte length should parse"));
    const breco::VisualizedNode oddUtf16Node =
        breco::visualize(oddUtf16.graph, QStringLiteral("OddUtf16"),
                         QByteArray::fromHex("410042"), 0, 1);
    if (!oddUtf16Node.children.isEmpty()) {
        expectTrue(!oddUtf16Node.children.first().valid,
                   QStringLiteral("Odd UTF-16 byte length should invalidate struct"));
    }

    const breco::ParseResult duplicateMember = breco::parseStructDeclaration(
        QStringLiteral("struct Duplicate { uint8 value; uint16 value; }"));
    expectTrue(!duplicateMember.valid,
               QStringLiteral("Duplicate member names should be rejected"));
    const breco::ParseResult duplicateVariable = breco::parseStructDeclaration(
        QStringLiteral("struct DuplicateVar { /var(n) uint8 a; /var(n) uint8 b; }"));
    expectTrue(!duplicateVariable.valid,
               QStringLiteral("Duplicate variable bindings should be rejected"));

    const breco::ParseResult stringLength = breco::parseStructDeclaration(
        QStringLiteral("byte<len:\"invalid\"> data;"));
    expectTrue(!stringLength.valid,
               QStringLiteral("String literal length should fail during parsing"));
    const breco::ParseResult stringRepeat = breco::parseStructDeclaration(
        QStringLiteral("/repeat(\"invalid\") uint8 values;"));
    expectTrue(!stringRepeat.valid,
               QStringLiteral("String literal repeat should fail during parsing"));

    QString overflowDeclaration =
        QStringLiteral("struct Overflow0 { uint64 a; }");
    for (int i = 1; i <= 29; ++i) {
        overflowDeclaration +=
            QStringLiteral(" struct Overflow%1 { Overflow%2 a; Overflow%2 b; }")
                .arg(i)
                .arg(i - 1);
    }
    const breco::ParseResult overflow =
        breco::parseStructDeclaration(overflowDeclaration);
    expectTrue(overflow.valid,
               QStringLiteral("Deep fixed layout declaration should parse"));
    expectTrue(!overflow.graph
                    .staticStructLayoutSizeBytes(QStringLiteral("Overflow29"))
                    .has_value(),
               QStringLiteral("Overflowing static layout should return unknown"));

    const breco::ParseResult entryCountCompatibility =
        breco::parseStructDeclaration(QStringLiteral("uint8 value;"));
    const breco::VisualizedNode zeroEntryCount =
        breco::visualize(entryCountCompatibility.graph, QStringLiteral("value"),
                         QByteArray::fromHex("01"), 0, 0);
    expectEqInt(zeroEntryCount.children.size(), 1,
                QStringLiteral("Non-positive entry count should still decode one entry"));
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

void testStructureFeatureServices() {
    QTemporaryDir dir;
    expectTrue(dir.isValid(), QStringLiteral("Structure feature temp directory"));
    QFile formats(dir.filePath(QStringLiteral("formats.brecoscript")));
    expectTrue(formats.open(QIODevice::WriteOnly), QStringLiteral("Open included outforms"));
    formats.write(
        "outform summary binary {\\{\\{literal\\}\\} {{name}}:{{#children}}{{name}}={{value}}{{/children}}}\n");
    formats.close();
    QFile alternateFormats(dir.filePath(QStringLiteral("alternate.brecoscript")));
    expectTrue(alternateFormats.open(QIODevice::WriteOnly),
               QStringLiteral("Open alternate included outforms"));
    alternateFormats.write("outform summary text {alternate {{name}}}\n");
    alternateFormats.close();
    QFile common(dir.filePath(QStringLiteral("common.brecostruct")));
    expectTrue(common.open(QIODevice::WriteOnly), QStringLiteral("Open included definition"));
    common.write(
        "include \"formats.brecoscript\";\n"
        "struct Header { /cond(=0x42) uint8 magic; };\n");
    common.close();
    QFile mainFile(dir.filePath(QStringLiteral("main.brecostruct")));
    expectTrue(mainFile.open(QIODevice::WriteOnly), QStringLiteral("Open including definition"));
    mainFile.write(
        "include \"common.brecostruct\";\n"
        "include \"alternate.brecoscript\";\n"
        "struct Packet { Header header; uint8 tail; };\n");
    mainFile.close();

    const breco::ParseResult parsed = breco::parseStructDeclarationFile(mainFile.fileName());
    expectTrue(parsed.valid, QStringLiteral("File parser resolves relative includes"));
    expectTrue(parsed.graph.entryNames().contains(QStringLiteral("Packet")),
               QStringLiteral("Included graph exposes local entries"));
    expectEqInt(parsed.graph.outforms().size(), 2,
                QStringLiteral("Included graph exposes direct and transitive outforms"));
    if (const breco::OutformNode* outform = parsed.graph.findOutform(
            QStringLiteral("summary"))) {
        expectTrue(outform->mode == breco::OutformMode::Binary,
                   QStringLiteral("Parser stores binary outform mode"));
        expectTrue(outform->templateText.contains(QStringLiteral("{{literal}}")),
                   QStringLiteral("Escaped template braces are retained literally"));
        expectEqQString(QFileInfo(outform->sourceFilePath).fileName(),
                        QStringLiteral("formats.brecoscript"),
                        QStringLiteral("Outform retains its declaring source file"));
    } else {
        expectTrue(false, QStringLiteral("Named included outform is discoverable"));
    }
    if (parsed.graph.outforms().size() == 2) {
        expectEqQString(parsed.graph.outforms().at(1).name,
                        QStringLiteral("summary"),
                        QStringLiteral("Same outform name is allowed from another file"));
        expectEqQString(QFileInfo(parsed.graph.outforms().at(1).sourceFilePath).fileName(),
                        QStringLiteral("alternate.brecoscript"),
                        QStringLiteral("Second outform retains distinct source provenance"));
    }
    const breco::ParseResult balancedOutform = breco::parseStructDeclaration(
        QStringLiteral("outform json text { {\"name\": \"{{name}}\"} } uint8 item;"));
    expectTrue(balancedOutform.valid,
               QStringLiteral("Outform parser accepts balanced literal braces"));
    const breco::ParseResult invalidOutform = breco::parseStructDeclaration(
        QStringLiteral("outform bad html {x} uint8 item;"));
    expectTrue(!invalidOutform.valid &&
                   invalidOutform.errorMessage.contains(QStringLiteral("mode")),
               QStringLiteral("Outform parser rejects unknown modes"));
    const breco::ParseResult scanConstraints = breco::parseStructDeclaration(
        QStringLiteral("uint8 unconstrained; "
                       "/cond(=0x42) uint8 marker; "
                       "struct Asserted { /var(v) uint8 value; /assert($v = 1); } "
                       "typedef Asserted AssertedAlias; "
                       "struct Nested { Asserted child; } "
                       "struct Propagated { /cond(true) Asserted child; }"));
    expectTrue(scanConstraints.valid,
               QStringLiteral("Scan constraint declarations parse"));
    expectTrue(!scanConstraints.graph.entryHasEffectiveScanConstraint(
                   QStringLiteral("unconstrained")),
               QStringLiteral("Unconstrained standalone entry cannot scan"));
    expectTrue(scanConstraints.graph.entryHasEffectiveScanConstraint(
                   QStringLiteral("marker")),
               QStringLiteral("Standalone /cond entry can scan"));
    expectTrue(scanConstraints.graph.entryHasEffectiveScanConstraint(
                   QStringLiteral("Asserted")) &&
                   scanConstraints.graph.entryHasEffectiveScanConstraint(
                       QStringLiteral("AssertedAlias")),
               QStringLiteral("Struct /assert and typedef target can scan"));
    expectTrue(!scanConstraints.graph.entryHasEffectiveScanConstraint(
                   QStringLiteral("Nested")) &&
                   scanConstraints.graph.entryHasEffectiveScanConstraint(
                       QStringLiteral("Propagated")),
               QStringLiteral("Nested constraints require normal propagation"));
    const QVector<breco::StructureScanMatch> matches = breco::scanForStructure(
        parsed.graph, QStringLiteral("Header"), QByteArray::fromHex("00420042"), 100);
    expectEqInt(matches.size(), 2, QStringLiteral("Structure scan finds condition matches"));
    if (matches.size() == 2) {
        expectTrue(matches.at(0).offset == 101 && matches.at(1).offset == 103,
                   QStringLiteral("Structure scan reports absolute offsets"));
    }

    const QVector<breco::StructureLibraryFile> files = breco::StructureLibrary(dir.path()).scan();
    expectEqInt(files.size(), 4, QStringLiteral("Library discovers definition files"));


    breco::VisualizedNode parent;
    parent.name = QStringLiteral("packet");
    breco::VisualizedNode child;
    child.name = QStringLiteral("magic");
    child.valueText = QStringLiteral("66");
    parent.children.push_back(child);
    QString error;
    const QString rendered = breco::renderStructureTemplate(
        QStringLiteral("{{name}}:{{#children}}{{name}}={{value}}{{/children}}"), parent, &error);
    expectEqQString(rendered, QStringLiteral("packet:magic=66"),
                    QStringLiteral("Structure export template renders children"));
    expectTrue(error.isEmpty(), QStringLiteral("Valid export template has no error"));

    const breco::ParseResult externalParsed = breco::parseStructDeclaration(
        QStringLiteral("/external(index); struct Split { "
                       "uint8 primary; /source(index) uint16 externalValue; };"));
    expectTrue(externalParsed.valid,
               QStringLiteral("Parser accepts external source roles"));
    expectTrue(externalParsed.graph.externalRoles() == QStringList{QStringLiteral("index")},
               QStringLiteral("Graph retains external roles"));
    if (externalParsed.valid) {
        breco::VisualizationSource primary{QByteArray::fromHex("AA"),
                                           QStringLiteral("primary.bin"), 10};
        QHash<QString, breco::VisualizationSource> externalSources;
        externalSources.insert(QStringLiteral("index"),
                               {QByteArray::fromHex("3412"),
                                QStringLiteral("index.bin"), 20});
        const breco::VisualizedNode externalRoot = breco::visualize(
            externalParsed.graph, QStringLiteral("Split"), primary, 0, 1,
            externalSources);
        expectTrue(externalRoot.children.size() == 1 &&
                       externalRoot.children.first().valid,
                   QStringLiteral("Visualizer decodes bound external fields"));
        if (!externalRoot.children.isEmpty() &&
            externalRoot.children.first().children.size() == 2) {
            const breco::VisualizedNode& external =
                externalRoot.children.first().children.at(1);
            expectEqQString(external.sourceFilePath, QStringLiteral("index.bin"),
                            QStringLiteral("External node retains source path"));
            expectTrue(external.sourceOffset == 20,
                       QStringLiteral("External node retains absolute source offset"));
        }
        const breco::VisualizedNode unbound = breco::visualize(
            externalParsed.graph, QStringLiteral("Split"), primary, 0, 1, {});
        expectTrue(!unbound.children.isEmpty() && !unbound.children.first().valid,
                   QStringLiteral("Missing external role invalidates visualization"));
    }
}

}  // namespace

void testScanProgressFormatting();

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
    testStructDeclarationParser();
    testStructVisualizer();
    testStructModePanelUtilities();
    testDynamicStructLanguage();
    testStructureFeatureServices();

    if (g_failures == 0) {
        qInfo() << "All unit tests passed";
        return 0;
    }

    qCritical() << g_failures << "unit test(s) failed";
    return 1;
}
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

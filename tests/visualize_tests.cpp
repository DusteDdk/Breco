#include <QTest>

#include "visualize/VisualizeData.h"

namespace {

using namespace breco;

QString programWith(QStringView declarations) {
    return QStringLiteral(
               "language breco 0.1\n"
               "inputs { input data \"Visualize\" { default } }\n%1\n")
        .arg(declarations);
}

class VisualizeTests final : public QObject {
    Q_OBJECT

private slots:
    void resolvesDefaultAndExplicitRanges() {
        const VisualizationWindow noSelection =
            resolveVisualizationWindow(std::nullopt, 25, 2000);
        QCOMPARE(noSelection.start, 25ULL);
        QCOMPARE(noSelection.length, 1024ULL);
        QVERIFY(!noSelection.truncated);

        const VisualizationWindow oneByte = resolveVisualizationWindow(
            qMakePair<quint64, quint64>(1995, 1996), 0, 2000);
        QCOMPARE(oneByte.start, 1995ULL);
        QCOMPARE(oneByte.length, 5ULL);

        const VisualizationWindow selected = resolveVisualizationWindow(
            qMakePair<quint64, quint64>(10, 30), 0, 2000);
        QCOMPARE(selected.start, 10ULL);
        QCOMPARE(selected.length, 20ULL);

        const VisualizationWindow capped = resolveVisualizationWindow(
            qMakePair<quint64, quint64>(10, 110), 0, 2000, 32);
        QCOMPARE(capped.length, 32ULL);
        QVERIFY(capped.truncated);

        const VisualizationWindow configured = resolveVisualizationWindow(
            std::nullopt, 0, 2000, kMaximumVisualizationBytes, 77);
        QCOMPARE(configured.length, 77ULL);

        const VisualizationWindow entireFile = resolveVisualizationWindow(
            std::nullopt, 1500, 2000, 32, 0);
        QCOMPARE(entireFile.start, 0ULL);
        QCOMPARE(entireFile.length, 2000ULL);
        QVERIFY(!entireFile.truncated);
    }

    void readsBuiltinConfigurationForEmptySource() {
        QVERIFY(!builtinVisualizeProgramSource().contains(
            QStringLiteral("language breco")));
        QVERIFY(!builtinVisualizeProgramSource().contains(
            QStringLiteral("inputs {")));
        const VisualizationConfigurationResult result =
            readVisualizationConfiguration({});
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.config.numBytesOnNoSelection, 1024ULL);
        QCOMPARE(result.config.style, CartesianStyle::Line);
    }

    void usesBuiltinVisCfgWhenUserRecordIsMissing() {
        const QString source = programWith(QStringLiteral(R"BRECO(
record Cart2D {
    Points: { y: u8 }
}
)BRECO"));
        const VisualizationDecodeResult result = decodeVisualization(
            source, QByteArray::fromHex("010203"), 100,
            VisualizationMode::Cartesian2D);
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.config.numBytesOnNoSelection, 1024ULL);
        QCOMPARE(result.points.size(), 3);
        QCOMPARE(result.points.at(0).x, 0.0);
        QCOMPARE(result.points.at(2).x, 2.0);
        QCOMPARE(result.points.at(2).y, 3.0);
        QVERIFY(!result.usedBuiltinRecord);
    }

    void modeFieldsOverrideVisCfg() {
        const QString source = programWith(QStringLiteral(R"BRECO(
record VisCfg {
    computed NumBytesOnNoSelection: u32 = 77
    computed Style: string = "area"
}
record Cart2D {
    computed NumBytesOnNoSelection: u32 = 55
    computed Style: string = "skin"
    Chart: { computed tickDistance: u32 = 4 }
    Points: { x: u8 y: u8 }
}
)BRECO"));
        const VisualizationDecodeResult result = decodeVisualization(
            source, QByteArray::fromHex("01020304"), 0,
            VisualizationMode::Cartesian2D);
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.config.numBytesOnNoSelection, 55ULL);
        QCOMPARE(result.config.style, CartesianStyle::Skin);
        QCOMPARE(result.config.tickDistance, 4.0);
        QCOMPARE(result.points.size(), 2);
    }

    void acceptsWholeFileAndBarConfiguration() {
        const QString source = programWith(QStringLiteral(R"BRECO(
record VisCfg {
    computed NumBytesOnNoSelection: u32 = 0
    computed Style: string = "bar"
}
record Cart2D {
    Points: { y: u8 }
}
)BRECO"));
        const VisualizationDecodeResult result = decodeVisualization(
            source, QByteArray::fromHex("010203"), 0,
            VisualizationMode::Cartesian2D);
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.config.numBytesOnNoSelection, 0ULL);
        QCOMPARE(result.config.style, CartesianStyle::Bar);
    }

    void barFallsBackToDefaultForCartesian3D() {
        const QString source = programWith(QStringLiteral(R"BRECO(
record VisCfg {
    computed Style: string = "bar"
}
record Cart3D {
    Points: { x: u8 y: u8 z: u8 }
}
)BRECO"));
        const VisualizationDecodeResult result = decodeVisualization(
            source, QByteArray::fromHex("010203"), 0,
            VisualizationMode::Cartesian3D);
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.config.style, CartesianStyle::Dot);
    }

    void decodesOptionalCartesianPointColor() {
        const QString source = programWith(QStringLiteral(R"BRECO(
record Cart2D {
    Color: { r: u8 g: u8 b: u8 a: u8 }
    Points: { x: u8 y: u8 }
}
)BRECO"));
        const VisualizationDecodeResult result = decodeVisualization(
            source, QByteArray::fromHex("102030400506"), 0,
            VisualizationMode::Cartesian2D);
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.points.size(), 1);
        QCOMPARE(result.points.first().color,
                 QColor(0x10, 0x20, 0x30, 0x40));
        QCOMPARE(result.points.first().x, 5.0);
        QCOMPARE(result.points.first().y, 6.0);
    }

    void missingModeRecordFallsBackToBuiltinRecord() {
        const QString source = programWith(QStringLiteral(R"BRECO(
record VisCfg {
    computed NumBytesOnNoSelection: u32 = 17
    computed Style: string = "area"
}
)BRECO"));
        const VisualizationDecodeResult result = decodeVisualization(
            source, QByteArray::fromHex("010203"), 0,
            VisualizationMode::Cartesian3D);
        QVERIFY2(result.success(), qPrintable(result.error));
        QVERIFY(result.usedBuiltinRecord);
        QCOMPARE(result.config.numBytesOnNoSelection, 17ULL);
        QCOMPARE(result.config.style, CartesianStyle::Dot);
        QCOMPARE(result.points.size(), 1);
    }

    void injectsVisualizeInputWhenProgramHasNone() {
        const QString source = QStringLiteral(R"BRECO(
record VisCfg {
    computed NumBytesOnNoSelection: u32 = 12
}
record Cart2D {
    Points: { y: u8 }
}
)BRECO");
        const VisualizationDecodeResult result = decodeVisualization(
            source, QByteArray::fromHex("090a"), 0,
            VisualizationMode::Cartesian2D);
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.config.numBytesOnNoSelection, 12ULL);
        QCOMPARE(result.points.size(), 2);
    }

    void decodesLargeRangesInBoundedChunks() {
        const QString source = programWith(QStringLiteral(R"BRECO(
record Cart2D {
    Points: { y: u8 }
}
)BRECO"));
        const qsizetype byteCount = 530000;
        const VisualizationDecodeResult result = decodeVisualization(
            source, QByteArray(byteCount, '\x01'), 0x1000,
            VisualizationMode::Cartesian2D);
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.points.size(), byteCount);
        QCOMPARE(result.points.last().x,
                 static_cast<double>(byteCount - 1));
    }

    void decodesSequentialOneBitBitmapWithoutColor() {
        const QString source = programWith(QStringLiteral(R"BRECO(
record Bitmap {
    computed NumBytesOnNoSelection: u32 = 33
}
)BRECO"));
        const VisualizationDecodeResult result = decodeVisualization(
            source, QByteArray::fromHex("81"), 0,
            VisualizationMode::Bitmap);
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.bitmapBitsPerPixel, 1);
        QCOMPARE(result.config.numBytesOnNoSelection, 33ULL);
        QVERIFY(!result.bitmapHasPlot);
        QVERIFY(result.bitmapPixels.isEmpty());
        QCOMPARE(result.bitmapPackedBits, QByteArray::fromHex("81"));
    }

    void decodesSequentialRgbBitmapFromColorShape() {
        const QString source = programWith(QStringLiteral(R"BRECO(
record Bitmap {
    Color: { r: u8 g: u8 b: u8 }
}
)BRECO"));
        const VisualizationDecodeResult result = decodeVisualization(
            source, QByteArray::fromHex("ff000000ff00"), 0,
            VisualizationMode::Bitmap);
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.bitmapBitsPerPixel, 24);
        QVERIFY(!result.bitmapHasPlot);
        QCOMPARE(result.bitmapPixels.size(), 2);
        QCOMPARE(result.bitmapPixels.first().color, QColor(255, 0, 0));
        QCOMPARE(result.bitmapPixels.last().color, QColor(0, 255, 0));
    }

    void decodesScatterBitmapCoordinatesAndAlpha() {
        const VisualizationDecodeResult result = decodeVisualization(
            builtinVisualizeProgramSource(),
            QByteArray::fromHex("102030400506"), 0,
            VisualizationMode::Bitmap);
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.bitmapBitsPerPixel, 32);
        QVERIFY(result.bitmapHasPlot);
        QCOMPARE(result.bitmapPixels.size(), 1);
        QCOMPARE(result.bitmapPixels.first().x, 5);
        QCOMPARE(result.bitmapPixels.first().y, 6);
        QCOMPARE(result.bitmapPixels.first().color, QColor(0x10, 0x20, 0x30,
                                                           0x40));
    }

    void preservesSignedScatterCoordinates() {
        const QString source = programWith(QStringLiteral(R"BRECO(
record Bitmap {
    Color: { r: u8 }
    Plot: { x: i8 y: i8 }
}
)BRECO"));
        const VisualizationDecodeResult result = decodeVisualization(
            source, QByteArray::fromHex("fffefd800102"), 0,
            VisualizationMode::Bitmap);
        QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(result.bitmapPixels.size(), 2);
        QCOMPARE(result.bitmapPixels.first().x, -2);
        QCOMPARE(result.bitmapPixels.first().y, -3);
        QCOMPARE(result.bitmapPixels.last().x, 1);
        QCOMPARE(result.bitmapPixels.last().y, 2);
    }
};

}  // namespace

QTEST_APPLESS_MAIN(VisualizeTests)

#include "visualize_tests.moc"

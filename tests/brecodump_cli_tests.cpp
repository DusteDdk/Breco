#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

namespace {

class BrecodumpCliTests : public QObject {
    Q_OBJECT

private slots:
    void helpStdinAndFileStructDumpWork();
    void serializedFieldOrderAndMetadataAreStable();
    void nestedStructsSerializeInsideValue();
    void namedOutformRendersAndValidatesName();
};

QByteArray runBrecodump(const QStringList& args, const QByteArray& stdinBytes = {},
                        int expectedExitCode = 0) {
    QProcess process;
    process.start(QStringLiteral(BRECODUMP_PATH), args);
    if (!stdinBytes.isEmpty()) {
        process.write(stdinBytes);
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(10000)) {
        qFatal("brecodump did not finish");
    }
    const QByteArray stderrBytes = process.readAllStandardError();
    if (!stderrBytes.isEmpty()) {
        qFatal("unexpected brecodump stderr: %s", stderrBytes.constData());
    }
    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != expectedExitCode) {
        qFatal("unexpected brecodump exit: status=%d code=%d",
               static_cast<int>(process.exitStatus()), process.exitCode());
    }
    return process.readAllStandardOutput();
}

QJsonObject jsonObjectFromBytes(const QByteArray& bytes) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qFatal("invalid JSON: %s", parseError.errorString().toUtf8().constData());
    }
    return document.object();
}

void BrecodumpCliTests::helpStdinAndFileStructDumpWork() {
    const QByteArray help = runBrecodump({QStringLiteral("--help")});
    QVERIFY(help.contains("Usage:"));
    QVERIFY(help.contains("stdin"));
    QVERIFY(help.contains("-e ENTRY_NAME=last"));
    QVERIFY(help.contains("Output format:"));

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString binaryPath = tempDir.filePath(QStringLiteral("sample.bin"));
    QFile binary(binaryPath);
    QVERIFY(binary.open(QIODevice::WriteOnly));
    const QByteArray binaryBytes = QByteArray::fromHex("010002FF");
    QCOMPARE(binary.write(binaryBytes), binaryBytes.size());
    binary.close();

    const QByteArray declarationBytes(
        "struct S { uint8 a; uint16<be> b; }\nuint8 standalone;");

    const QJsonObject stdinDump = jsonObjectFromBytes(runBrecodump({
        QStringLiteral("-i"), binaryPath,
        QStringLiteral("-ofs"), QStringLiteral("1"),
    }, declarationBytes));
    const QJsonObject stdinMetadata =
        stdinDump.value(QStringLiteral("metadata")).toObject();
    QCOMPARE(stdinMetadata.value(QStringLiteral("declarationSource")).toString(),
             QStringLiteral("stdin"));
    QCOMPARE(stdinMetadata.value(QStringLiteral("offset")).toString(), QStringLiteral("1"));
    QCOMPARE(stdinMetadata.value(QStringLiteral("entrypoint")).toString(),
             QStringLiteral("standalone"));
    QVERIFY(!stdinDump.contains(QStringLiteral("S")));
    QVERIFY(stdinDump.contains(QStringLiteral("standalone")));

    const QString declarationPath = tempDir.filePath(QStringLiteral("sample.struct"));
    QFile declaration(declarationPath);
    QVERIFY(declaration.open(QIODevice::WriteOnly));
    QCOMPARE(declaration.write(declarationBytes), declarationBytes.size());
    declaration.close();

    const QString outputPath = tempDir.filePath(QStringLiteral("dump.json"));
    const QByteArray stdoutBytes = runBrecodump({
        QStringLiteral("-s"), declarationPath,
        QStringLiteral("-i"), binaryPath,
        QStringLiteral("-e"), QStringLiteral("S"),
        QStringLiteral("-r"), QStringLiteral("1"),
        QStringLiteral("-o"), outputPath,
    });
    QVERIFY(stdoutBytes.isEmpty());

    QFile output(outputPath);
    QVERIFY(output.open(QIODevice::ReadOnly));
    const QJsonObject dumped = jsonObjectFromBytes(output.readAll());
    const QJsonObject metadata = dumped.value(QStringLiteral("metadata")).toObject();
    QCOMPARE(metadata.value(QStringLiteral("tool")).toString(), QStringLiteral("brecodump"));
    QCOMPARE(metadata.value(QStringLiteral("input")).toString(),
             QFileInfo(binaryPath).absoluteFilePath());
    QCOMPARE(metadata.value(QStringLiteral("entrypoint")).toString(),
             QStringLiteral("S"));
    const QJsonObject structDump = dumped.value(QStringLiteral("S")).toObject();
    const QJsonObject structValue =
        structDump.value(QStringLiteral("value")).toObject();
    QCOMPARE(structValue.value(QStringLiteral("a"))
                 .toObject()
                 .value(QStringLiteral("value"))
                 .toString(),
             QStringLiteral("1 (0X1)"));
    QCOMPARE(structValue.value(QStringLiteral("b"))
                 .toObject()
                 .value(QStringLiteral("value"))
                 .toString(),
             QStringLiteral("2 (0X2)"));
    QVERIFY(!dumped.contains(QStringLiteral("standalone")));

    const QString pngPath = tempDir.filePath(QStringLiteral("sample.png"));
    QFile png(pngPath);
    QVERIFY(png.open(QIODevice::WriteOnly));
    const QByteArray pngHeaderBytes = QByteArray::fromHex(
        "89504E470D0A1A0A"
        "0000000D"
        "49484452"
        "00000010"
        "00000020"
        "08"
        "02"
        "00"
        "00"
        "00"
        "12345678");
    QCOMPARE(png.write(pngHeaderBytes), pngHeaderBytes.size());
    png.close();
    const QByteArray pngDump = runBrecodump({
        QStringLiteral("-s"),
        QStringLiteral(QT_TESTCASE_SOURCEDIR) +
            QStringLiteral("/examples/pnghead.brecostruct"),
        QStringLiteral("-i"), pngPath,
    });
    QVERIFY(pngDump.contains("\"PNGHeader\""));
    QVERIFY(pngDump.contains("\"width\""));
    QVERIFY(pngDump.contains("16 (0X10)"));
    QVERIFY(pngDump.contains("\"height\""));
    QVERIFY(pngDump.contains("32 (0X20)"));

    QProcess invalidEntryProcess;
    invalidEntryProcess.start(
        QStringLiteral(BRECODUMP_PATH),
        {QStringLiteral("-s"), declarationPath,
         QStringLiteral("-i"), binaryPath,
         QStringLiteral("-e"), QStringLiteral("Missing")});
    invalidEntryProcess.closeWriteChannel();
    QVERIFY(invalidEntryProcess.waitForFinished(10000));
    QCOMPARE(invalidEntryProcess.exitStatus(), QProcess::NormalExit);
    QCOMPARE(invalidEntryProcess.exitCode(), 2);
    QVERIFY(invalidEntryProcess.readAllStandardError().contains(
        "Unknown entry 'Missing'. Available entries: standalone, S"));
}

void BrecodumpCliTests::serializedFieldOrderAndMetadataAreStable() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString binaryPath = tempDir.filePath(QStringLiteral("ordered.bin"));
    QFile binary(binaryPath);
    QVERIFY(binary.open(QIODevice::WriteOnly));
    const QByteArray binaryBytes = QByteArray::fromHex("0102000403");
    QCOMPARE(binary.write(binaryBytes), binaryBytes.size());
    binary.close();

    const QByteArray declarationBytes(
        "struct Ordered {"
        " uint8 zeta;"
        " /cond(=2) uint8 alpha;"
        " uint16<be> omega;"
        " /cond(=0xFF) uint8 middle;"
        " }");
    const QByteArray output = runBrecodump(
        {QStringLiteral("-i"), binaryPath,
         QStringLiteral("-e"), QStringLiteral("Ordered")},
        declarationBytes);

    const QJsonObject root = jsonObjectFromBytes(output);
    const QJsonObject ordered =
        root.value(QStringLiteral("Ordered")).toObject();
    const QJsonObject orderedValue =
        ordered.value(QStringLiteral("value")).toObject();
    QVERIFY(!ordered.contains(QStringLiteral("valid")));
    QVERIFY(!orderedValue.value(QStringLiteral("zeta"))
                 .toObject()
                 .contains(QStringLiteral("valid")));
    QVERIFY(!orderedValue.value(QStringLiteral("zeta"))
                 .toObject()
                 .contains(QStringLiteral("endianness")));
    QCOMPARE(orderedValue.value(QStringLiteral("alpha"))
                 .toObject()
                 .value(QStringLiteral("valid"))
                 .toBool(),
             true);
    QCOMPARE(orderedValue.value(QStringLiteral("middle"))
                 .toObject()
                 .value(QStringLiteral("valid"))
                 .toBool(),
             false);
    QCOMPARE(orderedValue.value(QStringLiteral("omega"))
                 .toObject()
                 .value(QStringLiteral("endianness"))
                 .toString(),
             QStringLiteral("big"));

    const qsizetype orderedPosition = output.indexOf("\"Ordered\"");
    const qsizetype zetaPosition = output.indexOf("\"zeta\"", orderedPosition);
    const qsizetype alphaPosition = output.indexOf("\"alpha\"", zetaPosition);
    const qsizetype omegaPosition = output.indexOf("\"omega\"", alphaPosition);
    const qsizetype middlePosition = output.indexOf("\"middle\"", omegaPosition);
    QVERIFY(orderedPosition >= 0);
    QVERIFY(zetaPosition > orderedPosition);
    QVERIFY(alphaPosition > zetaPosition);
    QVERIFY(omegaPosition > alphaPosition);
    QVERIFY(middlePosition > omegaPosition);

    const QByteArray parentMetadata =
        output.mid(orderedPosition, zetaPosition - orderedPosition);
    QVERIFY(!parentMetadata.contains("\"valid\""));

    const QByteArray zeta =
        output.mid(zetaPosition, alphaPosition - zetaPosition);
    const qsizetype zetaValue = zeta.indexOf("\"value\"");
    const qsizetype zetaBytes = zeta.indexOf("\"rawBytesHex\"");
    const qsizetype zetaType = zeta.indexOf("\"type\"");
    QVERIFY(zetaValue >= 0);
    QVERIFY(zetaBytes > zetaValue);
    QVERIFY(zetaType > zetaBytes);
    QVERIFY(!zeta.contains("\"valid\""));
    QVERIFY(!zeta.contains("\"endianness\""));

    const QByteArray alpha =
        output.mid(alphaPosition, omegaPosition - alphaPosition);
    const qsizetype alphaValue = alpha.indexOf("\"value\"");
    const qsizetype alphaValid = alpha.indexOf("\"valid\"");
    const qsizetype alphaBytes = alpha.indexOf("\"rawBytesHex\"");
    const qsizetype alphaType = alpha.indexOf("\"type\"");
    QVERIFY(alphaValue >= 0);
    QVERIFY(alphaValid > alphaValue);
    QVERIFY(alphaBytes > alphaValid);
    QVERIFY(alphaType > alphaBytes);
    QVERIFY(alpha.contains("\"valid\": true"));
    QVERIFY(!alpha.contains("\"endianness\""));

    const QByteArray omega =
        output.mid(omegaPosition, middlePosition - omegaPosition);
    const qsizetype omegaValue = omega.indexOf("\"value\"");
    const qsizetype omegaBytes = omega.indexOf("\"rawBytesHex\"");
    const qsizetype omegaType = omega.indexOf("\"type\"");
    const qsizetype omegaEndianness = omega.indexOf("\"endianness\"");
    QVERIFY(omegaValue >= 0);
    QVERIFY(omegaBytes > omegaValue);
    QVERIFY(omegaType > omegaBytes);
    QVERIFY(omegaEndianness > omegaType);
    QVERIFY(!omega.contains("\"valid\""));
    QVERIFY(omega.contains("\"endianness\": \"big\""));

    const QByteArray middle = output.mid(middlePosition);
    const qsizetype middleValue = middle.indexOf("\"value\"");
    const qsizetype middleValid = middle.indexOf("\"valid\"");
    const qsizetype middleBytes = middle.indexOf("\"rawBytesHex\"");
    const qsizetype middleType = middle.indexOf("\"type\"");
    QVERIFY(middleValue >= 0);
    QVERIFY(middleValid > middleValue);
    QVERIFY(middleBytes > middleValid);
    QVERIFY(middleType > middleBytes);
    QVERIFY(middle.contains("\"valid\": false"));
    QVERIFY(!middle.contains("\"endianness\""));
}

void BrecodumpCliTests::nestedStructsSerializeInsideValue() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString binaryPath = tempDir.filePath(QStringLiteral("nested.bin"));
    QFile binary(binaryPath);
    QVERIFY(binary.open(QIODevice::WriteOnly));
    const QByteArray binaryBytes = QByteArray::fromHex("AA010203");
    QCOMPARE(binary.write(binaryBytes), binaryBytes.size());
    binary.close();

    const QByteArray declarationBytes(
        "struct Inner {"
        " /cond(=0xAA) uint8 marker;"
        " uint8 payload;"
        " }"
        " struct Outer {"
        " /cond(true) Inner inner;"
        " /repeat(2) uint8 bytes;"
        " }");
    const QByteArray output = runBrecodump(
        {QStringLiteral("-i"), binaryPath,
         QStringLiteral("-e"), QStringLiteral("Outer")},
        declarationBytes);

    const QJsonObject root = jsonObjectFromBytes(output);
    const QJsonObject outer =
        root.value(QStringLiteral("Outer")).toObject();
    const QJsonObject outerValue =
        outer.value(QStringLiteral("value")).toObject();
    const QJsonObject inner =
        outerValue.value(QStringLiteral("inner")).toObject();
    const QJsonObject innerValue =
        inner.value(QStringLiteral("value")).toObject();
    const QJsonArray repeatedBytes =
        outerValue.value(QStringLiteral("bytes"))
            .toObject()
            .value(QStringLiteral("value"))
            .toArray();
    QVERIFY(outerValue.contains(QStringLiteral("inner")));
    QVERIFY(outerValue.contains(QStringLiteral("bytes")));
    QVERIFY(innerValue.contains(QStringLiteral("marker")));
    QVERIFY(innerValue.contains(QStringLiteral("payload")));
    QCOMPARE(inner.value(QStringLiteral("valid")).toBool(), true);
    QCOMPARE(repeatedBytes.size(), 2);
    QCOMPARE(repeatedBytes.at(0)
                 .toObject()
                 .value(QStringLiteral("value"))
                 .toString(),
             QStringLiteral("2 (0X2)"));
    QCOMPARE(repeatedBytes.at(1)
                 .toObject()
                 .value(QStringLiteral("value"))
                 .toString(),
             QStringLiteral("3 (0X3)"));
    QVERIFY(!outer.contains(QStringLiteral("valid")));

    const qsizetype innerPosition = output.indexOf("\"inner\"");
    const qsizetype innerValuePosition =
        output.indexOf("\"value\"", innerPosition);
    const qsizetype markerPosition =
        output.indexOf("\"marker\"", innerValuePosition);
    const qsizetype payloadPosition =
        output.indexOf("\"payload\"", markerPosition);
    const qsizetype innerValidPosition =
        output.indexOf("\"valid\"", payloadPosition);
    const qsizetype innerTypePosition =
        output.indexOf("\"type\"", innerValidPosition);
    QVERIFY(innerPosition >= 0);
    QVERIFY(innerValuePosition > innerPosition);
    QVERIFY(markerPosition > innerValuePosition);
    QVERIFY(payloadPosition > markerPosition);
    QVERIFY(innerValidPosition > payloadPosition);
    QVERIFY(innerTypePosition > innerValidPosition);
}

void BrecodumpCliTests::namedOutformRendersAndValidatesName() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString binaryPath = tempDir.filePath(QStringLiteral("sample.bin"));
    QFile binary(binaryPath);
    QVERIFY(binary.open(QIODevice::WriteOnly));
    QCOMPARE(binary.write(QByteArray::fromHex("2A")), 1);
    binary.close();

    const QString includedPath = tempDir.filePath(QStringLiteral("formats.brecostruct"));
    QFile included(includedPath);
    QVERIFY(included.open(QIODevice::WriteOnly));
    included.write("outform line binary {name={{name}};{{#children}}{{name}}={{value}}{{/children}}}");
    included.close();
    const QString declarationPath = tempDir.filePath(QStringLiteral("main.brecostruct"));
    QFile declaration(declarationPath);
    QVERIFY(declaration.open(QIODevice::WriteOnly));
    declaration.write("include \"formats.brecostruct\"; struct S { uint8 answer; }");
    declaration.close();

    const QByteArray rendered = runBrecodump({
        QStringLiteral("-s"), declarationPath,
        QStringLiteral("-i"), binaryPath,
        QStringLiteral("-e"), QStringLiteral("S"),
        QStringLiteral("-outform"), QStringLiteral("line"),
    });
    QCOMPARE(rendered, QByteArray("name=S[0];answer=42 (0X2A)"));

    QProcess invalid;
    invalid.start(QStringLiteral(BRECODUMP_PATH),
                  {QStringLiteral("-s"), declarationPath,
                   QStringLiteral("-i"), binaryPath,
                   QStringLiteral("-outform"), QStringLiteral("missing")});
    invalid.closeWriteChannel();
    QVERIFY(invalid.waitForFinished(10000));
    QCOMPARE(invalid.exitStatus(), QProcess::NormalExit);
    QCOMPARE(invalid.exitCode(), 2);
    QVERIFY(invalid.readAllStandardError().contains(
        "Unknown outform 'missing'. Available outforms: line"));
}

}  // namespace

QTEST_MAIN(BrecodumpCliTests)
#include "brecodump_cli_tests.moc"

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
    void helpAndStdinInputWork();
    void brecoLangFlagsStreamJsonAndReportNames();
    void brecoLangOutformsWriteTextAndBinary();
    void brecoLangFailedDecodePreservesOutputFile();
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

void BrecodumpCliTests::helpAndStdinInputWork() {
    const QByteArray help = runBrecodump({QStringLiteral("--help")});
    QVERIFY(help.contains("--schema FILE"));
    QVERIFY(help.contains("--input NAME=FILE"));
    QVERIFY(help.contains("--outform NAME"));
    QVERIFY(!help.contains("-ofs"));

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString schemaPath = tempDir.filePath(QStringLiteral("stdin.breco"));
    QFile schema(schemaPath);
    QVERIFY(schema.open(QIODevice::WriteOnly));
    const QByteArray source = QByteArrayLiteral(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Main from data { marker: u8 value: u16le }
default entry Main
)BRECO");
    QCOMPARE(schema.write(source), source.size());
    schema.close();

    const QByteArray output = runBrecodump(
        {QStringLiteral("--schema"), schemaPath,
         QStringLiteral("--input"), QStringLiteral("data=-")},
        QByteArray::fromHex("aa3412"));
    const QJsonObject root = jsonObjectFromBytes(output);
    QCOMPARE(root.value(QStringLiteral("marker")).toInt(), 0xaa);
    QCOMPARE(root.value(QStringLiteral("value")).toInt(), 0x1234);

    QProcess removedFlag;
    removedFlag.start(QStringLiteral(BRECODUMP_PATH),
                      {QStringLiteral("-i"), QStringLiteral("input.bin")});
    removedFlag.closeWriteChannel();
    QVERIFY(removedFlag.waitForFinished(10000));
    QCOMPARE(removedFlag.exitCode(), 2);
    QVERIFY(removedFlag.readAllStandardError().contains("Unknown argument: -i"));
}

void BrecodumpCliTests::brecoLangFlagsStreamJsonAndReportNames() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString schemaPath = tempDir.filePath(QStringLiteral("sample.breco"));
    QFile schema(schemaPath);
    QVERIFY(schema.open(QIODevice::WriteOnly));
    const QByteArray source = QByteArrayLiteral(R"BRECO(
language breco 0.1
inputs { input data { default } input aux { } }
record Header { marker: u8 value: u16le }
entry Main from data {
    header: Header
    external: u8 from aux at 0
    preserve remaining as tail
}
default entry Main
)BRECO");
    QCOMPARE(schema.write(source), source.size());
    schema.close();

    const QString dataPath = tempDir.filePath(QStringLiteral("data.bin"));
    QFile data(dataPath);
    QVERIFY(data.open(QIODevice::WriteOnly));
    QCOMPARE(data.write(QByteArray::fromHex("aa3412ff")), 4);
    data.close();
    const QString auxPath = tempDir.filePath(QStringLiteral("aux.bin"));
    QFile aux(auxPath);
    QVERIFY(aux.open(QIODevice::WriteOnly));
    QCOMPARE(aux.write(QByteArray::fromHex("77")), 1);
    aux.close();

    const QByteArray output = runBrecodump({
        QStringLiteral("--schema"), schemaPath,
        QStringLiteral("--input"), QStringLiteral("data=%1").arg(dataPath),
        QStringLiteral("--input"), QStringLiteral("aux=%1").arg(auxPath),
        QStringLiteral("--entry"), QStringLiteral("Main"),
    });
    const QJsonObject root = jsonObjectFromBytes(output);
    const QJsonObject header = root.value(QStringLiteral("header")).toObject();
    QCOMPARE(header.value(QStringLiteral("marker")).toInt(), 0xAA);
    QCOMPARE(header.value(QStringLiteral("value")).toInt(), 0x1234);
    QCOMPARE(root.value(QStringLiteral("external")).toInt(), 0x77);
    QCOMPARE(root.value(QStringLiteral("tail")).toString(), QStringLiteral("ff"));

    QProcess badEntry;
    badEntry.start(QStringLiteral(BRECODUMP_PATH),
                   {QStringLiteral("--schema"), schemaPath,
                    QStringLiteral("--entry"), QStringLiteral("Missing")});
    badEntry.closeWriteChannel();
    QVERIFY(badEntry.waitForFinished(10000));
    QCOMPARE(badEntry.exitCode(), 2);
    QVERIFY(badEntry.readAllStandardError().contains(
        "Unknown entry 'Missing'. Available entries: Main"));

    QProcess badInput;
    badInput.start(QStringLiteral(BRECODUMP_PATH),
                   {QStringLiteral("--schema"), schemaPath,
                    QStringLiteral("--input"),
                    QStringLiteral("other=%1").arg(dataPath)});
    badInput.closeWriteChannel();
    QVERIFY(badInput.waitForFinished(10000));
    QCOMPARE(badInput.exitCode(), 2);
    const QByteArray error = badInput.readAllStandardError();
    QVERIFY(error.contains("Unknown input 'other'"));
    QVERIFY(error.contains("Available inputs: data, aux"));
}

void BrecodumpCliTests::brecoLangOutformsWriteTextAndBinary() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString schemaPath = tempDir.filePath(QStringLiteral("outforms.breco"));
    QFile schema(schemaPath);
    QVERIFY(schema.open(QIODevice::WriteOnly));
    const QByteArray source = QByteArrayLiteral(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Main from data { value: u16le preserve remaining as tail }
default entry Main
outform Line(root: Main) text {
    emit "${root.@input}:${hex(root.value)}:${hex_bytes(root.tail.@bytes)}"
}
outform Packet(root: Main) binary {
    emit u16be(root.value)
    emit root.tail.@bytes
}
)BRECO");
    QCOMPARE(schema.write(source), source.size());
    schema.close();
    const QString dataPath = tempDir.filePath(QStringLiteral("input.bin"));
    QFile data(dataPath);
    QVERIFY(data.open(QIODevice::WriteOnly));
    QCOMPARE(data.write(QByteArray::fromHex("3412aabb")), 4);
    data.close();

    const QStringList base{QStringLiteral("--schema"), schemaPath,
                           QStringLiteral("--input"),
                           QStringLiteral("data=%1").arg(dataPath),
                           QStringLiteral("--entry"), QStringLiteral("Main")};
    QStringList textArgs = base;
    textArgs << QStringLiteral("--outform") << QStringLiteral("Line");
    QCOMPARE(runBrecodump(textArgs), QByteArray("data:0x1234:aabb"));

    QStringList binaryArgs = base;
    binaryArgs << QStringLiteral("--outform") << QStringLiteral("Packet");
    QCOMPARE(runBrecodump(binaryArgs), QByteArray::fromHex("1234aabb"));

    const QString outputPath = tempDir.filePath(QStringLiteral("packet.bin"));
    binaryArgs << QStringLiteral("--output") << outputPath;
    QVERIFY(runBrecodump(binaryArgs).isEmpty());
    QFile output(outputPath);
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), QByteArray::fromHex("1234aabb"));
}

void BrecodumpCliTests::brecoLangFailedDecodePreservesOutputFile() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString schemaPath = tempDir.filePath(QStringLiteral("failing.breco"));
    QFile schema(schemaPath);
    QVERIFY(schema.open(QIODevice::WriteOnly));
    const QByteArray source = QByteArrayLiteral(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Main from data { items: repeat 3 { value: u8 } }
default entry Main
)BRECO");
    QCOMPARE(schema.write(source), source.size());
    schema.close();

    const QString dataPath = tempDir.filePath(QStringLiteral("short.bin"));
    QFile data(dataPath);
    QVERIFY(data.open(QIODevice::WriteOnly));
    QCOMPARE(data.write(QByteArray::fromHex("0102")), 2);
    data.close();

    const QString outputPath = tempDir.filePath(QStringLiteral("existing.json"));
    const QByteArray original("existing output must survive");
    QFile existing(outputPath);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write(original), original.size());
    existing.close();

    const auto runFailure = [&](const QString& destination) {
        QProcess process;
        process.start(
            QStringLiteral(BRECODUMP_PATH),
            {QStringLiteral("--schema"), schemaPath,
             QStringLiteral("--input"),
             QStringLiteral("data=%1").arg(dataPath),
             QStringLiteral("--entry"), QStringLiteral("Main"),
             QStringLiteral("--output"), destination});
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(process.readAllStandardError().contains("Unexpected end"));
        QVERIFY(process.readAllStandardOutput().isEmpty());
    };

    runFailure(outputPath);
    QFile preserved(outputPath);
    QVERIFY(preserved.open(QIODevice::ReadOnly));
    QCOMPARE(preserved.readAll(), original);

    const QString absentPath = tempDir.filePath(QStringLiteral("absent.json"));
    QVERIFY(!QFileInfo::exists(absentPath));
    runFailure(absentPath);
    QVERIFY(!QFileInfo::exists(absentPath));
}

}  // namespace

QTEST_MAIN(BrecodumpCliTests)
#include "brecodump_cli_tests.moc"

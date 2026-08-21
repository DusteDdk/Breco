#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "brecolang/compiler/Compiler.h"
#include "brecolang/render/OutformRenderer.h"
#include "brecolang/runtime/ByteSource.h"
#include "brecolang/runtime/Interpreter.h"

namespace {

using namespace breco::lang;

QString compilerDiagnostics(const QVector<Diagnostic>& diagnostics) {
    QStringList messages;
    for (const Diagnostic& diagnostic : diagnostics) {
        messages.push_back(QStringLiteral("%1: %2")
                               .arg(diagnostic.code, diagnostic.message));
    }
    return messages.join(QLatin1Char('\n'));
}

QString runtimeDiagnostics(const QVector<RuntimeDiagnostic>& diagnostics) {
    QStringList messages;
    for (const RuntimeDiagnostic& diagnostic : diagnostics) {
        messages.push_back(QStringLiteral("%1: %2")
                               .arg(diagnostic.code, diagnostic.message));
    }
    return messages.join(QLatin1Char('\n'));
}

SymbolId symbol(const BrecoProgram& program, QStringView name) {
    for (SymbolId id = 0; id < static_cast<SymbolId>(program.symbols.size()); ++id) {
        if (program.symbols.at(id) == name) {
            return id;
        }
    }
    return kInvalidId;
}

DecodedValueId field(const DecodedTree& tree, const BrecoProgram& program,
                     DecodedValueId object, QStringView name) {
    const DecodedFieldValue* found = tree.findField(object, symbol(program, name));
    return found != nullptr ? found->value : kInvalidId;
}

DecodeResult decode(const std::shared_ptr<const BrecoProgram>& program,
                    QString entry, const QHash<QString, QByteArray>& inputs,
                    DecodeMode mode = DecodeMode::Tree,
                    quint64 baseOffset = 0) {
    DecodeRequest request;
    request.program = program;
    request.entryName = std::move(entry);
    request.mode = mode;
    request.inputs.resize(program->inputs.size());
    for (InputId id = 0; id < static_cast<InputId>(program->inputs.size()); ++id) {
        const QString role = program->symbol(program->inputs.at(id).name);
        const auto found = inputs.constFind(role);
        if (found != inputs.constEnd()) {
            request.inputs[id] = std::make_shared<BorrowedWindowSource>(
                *found, role + QStringLiteral(".bin"), baseOffset);
        }
    }
    return decodeBrecoProgram(request);
}

const QString kCoreProgram = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs {
    input data { default }
    input aux { }
}
limits {
    max_parse_depth 16
    max_loop_iterations 100
    max_nodes 1000
    max_probe_bytes 64
}
enum Mode: u8 { Main = 0xA }
record Pair { first: u8 second: u8 }
record Packet {
    identify {
        magic: u8
        match magic == 0xC7 else "not a packet"
    }
    commit
    require remaining >= 5 else "packet is truncated"
    flags: bitfield u8 {
        high: bits 7..4
        low: bits 3..0
    }
    computed doubled: f64 = flags.low * 2.0
    payload: region bytes(3) {
        values: repeat 2 { value: u8 } until at_end
        preserve remaining as padding
    }
    branch: select flags.high {
        Mode.Main => { selected: u8 }
        default => { fallback: u8 }
    }
    check flags.low == 5 else "unexpected low flags"
}
entry Main from data {
    packet: Packet
    external: Pair from aux at 1 within bytes(2)
    optional: Pair when remaining > 100
    computed has_optional: bool = present(optional)
    preserve remaining as tail
}
default entry Main
)BRECO");

const QString kTransactionalProgram = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_loop_iterations 100 max_nodes 1000 max_probe_bytes 32 }
record A {
    identify {
        tag: u8
        check false else "discarded trial warning"
        match tag == 0xA1 else "not A"
    }
    commit
    require remaining >= 1 else "A payload missing"
    value: u8
}
record B {
    identify { tag: u8 match tag == 0xB2 else "not B" }
    commit
    value: u8
}
record C {
    identify { tag: u8 match tag == 0xA1 else "not C" }
    commit
}
record Choice { item: one_of { as A as B as C } }
entry Choose from data { choice: Choice }
entry ManyA from data {
    items: many A
    preserve remaining as tail
}
)BRECO");

const QString kLoopProgram = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_loop_iterations 20 max_nodes 1000 }
entry Loops from data {
    fixed: repeat 3 { value: u8 }
    untils: repeat { value: u8 } until iteration == 1
    whiles: while remaining > 1 with { seed: u64 = 7 } {
        value: u8
        continue when iteration == 0
        break when iteration == 1
    }
    preserve remaining as tail
}
)BRECO");

const QString kRecoverProgram = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_loop_iterations 100 max_nodes 1000 max_probe_bytes 16 }
record Good {
    identify { marker: u8 match marker == 0x7E else "not good" }
    commit
    value: u8
}
entry Recover from data {
    items: recover Good {
        sync bytes [0x7E]
        step 1 byte
        max_probe 16
        gaps as Noise
    }
}
)BRECO");

const QString kStreamingProgram = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_loop_iterations 100 max_nodes 1000 max_probe_bytes 32 }
record A {
    identify { tag: u8 match tag == 0xA1 else "not A" }
    commit
    value: u8
}
record B {
    identify { tag: u8 match tag == 0xB2 else "not B" }
    commit
    value: u8
}
record Good {
    identify { marker: u8 match marker == 0x7E else "not good" }
    commit
    value: u8
}
entry Stream from data {
    choice: one_of { as A as B }
    items: recover Good {
        sync bytes [0x7E]
        step 1 byte
        max_probe 32
        gaps as Noise
    }
}
default entry Stream
)BRECO");

const QString kFailingStreamingLoopProgram = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_loop_iterations 100 max_nodes 1000 }
entry Fail from data {
    prefix: u8
    items: repeat 3 {
        value: u8
        check false else "discarded loop warning"
    }
}
)BRECO");

const QString kOutformProgram = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_loop_iterations 100 max_nodes 1000 max_transform_output 1_MiB }
enum Mode: u8 { Alpha = 1 Beta = 2 }
entry Run from data {
    groups: repeat 2 {
        rows: repeat 2 {
            flag: u8
            value: u16le
        }
    }
    mode: Mode
    preserve remaining as tail
    check mode == Mode.Beta else "alpha mode"
}
default entry Run

outform Text(root: Run) text {
    emit "input=${root.@input}\n"
    for group, group_index in root.groups {
        for row, row_index in group.rows {
            let encoded = upper(hex(row.value))
            if row.flag == 1 {
                emit "${group_index}.${row_index}:${encoded}:${row.@offset}\n"
            } else {
                emit "${group_index}.${row_index}:${lower(encoded)}:${row.@offset}\n"
            }
        }
    }
    emit enum_name(root.mode)
    emit "|"
    emit csv("a,b")
    emit "|"
    emit json("line\n")
    emit "|"
    emit hex_bytes(root.tail.@bytes)
    emit "|${root.@name}|${root.@type}|${root.@path}|${root.@length}|${count(root.@children)}|${count(root.@spans)}|${root.@valid}|${root.@error}"
    for span in root.@spans {
        emit "|${span.@input}:${span.@offset}:${span.@length}"
    }
}

outform Binary(root: Run) binary {
    for group in root.groups {
        for row in group.rows {
            if row.flag == 1 { emit u16be(row.value) }
            else { emit u16le(row.value) }
        }
    }
    emit u8(root.mode)
    emit root.tail.@bytes
    emit i8(-1)
    emit u16le(0x1234)
    emit u16be(0x1234)
    emit i16le(-2)
    emit i16be(-2)
    emit u32le(0x12345678)
    emit u32be(0x12345678)
    emit i32le(-3)
    emit i32be(-3)
    emit u64le(0x0102030405060708)
    emit u64be(0x0102030405060708)
    emit i64le(-4)
    emit i64be(-4)
    emit f32le(1.5)
    emit f32be(1.5)
    emit f64le(2.5)
    emit f64be(2.5)
    emit utf8("Z")
    emit utf16le("A")
    emit utf16be("B")
}

outform Overflow(root: Run) binary {
    emit u8(256)
}

outform SignedOverflow(root: Run) binary {
    emit i16le(32768)
}

outform NegativeUnsigned(root: Run) binary {
    emit u32le(-1)
}
)BRECO");

class RecordingOutput final : public QIODevice {
public:
    RecordingOutput() { open(QIODevice::WriteOnly); }

    QByteArray bytes;
    int writeCalls = 0;
    qint64 largestWrite = 0;

protected:
    qint64 writeData(const char* data, qint64 length) override {
        ++writeCalls;
        largestWrite = qMax(largestWrite, length);
        bytes.append(data, length);
        return length;
    }
    qint64 readData(char*, qint64) override { return -1; }
};

class BrecoLangRuntimeTests : public QObject {
    Q_OBJECT

private slots:
    void treeModeDecodesRecordsRegionsBitfieldsComputedSelectAndInputs();
    void probeModeExecutesWithoutConstructingNodes();
    void oneOfRollsBackAndCommitStopsAlternation();
    void manyStopsOnUncommittedMismatch();
    void loopsHonorRepeatUntilWhileContinueAndBreak();
    void recoverEmitsGapAndResynchronizes();
    void depthAndLoopLimitsAreEnforced();
    void nodeAndSpeculativeProbeLimitsAreEnforced();
    void byteSourcesExposeStableRangesAndOffsets();
    void streamingModeWritesIncrementallyAndReplaysTransactions();
    void streamingLoopFailureRollsBackBeforeEmission();
    void outformsRenderNestedTextAndRealBinary();
};

void BrecoLangRuntimeTests::treeModeDecodesRecordsRegionsBitfieldsComputedSelectAndInputs() {
    const CompileResult compiled = compileBrecoLang(kCoreProgram);
    QVERIFY2(compiled.success(), qPrintable(compilerDiagnostics(compiled.diagnostics)));
    const DecodeResult result = decode(
        compiled.program, QStringLiteral("Main"),
        {{QStringLiteral("data"), QByteArray::fromHex("c7a50102ff334455")},
         {QStringLiteral("aux"), QByteArray::fromHex("ee1020ff")}});
    QVERIFY2(result.success(), qPrintable(runtimeDiagnostics(result.diagnostics)));
    QVERIFY(result.tree != nullptr);
    QCOMPARE(result.endOffset, 8ULL);

    const DecodedTree& tree = *result.tree;
    const DecodedValueId packet =
        field(tree, *compiled.program, result.rootValue, u"packet");
    QVERIFY(packet != kInvalidId);
    const DecodedValueId flags = field(tree, *compiled.program, packet, u"flags");
    const DecodedValueId low = field(tree, *compiled.program, flags, u"low");
    QCOMPARE(tree.values.at(low).unsignedValue, 5ULL);
    const DecodedValueId doubled =
        field(tree, *compiled.program, packet, u"doubled");
    QCOMPARE(tree.values.at(doubled).floatingValue, 10.0);

    const DecodedValueId payload =
        field(tree, *compiled.program, packet, u"payload");
    const DecodedValueId values =
        field(tree, *compiled.program, payload, u"values");
    QCOMPARE(tree.values.at(values).elements.count, 2U);
    const DecodedValueId padding =
        field(tree, *compiled.program, payload, u"padding");
    QCOMPARE(tree.spans.at(tree.values.at(padding).payload).length, 1ULL);

    const DecodedValueId external =
        field(tree, *compiled.program, result.rootValue, u"external");
    QCOMPARE(tree.values.at(field(tree, *compiled.program, external, u"first"))
                 .unsignedValue,
             0x10ULL);
    const DecodedValueId hasOptional =
        field(tree, *compiled.program, result.rootValue, u"has_optional");
    QVERIFY(!tree.values.at(hasOptional).booleanValue);

    bool foundSharedBitSpan = false;
    bool foundExternalRole = false;
    for (const DecodedNode& node : tree.nodes) {
        if (tree.name(node.name) == QStringLiteral("low")) {
            foundSharedBitSpan = node.hasSourceSpan && node.offset == 1 &&
                                 node.length == 1;
        }
        if (tree.name(node.name) == QStringLiteral("external")) {
            foundExternalRole =
                node.input < static_cast<InputId>(compiled.program->inputs.size()) &&
                compiled.program->symbol(
                    compiled.program->inputs.at(node.input).name) ==
                    QStringLiteral("aux");
        }
    }
    QVERIFY(foundSharedBitSpan);
    QVERIFY(foundExternalRole);

    const DecodeResult checked = decode(
        compiled.program, QStringLiteral("Main"),
        {{QStringLiteral("data"), QByteArray::fromHex("c7a40102ff334455")},
         {QStringLiteral("aux"), QByteArray::fromHex("ee1020ff")}});
    QVERIFY2(checked.success(), qPrintable(runtimeDiagnostics(checked.diagnostics)));
    QVERIFY(!checked.diagnostics.isEmpty());
    bool invalidPacket = false;
    for (const DecodedNode& node : checked.tree->nodes) {
        if (checked.tree->name(node.name) == QStringLiteral("packet")) {
            invalidPacket = !node.valid;
        }
    }
    QVERIFY(invalidPacket);
}

void BrecoLangRuntimeTests::probeModeExecutesWithoutConstructingNodes() {
    const CompileResult compiled = compileBrecoLang(kCoreProgram);
    QVERIFY(compiled.success());
    const DecodeResult result = decode(
        compiled.program, QStringLiteral("Main"),
        {{QStringLiteral("data"), QByteArray::fromHex("c7a50102ff334455")},
         {QStringLiteral("aux"), QByteArray::fromHex("ee1020ff")}},
        DecodeMode::Probe);
    QVERIFY2(result.success(), qPrintable(runtimeDiagnostics(result.diagnostics)));
    QVERIFY(result.tree == nullptr);
    QCOMPARE(result.constructedNodes, 0ULL);
    QCOMPARE(result.endOffset, 8ULL);
}

void BrecoLangRuntimeTests::oneOfRollsBackAndCommitStopsAlternation() {
    const CompileResult compiled = compileBrecoLang(kTransactionalProgram);
    QVERIFY2(compiled.success(), qPrintable(compilerDiagnostics(compiled.diagnostics)));
    const DecodeResult selected =
        decode(compiled.program, QStringLiteral("Choose"),
               {{QStringLiteral("data"), QByteArray::fromHex("b299")}});
    QVERIFY2(selected.success(), qPrintable(runtimeDiagnostics(selected.diagnostics)));
    QVERIFY(selected.diagnostics.isEmpty());
    QCOMPARE(selected.endOffset, 2ULL);
    const DecodedValueId choice =
        field(*selected.tree, *compiled.program, selected.rootValue, u"choice");
    const DecodedValueId item =
        field(*selected.tree, *compiled.program, choice, u"item");
    QCOMPARE(selected.tree->values.at(
                 field(*selected.tree, *compiled.program, item, u"value"))
                 .unsignedValue,
             0x99ULL);

    const DecodeResult committed =
        decode(compiled.program, QStringLiteral("Choose"),
               {{QStringLiteral("data"), QByteArray::fromHex("a1")}});
    QCOMPARE(committed.status, DecodeStatus::Error);
    QVERIFY(runtimeDiagnostics(committed.diagnostics).contains(
        QStringLiteral("A payload missing")));
}

void BrecoLangRuntimeTests::manyStopsOnUncommittedMismatch() {
    const CompileResult compiled = compileBrecoLang(kTransactionalProgram);
    QVERIFY(compiled.success());
    const DecodeResult result =
        decode(compiled.program, QStringLiteral("ManyA"),
               {{QStringLiteral("data"),
                 QByteArray::fromHex("a101a102ff")}});
    QVERIFY2(result.success(), qPrintable(runtimeDiagnostics(result.diagnostics)));
    const DecodedValueId items =
        field(*result.tree, *compiled.program, result.rootValue, u"items");
    QCOMPARE(result.tree->values.at(items).elements.count, 2U);
    const DecodedValueId tail =
        field(*result.tree, *compiled.program, result.rootValue, u"tail");
    QCOMPARE(result.tree->spans.at(result.tree->values.at(tail).payload).length,
             1ULL);
}

void BrecoLangRuntimeTests::loopsHonorRepeatUntilWhileContinueAndBreak() {
    const CompileResult compiled = compileBrecoLang(kLoopProgram);
    QVERIFY2(compiled.success(), qPrintable(compilerDiagnostics(compiled.diagnostics)));
    const DecodeResult result =
        decode(compiled.program, QStringLiteral("Loops"),
               {{QStringLiteral("data"), QByteArray::fromHex("01020304050607aa")}});
    QVERIFY2(result.success(), qPrintable(runtimeDiagnostics(result.diagnostics)));
    const DecodedTree& tree = *result.tree;
    QCOMPARE(tree.values.at(field(tree, *compiled.program, result.rootValue, u"fixed"))
                 .elements.count,
             3U);
    QCOMPARE(tree.values.at(field(tree, *compiled.program, result.rootValue, u"untils"))
                 .elements.count,
             2U);
    QCOMPARE(tree.values.at(field(tree, *compiled.program, result.rootValue, u"whiles"))
                 .elements.count,
             2U);
    QCOMPARE(result.endOffset, 8ULL);
}

void BrecoLangRuntimeTests::recoverEmitsGapAndResynchronizes() {
    const CompileResult compiled = compileBrecoLang(kRecoverProgram);
    QVERIFY2(compiled.success(), qPrintable(compilerDiagnostics(compiled.diagnostics)));
    const DecodeResult result =
        decode(compiled.program, QStringLiteral("Recover"),
               {{QStringLiteral("data"), QByteArray::fromHex("7e01aabb7e02")}});
    QVERIFY2(result.success(), qPrintable(runtimeDiagnostics(result.diagnostics)));
    const DecodedValueId items =
        field(*result.tree, *compiled.program, result.rootValue, u"items");
    QCOMPARE(result.tree->values.at(items).elements.count, 2U);
    bool foundGap = false;
    for (const DecodedNode& node : result.tree->nodes) {
        if (node.kind == DecodedNodeKind::Gap &&
            result.tree->name(node.name) == QStringLiteral("Noise")) {
            foundGap = node.length == 2 && node.offset == 2;
        }
    }
    QVERIFY(foundGap);
}

void BrecoLangRuntimeTests::depthAndLoopLimitsAreEnforced() {
    const QString recursive = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_parse_depth 2 max_loop_iterations 2 max_nodes 100 }
record Nested(depth: u32) {
    length: u8
    payload: region bytes(length) {
        content: select {
            when length > 0 => { child: Nested(depth + 1) }
            else => raw remaining as value
        }
    }
}

entry Deep from data { root: Nested(0) }
entry TooMany from data { values: repeat 3 { value: u8 } }
entry AtLimit from data { values: repeat 2 { value: u8 } }
)BRECO");
    const CompileResult compiled = compileBrecoLang(recursive);
    QVERIFY2(compiled.success(), qPrintable(compilerDiagnostics(compiled.diagnostics)));
    const DecodeResult depth =
        decode(compiled.program, QStringLiteral("Deep"),
               {{QStringLiteral("data"), QByteArray::fromHex("03020000")}});
    QCOMPARE(depth.status, DecodeStatus::Error);
    QVERIFY(runtimeDiagnostics(depth.diagnostics).contains(
        QStringLiteral("Parse depth limit")));
    const DecodeResult loops =
        decode(compiled.program, QStringLiteral("TooMany"),
               {{QStringLiteral("data"), QByteArray::fromHex("010203")}});
    QCOMPARE(loops.status, DecodeStatus::Error);
    QVERIFY(runtimeDiagnostics(loops.diagnostics).contains(
        QStringLiteral("Loop iteration limit")));
    const DecodeResult atLimit =
        decode(compiled.program, QStringLiteral("AtLimit"),
               {{QStringLiteral("data"), QByteArray::fromHex("0102")}});
    QVERIFY2(atLimit.success(),
             qPrintable(runtimeDiagnostics(atLimit.diagnostics)));
}

void BrecoLangRuntimeTests::nodeAndSpeculativeProbeLimitsAreEnforced() {
    const QString nodeLimited = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_nodes 2 }
record Pair { first: u8 second: u8 }
entry Main from data { pair: Pair }
)BRECO");
    const CompileResult nodeProgram = compileBrecoLang(nodeLimited);
    QVERIFY2(nodeProgram.success(),
             qPrintable(compilerDiagnostics(nodeProgram.diagnostics)));
    const DecodeResult nodes =
        decode(nodeProgram.program, QStringLiteral("Main"),
               {{QStringLiteral("data"), QByteArray::fromHex("0102")}});
    QCOMPARE(nodes.status, DecodeStatus::Error);
    QVERIFY(runtimeDiagnostics(nodes.diagnostics).contains(
        QStringLiteral("node limit"), Qt::CaseInsensitive));

    const QString probeLimited = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_nodes 100 max_probe_bytes 2 }
record LongProbe {
    identify {
        first: u8
        second: u8
        third: u8
        match third == 1 else "not long"
    }
    commit
}
entry Main from data { item: one_of { as LongProbe } }
)BRECO");
    const CompileResult probeProgram = compileBrecoLang(probeLimited);
    QVERIFY2(probeProgram.success(),
             qPrintable(compilerDiagnostics(probeProgram.diagnostics)));
    const DecodeResult probe =
        decode(probeProgram.program, QStringLiteral("Main"),
               {{QStringLiteral("data"), QByteArray::fromHex("000001")}});
    QCOMPARE(probe.status, DecodeStatus::Error);
    QVERIFY(runtimeDiagnostics(probe.diagnostics).contains(
        QStringLiteral("probe limit"), Qt::CaseInsensitive));
}

void BrecoLangRuntimeTests::byteSourcesExposeStableRangesAndOffsets() {
    BorrowedWindowSource borrowed(QByteArray::fromHex("01020304"),
                                  QStringLiteral("window.bin"), 100);
    const ByteReadResult borrowedRead = borrowed.read(1, 2);
    QVERIFY(borrowedRead.ok());
    QCOMPARE(QByteArray(borrowedRead.view.data(), borrowedRead.view.length),
             QByteArray::fromHex("0203"));
    QCOMPARE(borrowed.absoluteOffset(2), 102ULL);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("paged.bin"));
    QByteArray fileBytes(9000, '\0');
    for (qsizetype i = 0; i < fileBytes.size(); ++i) {
        fileBytes[i] = static_cast<char>(i & 0xff);
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(fileBytes), static_cast<qint64>(fileBytes.size()));
    file.close();
    QString error;
    const auto paged = PagedFileSource::open(path, &error, 4096, 1);
    QVERIFY2(paged != nullptr, qPrintable(error));
    const ByteReadResult acrossPages = paged->read(4094, 8);
    QVERIFY(acrossPages.ok());
    QCOMPARE(QByteArray(acrossPages.view.data(), acrossPages.view.length),
             fileBytes.mid(4094, 8));

    auto sequentialDevice = std::make_shared<QBuffer>();
    sequentialDevice->setData(QByteArray("abcdef"));
    QVERIFY(sequentialDevice->open(QIODevice::ReadOnly));
    SequentialSource sequential(sequentialDevice, QStringLiteral("stream"));
    QVERIFY(!sequential.randomAccess());
    QCOMPARE(QByteArray(sequential.read(2, 3).view.data(), 3), QByteArray("cde"));
    sequential.releaseBefore(4);
    QCOMPARE(sequential.read(0, 1).status, ByteReadStatus::Error);
    const ByteReadResult sequentialTail = sequential.read(4, 2);
    QVERIFY(sequentialTail.ok());
    QCOMPARE(QByteArray(sequentialTail.view.data(), 2), QByteArray("ef"));

    auto spoolDevice = std::make_shared<QBuffer>();
    spoolDevice->setData(QByteArray("uvwxyz"));
    QVERIFY(spoolDevice->open(QIODevice::ReadOnly));
    SpoolingSource spool(spoolDevice, QStringLiteral("spool"));
    QVERIFY(spool.isOpen());
    QCOMPARE(QByteArray(spool.read(4, 2).view.data(), 2), QByteArray("yz"));
    QCOMPARE(QByteArray(spool.read(0, 2).view.data(), 2), QByteArray("uv"));
}

void BrecoLangRuntimeTests::streamingModeWritesIncrementallyAndReplaysTransactions() {
    const CompileResult compiled = compileBrecoLang(kStreamingProgram);
    QVERIFY2(compiled.success(),
             qPrintable(compilerDiagnostics(compiled.diagnostics)));
    RecordingOutput output;
    DecodeRequest request;
    request.program = compiled.program;
    request.entryName = QStringLiteral("Stream");
    request.mode = DecodeMode::Streaming;
    request.output = &output;
    request.inputs.resize(compiled.program->inputs.size());
    request.inputs[0] = std::make_shared<BorrowedWindowSource>(
        QByteArray::fromHex("b22a007e117e22"), QStringLiteral("stream.bin"));

    const DecodeResult decoded = decodeBrecoProgram(request);
    QVERIFY2(decoded.success(),
             qPrintable(runtimeDiagnostics(decoded.diagnostics)));
    QVERIFY(decoded.tree == nullptr);
    QCOMPARE(decoded.constructedNodes, 0ULL);
    QVERIFY(output.writeCalls > 20);
    QVERIFY(output.largestWrite < output.bytes.size());

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(output.bytes, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    const QJsonObject root = document.object();
    const QJsonObject choice = root.value(QStringLiteral("choice")).toObject();
    QCOMPARE(choice.value(QStringLiteral("tag")).toInt(), 0xB2);
    QCOMPARE(choice.value(QStringLiteral("value")).toInt(), 0x2A);
    const QJsonArray items = root.value(QStringLiteral("items")).toArray();
    QCOMPARE(items.size(), 3);
    const QJsonObject gap = items.at(0).toObject();
    QCOMPARE(gap.value(QStringLiteral("@gap")).toString(),
             QStringLiteral("Noise"));
    QCOMPARE(gap.value(QStringLiteral("@input")).toString(),
             QStringLiteral("data"));
    QCOMPARE(gap.value(QStringLiteral("@offset")).toInt(), 2);
    QCOMPARE(gap.value(QStringLiteral("bytes")).toString(),
             QStringLiteral("00"));
    QCOMPARE(items.at(1).toObject().value(QStringLiteral("value")).toInt(),
             0x11);
    QCOMPARE(items.at(2).toObject().value(QStringLiteral("value")).toInt(),
             0x22);
}

void BrecoLangRuntimeTests::streamingLoopFailureRollsBackBeforeEmission() {
    const CompileResult compiled = compileBrecoLang(kFailingStreamingLoopProgram);
    QVERIFY2(compiled.success(),
             qPrintable(compilerDiagnostics(compiled.diagnostics)));
    RecordingOutput output;
    DecodeRequest request;
    request.program = compiled.program;
    request.entryName = QStringLiteral("Fail");
    request.mode = DecodeMode::Streaming;
    request.output = &output;
    request.inputs.resize(compiled.program->inputs.size());
    request.inputs[0] = std::make_shared<BorrowedWindowSource>(
        QByteArray::fromHex("aa0102"), QStringLiteral("truncated.bin"));

    const DecodeResult decoded = decodeBrecoProgram(request);
    QCOMPARE(decoded.status, DecodeStatus::Error);
    QCOMPARE(decoded.endOffset, 1ULL);
    QCOMPARE(output.bytes, QByteArray("{\"prefix\":170"));
    QVERIFY(!output.bytes.contains("\"items\""));
    const QString diagnostics = runtimeDiagnostics(decoded.diagnostics);
    QVERIFY(diagnostics.contains(QStringLiteral("Unexpected end")));
    QVERIFY(!diagnostics.contains(QStringLiteral("discarded loop warning")));
}

void BrecoLangRuntimeTests::outformsRenderNestedTextAndRealBinary() {
    const CompileResult compiled = compileBrecoLang(kOutformProgram);
    QVERIFY2(compiled.success(),
             qPrintable(compilerDiagnostics(compiled.diagnostics)));
    QVector<std::shared_ptr<ByteSource>> sources(compiled.program->inputs.size());
    sources[0] = std::make_shared<BorrowedWindowSource>(
        QByteArray::fromHex("01341200785601bc9a00f0de01aabb"),
        QStringLiteral("capture.bin"), 100);
    DecodeRequest request;
    request.program = compiled.program;
    request.entryName = QStringLiteral("Run");
    request.inputs = sources;
    request.mode = DecodeMode::Tree;
    const DecodeResult decoded = decodeBrecoProgram(request);
    QVERIFY2(decoded.success(),
             qPrintable(runtimeDiagnostics(decoded.diagnostics)));
    const RenderStore store(compiled.program, decoded.tree, sources,
                            decoded.rootValue);

    QBuffer textOutput;
    QVERIFY(textOutput.open(QIODevice::WriteOnly));
    const OutformRenderResult text =
        renderOutform(store, u"Text", &textOutput);
    QVERIFY2(text.success, qPrintable(text.error));
    QCOMPARE(
        textOutput.data(),
        QByteArray("input=data\n"
                   "0.0:0X1234:100\n"
                   "0.1:0x5678:103\n"
                   "1.0:0X9ABC:106\n"
                   "1.1:0xdef0:109\n"
                   "Alpha|\"a,b\"|\"line\\n\"|aabb|Run|Run|capture.bin|15|3|1|false|alpha mode|data:100:15"));
    QCOMPARE(text.bytesWritten,
             static_cast<quint64>(textOutput.data().size()));

    QBuffer binaryOutput;
    QVERIFY(binaryOutput.open(QIODevice::WriteOnly));
    const OutformRenderResult binary =
        renderOutform(store, u"Binary", &binaryOutput);
    QVERIFY2(binary.success, qPrintable(binary.error));
    const QByteArray expected = QByteArray::fromHex(
        "123478569abcf0de"
        "01aabb"
        "ff"
        "34121234"
        "fefffffe"
        "7856341212345678"
        "fdfffffffffffffd"
        "08070605040302010102030405060708"
        "fcfffffffffffffffffffffffffffffc"
        "0000c03f3fc00000"
        "00000000000004404004000000000000"
        "5a41000042");
    QCOMPARE(binaryOutput.data(), expected);
    QCOMPARE(binary.bytesWritten, static_cast<quint64>(expected.size()));

    QBuffer missingOutput;
    QVERIFY(missingOutput.open(QIODevice::WriteOnly));
    const OutformRenderResult missing =
        renderOutform(store, u"Missing", &missingOutput);
    QVERIFY(!missing.success);
    QVERIFY(missing.error.contains(QStringLiteral("Available outforms")));

    QBuffer overflowOutput;
    QVERIFY(overflowOutput.open(QIODevice::WriteOnly));
    const OutformRenderResult overflow =
        renderOutform(store, u"Overflow", &overflowOutput);
    QVERIFY(!overflow.success);
    QVERIFY(overflowOutput.data().isEmpty());
    QVERIFY(overflow.error.contains(QStringLiteral("Encoder 'u8'")));
    QVERIFY(overflow.error.contains(QStringLiteral("value 256")));
    QVERIFY(overflow.error.contains(QStringLiteral("source offset")));

    QBuffer signedOverflowOutput;
    QVERIFY(signedOverflowOutput.open(QIODevice::WriteOnly));
    const OutformRenderResult signedOverflow =
        renderOutform(store, u"SignedOverflow", &signedOverflowOutput);
    QVERIFY(!signedOverflow.success);
    QVERIFY(signedOverflowOutput.data().isEmpty());
    QVERIFY(signedOverflow.error.contains(QStringLiteral("Encoder 'i16le'")));
    QVERIFY(signedOverflow.error.contains(QStringLiteral("value 32768")));

    QBuffer negativeUnsignedOutput;
    QVERIFY(negativeUnsignedOutput.open(QIODevice::WriteOnly));
    const OutformRenderResult negativeUnsigned =
        renderOutform(store, u"NegativeUnsigned", &negativeUnsignedOutput);
    QVERIFY(!negativeUnsigned.success);
    QVERIFY(negativeUnsignedOutput.data().isEmpty());
    QVERIFY(negativeUnsigned.error.contains(QStringLiteral("Encoder 'u32le'")));
    QVERIFY(negativeUnsigned.error.contains(QStringLiteral("value -1")));
}

}  // namespace

QTEST_APPLESS_MAIN(BrecoLangRuntimeTests)

#include "brecolang_runtime_tests.moc"

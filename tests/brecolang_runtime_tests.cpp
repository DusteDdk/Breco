#include <QBuffer>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "brecolang/compiler/Compiler.h"
#include "brecolang/render/OutformRenderer.h"
#include "brecolang/runtime/ByteSource.h"
#include "brecolang/runtime/DecodeDocument.h"
#include "brecolang/runtime/DecodeTarget.h"
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
    void recordOnlySchemaDecodesWithImplicitDefaults();
    void anonymousRecordFieldsDecodeAsNestedObjects();
    void recordTargetsAdaptToTheEntryPipeline();
    void treeModeDecodesRecordsRegionsBitfieldsComputedSelectAndInputs();
    void inlineSelectsYieldTransparentAndAggregatedFields();
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
    void fixedStrideWindowsAreGoldenEquivalentToLegacyTree();
    void millionByteSequenceResolvesAndMaterializesConstantSize();
    void inlineAggregatePagingUsesStableContainerLocator();
    void variableContinuationPreservesSemanticStateAndColdReplay();
    void variableReplayBudgetPausesAtCommittedBoundaries();
    void manyAndRecoverUseForwardContinuations();
    void referencesAreLazyCanonicalAndDecodeBoundedTargets();
    void referenceArithmeticAndStructuralPlansStayCheckedAndLazy();
    void shippedElfReferenceExampleNavigatesSample();
};

QString nodeDigest(const DecodedTree& tree, const BrecoProgram& program,
                   DecodedNodeId id) {
    const DecodedNode& node = tree.nodes.at(id);
    QStringList parts{
        QString::number(static_cast<int>(node.kind)), tree.name(node.name),
        QString::number(node.type), tree.displayValue(node.value, program),
        QString::number(node.input), QString::number(node.offset),
        QString::number(node.length), QString::number(node.hasSourceSpan),
        QString::number(node.valid)};
    if (node.parent != kInvalidId &&
        node.parent < static_cast<DecodedNodeId>(tree.locators.size())) {
        const MaterializationLocator& parent = tree.locators.at(node.parent);
        for (StatementId statement : parent.templatePath) {
            parts.push_back(QStringLiteral("p%1").arg(statement));
        }
        for (quint64 index : parent.sequenceIndexes) {
            parts.push_back(QStringLiteral("i%1").arg(index));
        }
    }
    if (node.storageLayout <
        static_cast<quint32>(tree.storageLayouts.size())) {
        const StorageLayout& layout = tree.storageLayouts.at(node.storageLayout);
        parts.push_back(QString::number(static_cast<int>(layout.kind)));
        parts.push_back(QString::number(layout.declaredType));
        parts.push_back(QString::number(static_cast<int>(layout.endianness)));
        parts.push_back(QString::number(layout.bitWidth));
        parts.push_back(QString::number(layout.highBit));
        parts.push_back(QString::number(layout.lowBit));
        for (quint32 span = 0; span < layout.spans.count; ++span) {
            const ByteSpanValue& value =
                tree.spans.at(layout.spans.first + span);
            parts.push_back(QStringLiteral("s%1:%2:%3")
                                .arg(value.input)
                                .arg(value.offset)
                                .arg(value.length));
        }
    }
    return parts.join(QLatin1Char('|'));
}

QHash<InstanceLocator, QString> canonicalNodes(
    const std::shared_ptr<const DecodedTree>& tree,
    const BrecoProgram& program) {
    QHash<InstanceLocator, QString> result;
    if (!tree) return result;
    for (DecodedNodeId id = 0;
         id < static_cast<DecodedNodeId>(tree->nodes.size()); ++id) {
        if (id < static_cast<DecodedNodeId>(tree->locators.size())) {
            result.insert(tree->locators.at(id), nodeDigest(*tree, program, id));
        }
    }
    return result;
}

void BrecoLangRuntimeTests::recordOnlySchemaDecodesWithImplicitDefaults() {
    const CompileResult compiled = compileBrecoLang(QString::fromUtf8(R"BRECO(
record Vis {
    computed MyString: string = "Test"
    computed MyNumber: i16 = 123
    A: u8
    B: u8
    C: u8
}
)BRECO"));
    QVERIFY2(compiled.success(),
             qPrintable(compilerDiagnostics(compiled.diagnostics)));

    const DecodeResult result =
        decode(compiled.program, {}, {{QStringLiteral("data"),
                                       QByteArray::fromHex("010203")}});
    QVERIFY2(result.success(), qPrintable(runtimeDiagnostics(result.diagnostics)));
    QVERIFY(result.tree != nullptr);
    QCOMPARE(result.endOffset, 3ULL);

    const DecodedTree& tree = *result.tree;
    const DecodedValueId myString =
        field(tree, *compiled.program, result.rootValue, u"MyString");
    const DecodedValueId myNumber =
        field(tree, *compiled.program, result.rootValue, u"MyNumber");
    const DecodedValueId a =
        field(tree, *compiled.program, result.rootValue, u"A");
    const DecodedValueId b =
        field(tree, *compiled.program, result.rootValue, u"B");
    const DecodedValueId c =
        field(tree, *compiled.program, result.rootValue, u"C");
    QVERIFY(myString != kInvalidId);
    QVERIFY(myNumber != kInvalidId);
    QCOMPARE(tree.displayValue(myString, *compiled.program),
             QStringLiteral("Test"));
    QCOMPARE(tree.values.at(myNumber).signedValue, 123);
    QCOMPARE(tree.values.at(a).unsignedValue, 1ULL);
    QCOMPARE(tree.values.at(b).unsignedValue, 2ULL);
    QCOMPARE(tree.values.at(c).unsignedValue, 3ULL);
}

void BrecoLangRuntimeTests::anonymousRecordFieldsDecodeAsNestedObjects() {
    const CompileResult compiled = compileBrecoLang(QString::fromUtf8(R"BRECO(
record TopRecord {
    computed StaticString: string = "Stuff"
    Settings: {
        computed staticInt: i16 = 123
    }
    data: {
        A: u8
        B: u8
    }
}
)BRECO"));
    QVERIFY2(compiled.success(),
             qPrintable(compilerDiagnostics(compiled.diagnostics)));

    const QByteArray bytes = QByteArray::fromHex("1122");
    const DecodeResult result =
        decode(compiled.program, {}, {{QStringLiteral("data"), bytes}});
    QVERIFY2(result.success(), qPrintable(runtimeDiagnostics(result.diagnostics)));
    QVERIFY(result.tree != nullptr);
    QCOMPARE(result.endOffset, 2ULL);

    const DecodedTree& tree = *result.tree;
    const DecodedValueId settings =
        field(tree, *compiled.program, result.rootValue, u"Settings");
    const DecodedValueId data =
        field(tree, *compiled.program, result.rootValue, u"data");
    QVERIFY(settings != kInvalidId);
    QVERIFY(data != kInvalidId);
    QCOMPARE(tree.values.at(field(tree, *compiled.program, settings,
                                  u"staticInt"))
                 .signedValue,
             123);
    QCOMPARE(tree.values.at(field(tree, *compiled.program, data, u"A"))
                 .unsignedValue,
             0x11ULL);
    QCOMPARE(tree.values.at(field(tree, *compiled.program, data, u"B"))
                 .unsignedValue,
             0x22ULL);

    int nestedRecordNodes = 0;
    for (const DecodedNode& node : tree.nodes) {
        const QString name = tree.name(node.name);
        if ((name == QStringLiteral("Settings") ||
             name == QStringLiteral("data")) &&
            node.kind == DecodedNodeKind::Record) {
            ++nestedRecordNodes;
        }
    }
    QCOMPARE(nestedRecordNodes, 2);

    RecordingOutput output;
    DecodeRequest request;
    request.program = compiled.program;
    request.mode = DecodeMode::Streaming;
    request.output = &output;
    request.inputs.resize(compiled.program->inputs.size());
    request.inputs[0] = std::make_shared<BorrowedWindowSource>(
        bytes, QStringLiteral("anonymous-records.bin"));
    const DecodeResult streamed = decodeBrecoProgram(request);
    QVERIFY2(streamed.success(),
             qPrintable(runtimeDiagnostics(streamed.diagnostics)));
    const QJsonObject json = QJsonDocument::fromJson(output.bytes).object();
    QCOMPARE(json.value(QStringLiteral("StaticString")).toString(),
             QStringLiteral("Stuff"));
    QCOMPARE(json.value(QStringLiteral("Settings"))
                 .toObject()
                 .value(QStringLiteral("staticInt"))
                 .toInt(),
             123);
    QCOMPARE(json.value(QStringLiteral("data"))
                 .toObject()
                 .value(QStringLiteral("A"))
                 .toInt(),
             0x11);
    QCOMPARE(json.value(QStringLiteral("data"))
                 .toObject()
                 .value(QStringLiteral("B"))
                 .toInt(),
             0x22);

    const CompileResult coexistence =
        compileBrecoLang(QString::fromUtf8(R"BRECO(
record Shared { Named: u8 }
record Container {
    Shared: { Inline: u8 }
    Referenced: Shared
}
entry Main from data { value: Container }
)BRECO"));
    QVERIFY2(coexistence.success(),
             qPrintable(compilerDiagnostics(coexistence.diagnostics)));
    const DecodeResult coexistenceResult = decode(
        coexistence.program, QStringLiteral("Main"),
        {{QStringLiteral("data"), QByteArray::fromHex("3344")}});
    QVERIFY2(coexistenceResult.success(),
             qPrintable(runtimeDiagnostics(coexistenceResult.diagnostics)));
    const DecodedValueId container = field(
        *coexistenceResult.tree, *coexistence.program,
        coexistenceResult.rootValue, u"value");
    const DecodedValueId inlineRecord = field(
        *coexistenceResult.tree, *coexistence.program, container, u"Shared");
    const DecodedValueId referencedRecord = field(
        *coexistenceResult.tree, *coexistence.program, container, u"Referenced");
    QCOMPARE(coexistenceResult.tree->values.at(
                 field(*coexistenceResult.tree, *coexistence.program,
                       inlineRecord, u"Inline"))
                 .unsignedValue,
             0x33ULL);
    QCOMPARE(coexistenceResult.tree->values.at(
                 field(*coexistenceResult.tree, *coexistence.program,
                       referencedRecord, u"Named"))
                 .unsignedValue,
             0x44ULL);
}

void BrecoLangRuntimeTests::recordTargetsAdaptToTheEntryPipeline() {
    const CompileResult compiled = compileBrecoLang(QString::fromUtf8(R"BRECO(
inputs {
    input primary { default }
    input auxiliary { }
}
record Plain {
    value: u8
    Nested: { nested: u8 }
    external: u8 from auxiliary at 0
}
record NeedsParameter(seed: u8) {
    computed value: u8 = seed
}
)BRECO"));
    QVERIFY2(compiled.success(),
             qPrintable(compilerDiagnostics(compiled.diagnostics)));
    QVERIFY(compiled.program->entries.isEmpty());

    QString error;
    const ResolvedDecodeTarget target = resolveDecodeTarget(
        compiled.program, DecodeTargetKind::Record, u"Plain", &error);
    QVERIFY2(target.isValid(), qPrintable(error));
    QCOMPARE(target.primaryInput, 0U);
    QVERIFY(target.program != compiled.program);
    QCOMPARE(target.program->entries.size(), 1);
    QCOMPARE(target.program->symbol(target.program->entries.first().name),
             QStringLiteral("Plain"));

    const DecodeResult decoded = decode(
        target.program, target.entryName,
        {{QStringLiteral("primary"), QByteArray::fromHex("1133")},
         {QStringLiteral("auxiliary"), QByteArray::fromHex("22")}});
    QVERIFY2(decoded.success(),
             qPrintable(runtimeDiagnostics(decoded.diagnostics)));
    QCOMPARE(decoded.endOffset, 2ULL);
    QCOMPARE(decoded.tree->nodes.first().kind, DecodedNodeKind::Entry);
    QCOMPARE(decoded.tree->values.at(
                 field(*decoded.tree, *target.program, decoded.rootValue,
                       u"value"))
                 .unsignedValue,
             0x11ULL);
    QCOMPARE(decoded.tree->values.at(
                 field(*decoded.tree, *target.program, decoded.rootValue,
                       u"external"))
                 .unsignedValue,
             0x22ULL);

    const RecordDesc* anonymous = nullptr;
    for (const RecordDesc& record : compiled.program->records) {
        if (compiled.program->symbol(record.name)
                .startsWith(QStringLiteral("$anon_record_"))) {
            anonymous = &record;
            break;
        }
    }
    QVERIFY(anonymous != nullptr);
    error.clear();
    const ResolvedDecodeTarget anonymousTarget = resolveDecodeTarget(
        compiled.program, DecodeTargetKind::Record,
        compiled.program->symbol(anonymous->name), &error);
    QVERIFY2(anonymousTarget.isValid(), qPrintable(error));
    const DecodeResult anonymousDecoded = decode(
        anonymousTarget.program, anonymousTarget.entryName,
        {{QStringLiteral("primary"), QByteArray::fromHex("44")},
         {QStringLiteral("auxiliary"), QByteArray::fromHex("22")}});
    QVERIFY2(anonymousDecoded.success(),
             qPrintable(runtimeDiagnostics(anonymousDecoded.diagnostics)));
    QCOMPARE(anonymousDecoded.tree->values.at(
                 field(*anonymousDecoded.tree, *anonymousTarget.program,
                       anonymousDecoded.rootValue, u"nested"))
                 .unsignedValue,
             0x44ULL);

    error.clear();
    const ResolvedDecodeTarget parameterized = resolveDecodeTarget(
        compiled.program, DecodeTargetKind::Record, u"NeedsParameter", &error);
    QVERIFY(!parameterized.isValid());
    QVERIFY(error.contains(QStringLiteral("requires parameters")));

    const CompileResult ambiguous = compileBrecoLang(QString::fromUtf8(R"BRECO(
inputs { input left { } input right { } }
record First { value: u8 }
record Second { value: u8 }
)BRECO"));
    QVERIFY2(ambiguous.success(),
             qPrintable(compilerDiagnostics(ambiguous.diagnostics)));
    error.clear();
    const ResolvedDecodeTarget noDefault = resolveDecodeTarget(
        ambiguous.program, DecodeTargetKind::Record, u"First", &error);
    QVERIFY(!noDefault.isValid());
    QVERIFY(error.contains(QStringLiteral("default input")));

    const CompileResult implicit =
        compileBrecoLang(QStringLiteral("record Only { value: u8 }"));
    QVERIFY2(implicit.success(),
             qPrintable(compilerDiagnostics(implicit.diagnostics)));
    error.clear();
    const ResolvedDecodeTarget existing = resolveDecodeTarget(
        implicit.program, DecodeTargetKind::Record, u"Only", &error);
    QVERIFY2(existing.isValid(), qPrintable(error));
    QCOMPARE(existing.program, implicit.program);

    const CompileResult declared =
        compileBrecoLang(QStringLiteral("entry Main from data { value: u8 }"));
    QVERIFY2(declared.success(),
             qPrintable(compilerDiagnostics(declared.diagnostics)));
    error.clear();
    const ResolvedDecodeTarget entry = resolveDecodeTarget(
        declared.program, DecodeTargetKind::Entry, u"Main", &error);
    QVERIFY2(entry.isValid(), qPrintable(error));
    QCOMPARE(entry.program, declared.program);
}

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

void BrecoLangRuntimeTests::inlineSelectsYieldTransparentAndAggregatedFields() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Main from data {
    tag: u8
    select tag {
        1 => { direct: u8 shared: u8 }
        default => {
            fallback: u8
            shared: repeat 2 { part: u8 }
        }
    }
    select {
        when tag == 1 => { shared: repeat 2 { part: u8 } }
        else => { }
    }
    select { when tag == 1 => { solo: u8 } else => { } }
    select { when tag == 2 => { solo: u8 } else => { } }
    select { when tag == 9 => { never: u8 } else => { } }
    select { when tag == 8 => { never: u8 } else => { } }
    select { when true => { conditional: u8 when false } else => { } }
    select { when true => { conditional: u8 } else => { } }
    computed total: u64 = count(shared)
    computed has_never: bool = present(never)
}
)BRECO");
    const CompileResult compiled = compileBrecoLang(source);
    QVERIFY2(compiled.success(),
             qPrintable(compilerDiagnostics(compiled.diagnostics)));

    const QByteArray bytes = QByteArray::fromHex("01112233445566");
    const DecodeResult tree =
        decode(compiled.program, QStringLiteral("Main"),
               {{QStringLiteral("data"), bytes}});
    QVERIFY2(tree.success(), qPrintable(runtimeDiagnostics(tree.diagnostics)));
    const DecodedValueId direct =
        field(*tree.tree, *compiled.program, tree.rootValue, u"direct");
    QCOMPARE(tree.tree->values.at(direct).unsignedValue, 0x11ULL);
    const DecodedValueId shared =
        field(*tree.tree, *compiled.program, tree.rootValue, u"shared");
    QCOMPARE(tree.tree->values.at(shared).kind,
             DecodedValueKind::Aggregate);
    QCOMPARE(tree.tree->values.at(shared).logicalCount, 3ULL);
    QCOMPARE(tree.tree->values.at(shared).aggregateShape,
             DecodedAggregateShape::PromotedSequence);
    const DecodedValueId solo =
        field(*tree.tree, *compiled.program, tree.rootValue, u"solo");
    QCOMPARE(tree.tree->values.at(solo).kind,
             DecodedValueKind::Aggregate);
    QCOMPARE(tree.tree->values.at(solo).logicalCount, 1ULL);
    QCOMPARE(tree.tree->values.at(solo).aggregateShape,
             DecodedAggregateShape::SingleScalar);
    const DecodedValueId conditional =
        field(*tree.tree, *compiled.program, tree.rootValue, u"conditional");
    QCOMPARE(tree.tree->values.at(conditional).kind,
             DecodedValueKind::Aggregate);
    QCOMPARE(tree.tree->values.at(conditional).logicalCount, 1ULL);
    QCOMPARE(tree.tree->values.at(conditional).aggregateShape,
             DecodedAggregateShape::SingleScalar);
    QCOMPARE(field(*tree.tree, *compiled.program, tree.rootValue, u"never"),
             kInvalidId);
    QCOMPARE(tree.tree->values.at(
                 field(*tree.tree, *compiled.program, tree.rootValue, u"total"))
                 .unsignedValue,
             3ULL);
    QVERIFY(!tree.tree->values.at(
                  field(*tree.tree, *compiled.program, tree.rootValue,
                        u"has_never"))
                  .booleanValue);

    int sharedContainers = 0;
    int soloContainers = 0;
    for (const DecodedNode& node : tree.tree->nodes) {
        if (node.hidden) {
            continue;
        }
        QVERIFY(node.kind != DecodedNodeKind::Select);
        if (tree.tree->name(node.name) == QStringLiteral("shared")) {
            ++sharedContainers;
            QCOMPARE(node.kind, DecodedNodeKind::Sequence);
        }
        if (tree.tree->name(node.name) == QStringLiteral("solo")) {
            ++soloContainers;
            QCOMPARE(node.kind, DecodedNodeKind::Sequence);
        }
    }
    QCOMPARE(sharedContainers, 1);
    QCOMPARE(soloContainers, 1);

    RecordingOutput output;
    DecodeRequest request;
    request.program = compiled.program;
    request.entryName = QStringLiteral("Main");
    request.mode = DecodeMode::Streaming;
    request.output = &output;
    request.inputs.resize(compiled.program->inputs.size());
    request.inputs[0] = std::make_shared<BorrowedWindowSource>(
        bytes, QStringLiteral("inline.bin"));
    const DecodeResult streamed = decodeBrecoProgram(request);
    QVERIFY2(streamed.success(),
             qPrintable(runtimeDiagnostics(streamed.diagnostics)));
    const QJsonObject object = QJsonDocument::fromJson(output.bytes).object();
    QCOMPARE(object.value(QStringLiteral("direct")).toInt(), 0x11);
    QCOMPARE(object.value(QStringLiteral("solo")).toInt(), 0x55);
    QCOMPARE(object.value(QStringLiteral("conditional")).toInt(), 0x66);
    QVERIFY(!object.contains(QStringLiteral("never")));
    const QJsonArray sharedJson =
        object.value(QStringLiteral("shared")).toArray();
    QCOMPARE(sharedJson.size(), 3);
    QCOMPARE(sharedJson.at(0).toInt(), 0x22);
    QCOMPARE(sharedJson.at(1).toObject()
                 .value(QStringLiteral("part")).toInt(),
             0x33);
    QCOMPARE(sharedJson.at(2).toObject()
                 .value(QStringLiteral("part")).toInt(),
             0x44);
    QVERIFY(output.bytes.indexOf("\"shared\"") >
            output.bytes.indexOf("\"total\""));
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

void BrecoLangRuntimeTests::fixedStrideWindowsAreGoldenEquivalentToLegacyTree() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_loop_iterations 1000 max_nodes 10000 }
entry Run from data {
    prefix: u16le
    items: repeat 130 { flag: u8 value: u16le }
    suffix: u8
    check false else "golden warning"
}
)BRECO");
    const CompileResult compiled = compileBrecoLang(source);
    QVERIFY2(compiled.success(), qPrintable(compilerDiagnostics(compiled.diagnostics)));
    QByteArray bytes(2 + 130 * 3 + 1, '\0');
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>(i & 0xff);
    }

    DecodeRequest legacyRequest;
    legacyRequest.program = compiled.program;
    legacyRequest.entryName = QStringLiteral("Run");
    legacyRequest.mode = DecodeMode::Tree;
    legacyRequest.documentGeneration = 41;
    legacyRequest.inputs = {
        std::make_shared<BorrowedWindowSource>(bytes, QStringLiteral("data.bin"))};
    const DecodeResult legacy = decodeBrecoProgram(legacyRequest);
    QVERIFY2(legacy.success(), qPrintable(runtimeDiagnostics(legacy.diagnostics)));

    DecodeDocument document({1}, 41);
    DecodeRequest shapeRequest = legacyRequest;
    shapeRequest.mode = DecodeMode::ResolveShape;
    shapeRequest.inputs = {
        std::make_shared<BorrowedWindowSource>(bytes, QStringLiteral("data.bin"))};
    const DecodeResult resolved = document.resolve(shapeRequest);
    QVERIFY2(resolved.success(), qPrintable(runtimeDiagnostics(resolved.diagnostics)));
    QVERIFY(resolved.tree == nullptr);
    QVERIFY(resolved.shape != nullptr);
    QCOMPARE(resolved.shape->sequences.size(), 1);
    const ResolvedSequenceShape sequence = resolved.shape->sequences.first();
    QCOMPARE(sequence.indexKind, SequenceIndexKind::Arithmetic);
    QCOMPARE(sequence.arithmetic.count, 130ULL);
    QCOMPARE(sequence.arithmetic.stride, 3ULL);
    QVERIFY(resolved.constructedNodes < 10);

    QHash<InstanceLocator, QString> paged = canonicalNodes(
        resolved.shape->outline, *compiled.program);
    DisplayPageRequest first;
    first.document = {1};
    first.root = resolved.shape->root;
    first.sequenceWindows = {{sequence.locator, 0, 64}};
    const DisplayPageResult firstPage = document.requestDisplayPage(first);
    QVERIFY2(firstPage.success(),
             qPrintable(runtimeDiagnostics(firstPage.diagnostics)));
    QCOMPARE(firstPage.metrics.replayedItems, 64ULL);
    QVERIFY(!firstPage.deltas.first().windows.first().successor.has_value());
    QCOMPARE(runtimeDiagnostics(firstPage.diagnostics),
             runtimeDiagnostics(legacy.diagnostics));
    const auto firstNodes = canonicalNodes(firstPage.deltas.first().tree,
                                           *compiled.program);
    for (auto item = firstNodes.cbegin(); item != firstNodes.cend(); ++item) {
        paged.insert(item.key(), item.value());
    }

    DisplayPageRequest second = first;
    second.sequenceWindows = {{sequence.locator, 64, 66}};
    const DisplayPageResult secondPage = document.requestDisplayPage(second);
    QVERIFY2(secondPage.success(),
             qPrintable(runtimeDiagnostics(secondPage.diagnostics)));
    const auto secondNodes = canonicalNodes(secondPage.deltas.first().tree,
                                            *compiled.program);
    for (auto item = secondNodes.cbegin(); item != secondNodes.cend(); ++item) {
        paged.insert(item.key(), item.value());
    }

    const auto legacyNodes = canonicalNodes(legacy.tree, *compiled.program);
    QCOMPARE(paged.size(), legacyNodes.size());
    for (auto item = legacyNodes.cbegin(); item != legacyNodes.cend(); ++item) {
        QVERIFY2(paged.contains(item.key()), qPrintable(item.value()));
        QCOMPARE(paged.value(item.key()), item.value());
    }
    QCOMPARE(runtimeDiagnostics(resolved.diagnostics),
             runtimeDiagnostics(legacy.diagnostics));

    ExportSpanRequest exportItem;
    exportItem.document = {1};
    InstanceLocator item = sequence.locator;
    item.sequenceIndexes.push_back(129);
    exportItem.target = item;
    const ExportSpanResult spans = document.requestExportSpans(exportItem);
    QVERIFY(spans.success());
    QCOMPARE(spans.spans.spans.size(), 1);
    QCOMPARE(spans.spans.spans.first().offset, 2ULL + 129ULL * 3ULL);
    QCOMPARE(spans.spans.spans.first().length, 3ULL);
}

void BrecoLangRuntimeTests::millionByteSequenceResolvesAndMaterializesConstantSize() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Run from data {
    select {
        when true => { items: repeat 1000000 { value: u8 } }
        else => { }
    }
}
)BRECO");
    const CompileResult compiled = compileBrecoLang(source);
    QVERIFY2(compiled.success(), qPrintable(compilerDiagnostics(compiled.diagnostics)));
    QByteArray bytes(1000000, '\x5a');
    DecodeRequest request;
    request.program = compiled.program;
    request.entryName = QStringLiteral("Run");
    request.inputs = {
        std::make_shared<BorrowedWindowSource>(bytes, QStringLiteral("million.bin"))};

    DecodeDocument document({2}, 42);
    QElapsedTimer timer;
    timer.start();
    const DecodeResult resolved = document.resolve(request);
    const qint64 elapsedMs = timer.elapsed();
    QVERIFY2(resolved.success(), qPrintable(runtimeDiagnostics(resolved.diagnostics)));
    QCOMPARE(resolved.shape->sequences.size(), 1);
    QCOMPARE(resolved.shape->sequences.first().arithmetic.count, 1000000ULL);
    QCOMPARE(resolved.metrics.arithmeticSkippedItems, 1000000ULL);
    QCOMPARE(resolved.constructedNodes, 2ULL);
    QCOMPARE(resolved.logicalNodes, 2000002ULL);
    QVERIFY2(elapsedMs < 500,
             qPrintable(QStringLiteral("shape resolution took %1 ms").arg(elapsedMs)));

    DisplayPageRequest pageRequest;
    pageRequest.document = {2};
    pageRequest.root = resolved.shape->root;
    const DisplayPageResult page = document.requestDisplayPage(pageRequest);
    QVERIFY2(page.success(), qPrintable(runtimeDiagnostics(page.diagnostics)));
    QCOMPARE(page.metrics.replayedItems, 64ULL);
    QCOMPARE(page.metrics.materializedNodes, 130ULL);
    QVERIFY(page.deltas.first().tree->nodes.size() < 200);
}

void BrecoLangRuntimeTests::inlineAggregatePagingUsesStableContainerLocator() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Run from data {
    select { when true => { items: repeat 3 { value: u8 } } else => { } }
    select { when true => { items: repeat 3 { value: u8 } } else => { } }
}
)BRECO");
    const CompileResult compiled = compileBrecoLang(source);
    QVERIFY2(compiled.success(),
             qPrintable(compilerDiagnostics(compiled.diagnostics)));
    DecodeRequest request;
    request.program = compiled.program;
    request.entryName = QStringLiteral("Run");
    request.inputs = {std::make_shared<BorrowedWindowSource>(
        QByteArray::fromHex("010203040506"), QStringLiteral("aggregate.bin"))};

    DecodeDocument document({19}, 43);
    const DecodeResult resolved = document.resolve(request);
    QVERIFY2(resolved.success(),
             qPrintable(runtimeDiagnostics(resolved.diagnostics)));
    QCOMPARE(resolved.shape->sequences.size(), 2);

    auto containerLocator =
        [&](const ResolvedSequenceShape& sequence) {
            DisplayPageRequest pageRequest;
            pageRequest.document = {19};
            pageRequest.root = resolved.shape->root;
            pageRequest.sequenceWindows = {{sequence.locator, 0, 2}};
            const DisplayPageResult page =
                document.requestDisplayPage(pageRequest);
            MaterializationLocator result;
            if (!page.success()) {
                return QPair<bool, MaterializationLocator>{false, result};
            }
            for (const MaterializedPageDelta& delta : page.deltas) {
                if (!delta.tree) {
                    continue;
                }
                for (DecodedNodeId id = 0;
                     id < static_cast<DecodedNodeId>(
                              delta.tree->nodes.size());
                     ++id) {
                    const DecodedNode& node = delta.tree->nodes.at(id);
                    if (!node.hidden &&
                        delta.tree->name(node.name) ==
                            QStringLiteral("items")) {
                        if (node.kind != DecodedNodeKind::Sequence) {
                            return QPair<bool, MaterializationLocator>{
                                false, {}};
                        }
                        result = delta.tree->locators.at(id);
                        break;
                    }
                }
            }
            return QPair<bool, MaterializationLocator>{result.isValid(),
                                                       result};
        };

    const auto first =
        containerLocator(resolved.shape->sequences.at(0));
    const auto second =
        containerLocator(resolved.shape->sequences.at(1));
    QVERIFY(first.first);
    QVERIFY(second.first);
    QCOMPARE(first.second, second.second);
}

void BrecoLangRuntimeTests::variableContinuationPreservesSemanticStateAndColdReplay() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_loop_iterations 100 max_nodes 10000 }
entry Run from data {
    items: while remaining > 1 with { seed: u64 = 7 } {
        value: u8
        computed checksum: u64 = seed + value + iteration
        check checksum == 7 + value + iteration else "state mismatch"
    }
    tail: u8
}
)BRECO");
    const CompileResult compiled = compileBrecoLang(source);
    QVERIFY2(compiled.success(), qPrintable(compilerDiagnostics(compiled.diagnostics)));
    const QByteArray bytes = QByteArray::fromHex("0102030463");

    DecodeRequest legacyRequest;
    legacyRequest.program = compiled.program;
    legacyRequest.entryName = QStringLiteral("Run");
    legacyRequest.mode = DecodeMode::Tree;
    legacyRequest.documentGeneration = 81;
    legacyRequest.inputs = {
        std::make_shared<BorrowedWindowSource>(bytes, QStringLiteral("state.bin"))};
    const DecodeResult legacy = decodeBrecoProgram(legacyRequest);
    QVERIFY2(legacy.success(), qPrintable(runtimeDiagnostics(legacy.diagnostics)));

    DecodeDocument document({11}, 81);
    DecodeRequest shapeRequest = legacyRequest;
    shapeRequest.mode = DecodeMode::ResolveShape;
    shapeRequest.inputs = {
        std::make_shared<BorrowedWindowSource>(bytes, QStringLiteral("state.bin"))};
    const DecodeResult resolved = document.resolve(shapeRequest);
    QVERIFY2(resolved.success(), qPrintable(runtimeDiagnostics(resolved.diagnostics)));
    QCOMPARE(resolved.shape->sequences.size(), 1);
    const ResolvedSequenceShape sequence = resolved.shape->sequences.first();
    QCOMPARE(sequence.indexKind, SequenceIndexKind::ForwardReplay);
    QCOMPARE(sequence.itemCount, 4ULL);
    QCOMPARE(sequence.displayCount, 4ULL);
    QVERIFY(sequence.startContinuation.has_value());
    QVERIFY(!resolved.shape->usesLegacyEagerTree);
    QCOMPARE(resolved.constructedNodes, 3ULL);
    QCOMPARE(resolved.logicalNodes, legacy.constructedNodes);
    QVERIFY(resolved.shape->outline->values.size() < 32);
    QCOMPARE(resolved.shape->outline->nodes.size(), 3);
    const auto outlineNodes = canonicalNodes(resolved.shape->outline,
                                             *compiled.program);
    const auto legacyAllNodes = canonicalNodes(legacy.tree,
                                               *compiled.program);
    for (auto item = outlineNodes.cbegin(); item != outlineNodes.cend(); ++item) {
        QVERIFY(legacyAllNodes.contains(item.key()));
        QCOMPARE(item.value(), legacyAllNodes.value(item.key()));
    }

    DisplayPageRequest first;
    first.document = {11};
    first.root = resolved.shape->root;
    first.sequenceWindows = {{sequence.locator, 0, 2}};
    const DisplayPageResult firstPage = document.requestDisplayPage(first);
    QVERIFY2(firstPage.success(), qPrintable(runtimeDiagnostics(firstPage.diagnostics)));
    const SequenceWindow firstApplied = firstPage.deltas.first().windows.first();
    QCOMPARE(firstApplied.itemCount, 2ULL);
    QVERIFY(firstApplied.successor.has_value());
    QCOMPARE(firstApplied.successor->nextItem, 2ULL);

    DisplayPageRequest second = first;
    second.sequenceWindows = {
        {sequence.locator, 2, 2, firstApplied.successor}};
    const DisplayPageResult secondPage = document.requestDisplayPage(second);
    QVERIFY2(secondPage.success(), qPrintable(runtimeDiagnostics(secondPage.diagnostics)));
    QCOMPARE(secondPage.metrics.resumedItems, 2ULL);
    QCOMPARE(secondPage.metrics.coldReplayedItems, 0ULL);
    QCOMPARE(secondPage.deltas.first().windows.first().successor->nextItem,
             4ULL);

    const DisplayPageResult forkedPage = document.requestDisplayPage(second);
    QVERIFY2(forkedPage.success(), qPrintable(runtimeDiagnostics(forkedPage.diagnostics)));
    QCOMPARE(canonicalNodes(secondPage.deltas.first().tree, *compiled.program),
             canonicalNodes(forkedPage.deltas.first().tree, *compiled.program));

    DisplayPageRequest tampered = second;
    tampered.sequenceWindows.first().successor->nextItem = 1;
    const DisplayPageResult rejected = document.requestDisplayPage(tampered);
    QCOMPARE(rejected.status, DecodeStatus::Error);

    DisplayPageRequest cold = first;
    cold.sequenceWindows = {{sequence.locator, 2, 2}};
    const DisplayPageResult coldPage = document.requestDisplayPage(cold);
    QVERIFY2(coldPage.success(), qPrintable(runtimeDiagnostics(coldPage.diagnostics)));
    QCOMPARE(coldPage.metrics.coldCursorOpens, 1ULL);
    QCOMPARE(coldPage.metrics.coldReplayedItems, 4ULL);

    auto itemNodes = [](const std::shared_ptr<const DecodedTree>& tree,
                        const BrecoProgram& program) {
        QHash<InstanceLocator, QString> result;
        if (!tree) return result;
        for (DecodedNodeId id = 0;
             id < static_cast<DecodedNodeId>(tree->nodes.size()) &&
             id < static_cast<DecodedNodeId>(tree->locators.size()); ++id) {
            if (!tree->locators.at(id).sequenceIndexes.isEmpty()) {
                result.insert(tree->locators.at(id),
                              nodeDigest(*tree, program, id));
            }
        }
        return result;
    };
    QHash<InstanceLocator, QString> resumedItems =
        itemNodes(firstPage.deltas.first().tree, *compiled.program);
    const auto latter = itemNodes(secondPage.deltas.first().tree,
                                  *compiled.program);
    for (auto item = latter.cbegin(); item != latter.cend(); ++item) {
        resumedItems.insert(item.key(), item.value());
    }
    QCOMPARE(resumedItems,
             itemNodes(legacy.tree, *compiled.program));
    QCOMPARE(itemNodes(secondPage.deltas.first().tree, *compiled.program),
             itemNodes(coldPage.deltas.first().tree, *compiled.program));
}

void BrecoLangRuntimeTests::variableReplayBudgetPausesAtCommittedBoundaries() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
limits { max_loop_iterations 100 max_nodes 10000 }
entry Run from data {
    items: while remaining > 1 with { seed: u64 = 9 } {
        value: u8
        computed carried: u64 = seed + iteration + value
    }
    tail: u8
}
)BRECO");
    const CompileResult compiled = compileBrecoLang(source);
    QVERIFY2(compiled.success(), qPrintable(compilerDiagnostics(compiled.diagnostics)));
    const QByteArray bytes = QByteArray::fromHex("0102030463");
    DecodeDocument document({12}, 82);
    DecodeRequest resolveRequest;
    resolveRequest.program = compiled.program;
    resolveRequest.entryName = QStringLiteral("Run");
    resolveRequest.documentGeneration = 82;
    resolveRequest.inputs = {
        std::make_shared<BorrowedWindowSource>(bytes, QStringLiteral("budget.bin"))};
    const DecodeResult resolved = document.resolve(resolveRequest);
    QVERIFY2(resolved.success(), qPrintable(runtimeDiagnostics(resolved.diagnostics)));
    const ResolvedSequenceShape sequence = resolved.shape->sequences.first();

    DisplayPageRequest pausedRequest;
    pausedRequest.document = {12};
    pausedRequest.root = resolved.shape->root;
    pausedRequest.sequenceWindows = {{sequence.locator, 3, 1}};
    pausedRequest.budget.maxWorkUnits = 2;
    const DisplayPageResult paused =
        document.requestDisplayPage(pausedRequest);
    QCOMPARE(paused.status, DecodeStatus::Paused);
    QVERIFY(!paused.deltas.isEmpty());
    const SequenceWindow progress = paused.deltas.first().windows.first();
    QCOMPARE(progress.itemCount, 0ULL);
    QVERIFY(progress.successor.has_value());
    QCOMPARE(progress.successor->nextItem, 2ULL);
    int partialItems = 0;
    for (const DecodedNode& node : paused.deltas.first().tree->nodes) {
        partialItems += node.kind == DecodedNodeKind::SequenceItem ? 1 : 0;
    }
    QCOMPARE(partialItems, 0);

    DisplayPageRequest resumed = pausedRequest;
    resumed.sequenceWindows = {
        {sequence.locator, 3, 1, progress.successor}};
    const DisplayPageResult completed = document.requestDisplayPage(resumed);
    QVERIFY2(completed.success(),
             qPrintable(runtimeDiagnostics(completed.diagnostics)));
    const SequenceWindow applied = completed.deltas.first().windows.first();
    QCOMPARE(applied.itemCount, 1ULL);
    QCOMPARE(applied.successor->nextItem, 4ULL);
    int completeItems = 0;
    for (const DecodedNode& node : completed.deltas.first().tree->nodes) {
        completeItems += node.kind == DecodedNodeKind::SequenceItem ? 1 : 0;
    }
    QCOMPARE(completeItems, 1);

    DisplayPageRequest partialRequest;
    partialRequest.document = {12};
    partialRequest.root = resolved.shape->root;
    partialRequest.sequenceWindows = {{sequence.locator, 0, 4}};
    partialRequest.budget.maxWorkUnits = 2;
    const DisplayPageResult partial =
        document.requestDisplayPage(partialRequest);
    QCOMPARE(partial.status, DecodeStatus::Paused);
    const SequenceWindow partialWindow =
        partial.deltas.first().windows.first();
    QCOMPARE(partialWindow.itemCount, 2ULL);
    QCOMPARE(partialWindow.successor->nextItem, 2ULL);
    int publishedItems = 0;
    for (const DecodedNode& node : partial.deltas.first().tree->nodes) {
        publishedItems += node.kind == DecodedNodeKind::SequenceItem ? 1 : 0;
    }
    QCOMPARE(publishedItems, 2);

    DisplayPageRequest finishPartial = partialRequest;
    finishPartial.sequenceWindows = {
        {sequence.locator, 2, 2, partialWindow.successor}};
    const DisplayPageResult finished =
        document.requestDisplayPage(finishPartial);
    QVERIFY2(finished.success(),
             qPrintable(runtimeDiagnostics(finished.diagnostics)));
    QCOMPARE(finished.deltas.first().windows.first().itemCount, 2ULL);
}

void BrecoLangRuntimeTests::manyAndRecoverUseForwardContinuations() {
    const CompileResult loopCompiled = compileBrecoLang(kLoopProgram);
    QVERIFY(loopCompiled.success());
    DecodeDocument loopDocument({15}, 85);
    DecodeRequest loopResolve;
    loopResolve.program = loopCompiled.program;
    loopResolve.entryName = QStringLiteral("Loops");
    loopResolve.documentGeneration = 85;
    loopResolve.inputs = {std::make_shared<BorrowedWindowSource>(
        QByteArray::fromHex("01020304050607aa"),
        QStringLiteral("loops.bin"))};
    const DecodeResult loopShape = loopDocument.resolve(loopResolve);
    QVERIFY2(loopShape.success(),
             qPrintable(runtimeDiagnostics(loopShape.diagnostics)));
    QVector<ResolvedSequenceShape> forwardLoops;
    for (const ResolvedSequenceShape& sequence :
         loopShape.shape->sequences) {
        if (sequence.indexKind == SequenceIndexKind::ForwardReplay) {
            forwardLoops.push_back(sequence);
        }
    }
    QCOMPARE(forwardLoops.size(), 2);
    for (const ResolvedSequenceShape& sequence : forwardLoops) {
        QCOMPARE(sequence.itemCount, 2ULL);
        DisplayPageRequest pageRequest;
        pageRequest.document = {15};
        pageRequest.root = loopShape.shape->root;
        pageRequest.sequenceWindows = {{sequence.locator, 0, 2}};
        const DisplayPageResult page =
            loopDocument.requestDisplayPage(pageRequest);
        QVERIFY2(page.success(), qPrintable(runtimeDiagnostics(page.diagnostics)));
        QCOMPARE(page.deltas.first().windows.first().successor->nextItem,
                 2ULL);
    }

    const CompileResult manyCompiled = compileBrecoLang(kTransactionalProgram);
    QVERIFY(manyCompiled.success());
    const QByteArray manyBytes = QByteArray::fromHex("a101a102ff");
    DecodeDocument manyDocument({13}, 83);
    DecodeRequest manyResolve;
    manyResolve.program = manyCompiled.program;
    manyResolve.entryName = QStringLiteral("ManyA");
    manyResolve.documentGeneration = 83;
    manyResolve.inputs = {std::make_shared<BorrowedWindowSource>(
        manyBytes, QStringLiteral("many.bin"))};
    const DecodeResult manyShape = manyDocument.resolve(manyResolve);
    QVERIFY2(manyShape.success(),
             qPrintable(runtimeDiagnostics(manyShape.diagnostics)));
    const ResolvedSequenceShape many = manyShape.shape->sequences.first();
    QCOMPARE(many.indexKind, SequenceIndexKind::ForwardReplay);
    QCOMPARE(many.itemCount, 2ULL);
    DisplayPageRequest manyFirst;
    manyFirst.document = {13};
    manyFirst.root = manyShape.shape->root;
    manyFirst.sequenceWindows = {{many.locator, 0, 1}};
    const DisplayPageResult manyPage1 =
        manyDocument.requestDisplayPage(manyFirst);
    QVERIFY2(manyPage1.success(),
             qPrintable(runtimeDiagnostics(manyPage1.diagnostics)));
    DisplayPageRequest manySecond = manyFirst;
    manySecond.sequenceWindows = {
        {many.locator, 1, 1,
         manyPage1.deltas.first().windows.first().successor}};
    const DisplayPageResult manyPage2 =
        manyDocument.requestDisplayPage(manySecond);
    QVERIFY2(manyPage2.success(),
             qPrintable(runtimeDiagnostics(manyPage2.diagnostics)));
    QCOMPARE(manyPage2.deltas.first().windows.first().successor->nextItem,
             2ULL);

    const CompileResult recoverCompiled = compileBrecoLang(kRecoverProgram);
    QVERIFY(recoverCompiled.success());
    const QByteArray recoverBytes = QByteArray::fromHex("7e01aabb7e02");
    DecodeDocument recoverDocument({14}, 84);
    DecodeRequest recoverResolve;
    recoverResolve.program = recoverCompiled.program;
    recoverResolve.entryName = QStringLiteral("Recover");
    recoverResolve.documentGeneration = 84;
    recoverResolve.inputs = {std::make_shared<BorrowedWindowSource>(
        recoverBytes, QStringLiteral("recover.bin"))};
    const DecodeResult recoverShape = recoverDocument.resolve(recoverResolve);
    QVERIFY2(recoverShape.success(),
             qPrintable(runtimeDiagnostics(recoverShape.diagnostics)));
    const ResolvedSequenceShape recovery =
        recoverShape.shape->sequences.first();
    QCOMPARE(recovery.indexKind, SequenceIndexKind::ForwardReplay);
    QCOMPARE(recovery.itemCount, 2ULL);
    QCOMPARE(recovery.displayCount, 3ULL);

    DisplayPageRequest recoverFirst;
    recoverFirst.document = {14};
    recoverFirst.root = recoverShape.shape->root;
    recoverFirst.sequenceWindows = {{recovery.locator, 0, 2}};
    const DisplayPageResult recoveryPage1 =
        recoverDocument.requestDisplayPage(recoverFirst);
    QVERIFY2(recoveryPage1.success(),
             qPrintable(runtimeDiagnostics(recoveryPage1.diagnostics)));
    int firstItems = 0;
    int firstGaps = 0;
    for (const DecodedNode& node : recoveryPage1.deltas.first().tree->nodes) {
        firstItems += node.kind == DecodedNodeKind::Record ? 1 : 0;
        firstGaps += node.kind == DecodedNodeKind::Gap ? 1 : 0;
    }
    QCOMPARE(firstItems, 1);
    QCOMPARE(firstGaps, 1);
    QCOMPARE(recoveryPage1.deltas.first().windows.first().successor->nextItem,
             2ULL);

    DisplayPageRequest recoverSecond = recoverFirst;
    recoverSecond.sequenceWindows = {
        {recovery.locator, 2, 1,
         recoveryPage1.deltas.first().windows.first().successor}};
    const DisplayPageResult recoveryPage2 =
        recoverDocument.requestDisplayPage(recoverSecond);
    QVERIFY2(recoveryPage2.success(),
             qPrintable(runtimeDiagnostics(recoveryPage2.diagnostics)));
    QCOMPARE(recoveryPage2.deltas.first().windows.first().successor->nextItem,
             3ULL);
}

void BrecoLangRuntimeTests::referencesAreLazyCanonicalAndDecodeBoundedTargets() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
record Node {
    value: u8
    next_offset: u8
    next: ref Node
        from data at root_offset(next_offset)
        within bytes(2)
        key next_offset
        follow
    self_byte: ref u8
        from data at self_offset(0)
        within bytes(1)
        key value
        weak
}
entry Run from data {
    first_offset: u8
    first: ref Node
        from data at root_offset(first_offset)
        within bytes(2)
        key first_offset
        follow
    alias: ref Node
        from data at root_offset(first_offset)
        within bytes(2)
        key first_offset
        follow
    alternate_representation: ref Node
        from data at root_offset(first_offset + 2)
        within bytes(2)
        key first_offset
        follow
    input_zero: ref u8
        from data at input_offset(0)
        within bytes(1)
        weak
    missing: ref Node
        from data at root_offset(0xffff)
        within bytes(2)
        follow
        when false
    computed first_address: u64 = first.@address
    computed first_key: u64 = first.@key
    computed first_length: u64 = first.@length
    computed missing_null: bool = missing.@is_null
}
entry Semantic from data {
    first_offset: u8
    first: ref Node
        from data at root_offset(first_offset)
        within bytes(2)
        key first_offset
        follow
    computed target_value: u8 = deref(first).value
}
)BRECO");
    const CompileResult compiled = compileBrecoLang(source);
    QVERIFY2(compiled.success(),
             qPrintable(compilerDiagnostics(compiled.diagnostics)));
    QByteArray bytes(10, '\0');
    bytes[4] = '\x02';
    bytes[6] = static_cast<char>(0xa1);
    bytes[7] = '\x04';
    bytes[8] = static_cast<char>(0xb2);
    bytes[9] = '\x02';

    DecodeRequest resolveRequest;
    resolveRequest.program = compiled.program;
    resolveRequest.entryName = QStringLiteral("Run");
    resolveRequest.documentGeneration = 901;
    resolveRequest.startOffset = 4;
    resolveRequest.inputs = {std::make_shared<BorrowedWindowSource>(
        bytes, QStringLiteral("embedded.bin"), 0x1000)};
    DecodeDocument document({91}, 901);
    const DecodeResult resolved = document.resolve(resolveRequest);
    QVERIFY2(resolved.success(),
             qPrintable(runtimeDiagnostics(resolved.diagnostics)));
    QVERIFY(resolved.shape != nullptr);
    QCOMPARE(resolved.constructedNodes, 11ULL);
    const DecodedValueId firstLength = field(
        *resolved.shape->outline, *compiled.program,
        resolved.shape->outline->nodes.first().value,
        QStringLiteral("first_length"));
    QVERIFY(firstLength != kInvalidId);
    QCOMPARE(resolved.shape->outline->values.at(firstLength).unsignedValue,
             2ULL);

    QVector<ReferenceHandle> rootReferences;
    for (const DecodedValue& value : resolved.shape->outline->values) {
        if (value.kind == DecodedValueKind::Reference &&
            value.payload < static_cast<quint32>(
                                resolved.shape->outline->references.size())) {
            rootReferences.push_back(
                resolved.shape->outline->references.at(value.payload));
        }
    }
    QCOMPARE(rootReferences.size(), 5);
    const ReferenceHandle first = rootReferences.at(0);
    const ReferenceHandle alias = rootReferences.at(1);
    const ReferenceHandle alternate = rootReferences.at(2);
    QCOMPARE(first.target.logicalOffset, 6ULL);
    QCOMPARE(first.target.regionLength, 2ULL);
    QCOMPARE(first.target.identity, alias.target.identity);
    QCOMPARE(first.target.identity, alternate.target.identity);
    QVERIFY(first.target != alternate.target);
    QCOMPARE(alternate.target.logicalOffset, 8ULL);
    QCOMPARE(first.target.identity.kind, ReferenceIdentityKind::Explicit);
    ReferenceTargetIdentity observedElsewhere = first.target.identity;
    observedElsewhere.physicalInput = 7;
    observedElsewhere.physicalOffset = 0xdeadbeef;
    observedElsewhere.physicalLength = 99;
    QCOMPARE(observedElsewhere, first.target.identity);
    QCOMPARE(qHash(observedElsewhere), qHash(first.target.identity));
    QCOMPARE(rootReferences.at(3).target.logicalOffset, 0ULL);
    QCOMPARE(rootReferences.at(3).target.identity.kind,
             ReferenceIdentityKind::Physical);
    QCOMPARE(rootReferences.at(3).target.identity.physicalOffset, 0ULL);
    QCOMPARE(rootReferences.at(3).target.identity.physicalLength, 1ULL);
    ReferenceTargetIdentity otherPhysical =
        rootReferences.at(3).target.identity;
    otherPhysical.physicalOffset = 1;
    QVERIFY(otherPhysical != rootReferences.at(3).target.identity);
    QVERIFY(rootReferences.at(4).isNull);
    QVERIFY(!rootReferences.at(4).target.isValid());
    QVERIFY(!first.owner.isReferenceTarget());

    DecodeRequest scanRequest = resolveRequest;
    scanRequest.mode = DecodeMode::ReferenceScan;
    const DecodeResult scanned = decodeBrecoProgram(scanRequest);
    QVERIFY2(scanned.success(),
             qPrintable(runtimeDiagnostics(scanned.diagnostics)));
    QVERIFY(scanned.tree == nullptr);
    QCOMPARE(scanned.constructedNodes, 0ULL);
    QCOMPARE(scanned.referenceEvents.size(), 5);

    DisplayPageRequest targetRequest;
    targetRequest.document = {91};
    targetRequest.root = first.targetLocator();
    targetRequest.expansionPath = {MaterializationLocator{
        InstanceLocator{901, {first.referenceTemplate}, {}}}};
    const DisplayPageResult targetPage =
        document.requestDisplayPage(targetRequest);
    QVERIFY2(targetPage.success(),
             qPrintable(runtimeDiagnostics(targetPage.diagnostics)));
    QVERIFY(!targetPage.deltas.isEmpty());
    const std::shared_ptr<const DecodedTree> targetTree =
        targetPage.deltas.first().tree;
    QVERIFY(targetTree != nullptr);
    QCOMPARE(targetTree->nodes.first().offset, 0x1006ULL);
    QCOMPARE(targetTree->nodes.first().length, 2ULL);
    QCOMPARE(targetTree->locators.first().rootKind,
             MaterializationRootKind::ReferenceTarget);

    QVector<ReferenceHandle> nested;
    for (const ReferenceHandle& handle : targetTree->references) {
        nested.push_back(handle);
    }
    QCOMPARE(nested.size(), 2);
    QVERIFY(nested.first().owner.isReferenceTarget());
    QVERIFY(nested.first().owner.referenceTarget.has_value());
    QCOMPARE(*nested.first().owner.referenceTarget, first.target);
    QCOMPARE(nested.first().target.logicalOffset, 8ULL);
    QCOMPARE(nested.at(1).target.logicalOffset, 6ULL);

    DisplayPageRequest aliasRequest = targetRequest;
    aliasRequest.expansionPath = {MaterializationLocator{
        InstanceLocator{901, {alias.referenceTemplate}, {1}}}};
    const DisplayPageResult aliasPage =
        document.requestDisplayPage(aliasRequest);
    QVERIFY(aliasPage.success());
    QCOMPARE(aliasPage.metrics.cacheHits, 2ULL);
    QVERIFY(aliasPage.deltas.first().tree == targetTree);

    DisplayPageRequest alternateRequest = targetRequest;
    alternateRequest.root = alternate.targetLocator();
    alternateRequest.expansionPath = {MaterializationLocator{
        InstanceLocator{901, {alternate.referenceTemplate}, {2}}}};
    const DisplayPageResult alternatePage =
        document.requestDisplayPage(alternateRequest);
    QVERIFY2(alternatePage.success(),
             qPrintable(runtimeDiagnostics(alternatePage.diagnostics)));
    QVERIFY(alternatePage.deltas.first().tree != targetTree);
    QCOMPARE(alternatePage.deltas.first().tree->nodes.first().offset,
             0x1008ULL);

    DisplayPageRequest boundedRequest = targetRequest;
    ReferenceTargetKey shortTarget = first.target;
    shortTarget.regionLength = 1;
    boundedRequest.root = MaterializationLocator::target(shortTarget);
    const DisplayPageResult boundedPage =
        document.requestDisplayPage(boundedRequest);
    QVERIFY(!boundedPage.success());
    QVERIFY(runtimeDiagnostics(boundedPage.diagnostics)
                .contains(QStringLiteral("BRR0313")));

    DecodeRequest semanticRequest = resolveRequest;
    semanticRequest.entryName = QStringLiteral("Semantic");
    semanticRequest.mode = DecodeMode::Tree;
    const DecodeResult semantic = decodeBrecoProgram(semanticRequest);
    QVERIFY2(semantic.success(),
             qPrintable(runtimeDiagnostics(semantic.diagnostics)));
    const DecodedValueId targetValue = field(
        *semantic.tree, *compiled.program, semantic.rootValue,
        QStringLiteral("target_value"));
    QVERIFY(targetValue != kInvalidId);
    QCOMPARE(semantic.tree->values.at(targetValue).unsignedValue, 0xa1ULL);

    const QString parameterSource = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
record ParameterTarget(count: u8) {
    items: repeat count { value: u8 }
}
entry Parameterized from data {
    count: u8
    target: ref ParameterTarget(count)
        from data at root_offset(1)
        within bytes(count)
        key count
        weak
}
)BRECO");
    const CompileResult parameterCompiled = compileBrecoLang(parameterSource);
    QVERIFY2(parameterCompiled.success(),
             qPrintable(compilerDiagnostics(parameterCompiled.diagnostics)));
    DecodeDocument parameterDocument({94}, 904);
    DecodeRequest parameterRequest;
    parameterRequest.program = parameterCompiled.program;
    parameterRequest.entryName = QStringLiteral("Parameterized");
    parameterRequest.documentGeneration = 904;
    parameterRequest.inputs = {std::make_shared<BorrowedWindowSource>(
        QByteArray::fromHex("03a1a2a3"), QStringLiteral("parameter.bin"))};
    const DecodeResult parameterShape =
        parameterDocument.resolve(parameterRequest);
    QVERIFY2(parameterShape.success(),
             qPrintable(runtimeDiagnostics(parameterShape.diagnostics)));
    QCOMPARE(parameterShape.shape->outline->references.size(), 1);
    const ReferenceHandle parameterHandle =
        parameterShape.shape->outline->references.first();
    QCOMPARE(parameterHandle.target.identity.targetArguments.values.size(), 1);
    DisplayPageRequest parameterPageRequest;
    parameterPageRequest.document = {94};
    parameterPageRequest.root = parameterHandle.targetLocator();
    const DisplayPageResult parameterPage =
        parameterDocument.requestDisplayPage(parameterPageRequest);
    QVERIFY2(parameterPage.success(),
             qPrintable(runtimeDiagnostics(parameterPage.diagnostics)));
    QCOMPARE(parameterPage.deltas.first().shape->sequences.size(), 1);
    QCOMPARE(parameterPage.deltas.first().shape->sequences.first().indexKind,
             SequenceIndexKind::Arithmetic);
    int parameterItems = 0;
    for (const DecodedNode& node : parameterPage.deltas.first().tree->nodes) {
        parameterItems += node.kind == DecodedNodeKind::SequenceItem ? 1 : 0;
    }
    QCOMPARE(parameterItems, 3);
}

void BrecoLangRuntimeTests::referenceArithmeticAndStructuralPlansStayCheckedAndLazy() {
    const QString fixedSource = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Run from data {
    items: repeat 1000001 {
        pointer: u8
        target: ref u8
            from data at input_offset(pointer)
            within bytes(1)
            key pointer
            weak
    }
}
)BRECO");
    const CompileResult fixed = compileBrecoLang(fixedSource);
    QVERIFY2(fixed.success(),
             qPrintable(compilerDiagnostics(fixed.diagnostics)));
    QByteArray bytes(1000001, '\0');
    DecodeDocument document({92}, 902);
    DecodeRequest request;
    request.program = fixed.program;
    request.entryName = QStringLiteral("Run");
    request.documentGeneration = 902;
    request.inputs = {std::make_shared<BorrowedWindowSource>(
        bytes, QStringLiteral("large.bin"))};
    const DecodeResult shape = document.resolve(request);
    QVERIFY2(shape.success(), qPrintable(runtimeDiagnostics(shape.diagnostics)));
    QCOMPARE(shape.shape->sequences.first().indexKind,
             SequenceIndexKind::Arithmetic);
    QCOMPARE(shape.metrics.arithmeticSkippedItems, 1000001ULL);
    QCOMPARE(shape.referenceEvents.size(), 0);
    QCOMPARE(shape.constructedNodes, 2ULL);

    DisplayPageRequest pageRequest;
    pageRequest.document = {92};
    pageRequest.root = shape.shape->root;
    pageRequest.sequenceWindows = {
        {shape.shape->sequences.first().locator, 0, 1}};
    const DisplayPageResult page = document.requestDisplayPage(pageRequest);
    QVERIFY2(page.success(), qPrintable(runtimeDiagnostics(page.diagnostics)));
    QCOMPARE(page.metrics.replayedItems, 1ULL);
    QCOMPARE(page.deltas.first().tree->references.size(), 1);

    const QString overflowSource = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Overflow from data {
    pointer: u64le
    target: ref u8
        from data at input_offset(pointer + 1)
        within bytes(1)
        weak
}
)BRECO");
    const CompileResult overflowCompiled = compileBrecoLang(overflowSource);
    QVERIFY(overflowCompiled.success());
    const DecodeResult overflow = decode(
        overflowCompiled.program, QStringLiteral("Overflow"),
        {{QStringLiteral("data"), QByteArray::fromHex("ffffffffffffffff")}});
    QVERIFY(!overflow.success());
    QVERIFY(runtimeDiagnostics(overflow.diagnostics)
                .contains(QStringLiteral("BRR0441")));

    const QString keyOverflowSource = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry KeyOverflow from data {
    pointer: u64le
    target: ref u8
        from data at input_offset(0)
        within bytes(1)
        key pointer + 1
        weak
}
)BRECO");
    const CompileResult keyOverflowCompiled =
        compileBrecoLang(keyOverflowSource);
    QVERIFY(keyOverflowCompiled.success());
    const DecodeResult keyOverflow = decode(
        keyOverflowCompiled.program, QStringLiteral("KeyOverflow"),
        {{QStringLiteral("data"), QByteArray::fromHex("ffffffffffffffff")}});
    QVERIFY(!keyOverflow.success());
    QVERIFY(runtimeDiagnostics(keyOverflow.diagnostics)
                .contains(QStringLiteral("BRR0441")));

    const QString nullSource = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Null from data {
    target: ref u8
        from data at input_offset(0xffffffffffffffff)
        within bytes(2)
        follow
        when false
    computed null_value: bool = target.@is_null
}
)BRECO");
    const CompileResult nullCompiled = compileBrecoLang(nullSource);
    QVERIFY(nullCompiled.success());
    const DecodeResult nullResult = decode(
        nullCompiled.program, QStringLiteral("Null"),
        {{QStringLiteral("data"), QByteArray(1, '\0')}});
    QVERIFY2(nullResult.success(),
             qPrintable(runtimeDiagnostics(nullResult.diagnostics)));

    const QString outOfRegionSource = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry OutOfRegion from data {
    target: ref u8
        from data at input_offset(2)
        within bytes(1)
        weak
}
)BRECO");
    const CompileResult outOfRegionCompiled =
        compileBrecoLang(outOfRegionSource);
    QVERIFY(outOfRegionCompiled.success());
    const DecodeResult outOfRegion = decode(
        outOfRegionCompiled.program, QStringLiteral("OutOfRegion"),
        {{QStringLiteral("data"), QByteArray(1, '\0')}});
    QVERIFY(!outOfRegion.success());
    QVERIFY(runtimeDiagnostics(outOfRegion.diagnostics)
                .contains(QStringLiteral("BRR0226")));

    const QString variableSource = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Variable from data {
    items: while remaining > 1 {
        pointer: u8
        target: ref u8
            from data at input_offset(pointer)
            within bytes(1)
            key pointer
            weak
    }
    tail: u8
}
)BRECO");
    const CompileResult variableCompiled = compileBrecoLang(variableSource);
    QVERIFY2(variableCompiled.success(),
             qPrintable(compilerDiagnostics(variableCompiled.diagnostics)));
    DecodeDocument variableDocument({93}, 903);
    DecodeRequest variableRequest;
    variableRequest.program = variableCompiled.program;
    variableRequest.entryName = QStringLiteral("Variable");
    variableRequest.documentGeneration = 903;
    variableRequest.inputs = {std::make_shared<BorrowedWindowSource>(
        QByteArray::fromHex("0102030463"), QStringLiteral("variable.bin"))};
    const DecodeResult variableShape =
        variableDocument.resolve(variableRequest);
    QVERIFY2(variableShape.success(),
             qPrintable(runtimeDiagnostics(variableShape.diagnostics)));
    QCOMPARE(variableShape.shape->sequences.size(), 1);
    const ResolvedSequenceShape variableSequence =
        variableShape.shape->sequences.first();
    QCOMPARE(variableSequence.indexKind, SequenceIndexKind::ForwardReplay);
    QCOMPARE(variableSequence.itemCount, 4ULL);
    QCOMPARE(variableShape.referenceEvents.size(), 0);
    QCOMPARE(variableShape.shape->outline->references.size(), 0);

    DisplayPageRequest variableFirst;
    variableFirst.document = {93};
    variableFirst.root = variableShape.shape->root;
    variableFirst.sequenceWindows = {{variableSequence.locator, 0, 1}};
    const DisplayPageResult variableFirstPage =
        variableDocument.requestDisplayPage(variableFirst);
    QVERIFY2(variableFirstPage.success(),
             qPrintable(runtimeDiagnostics(variableFirstPage.diagnostics)));
    QCOMPARE(variableFirstPage.deltas.first().tree->references.size(), 1);
    QVERIFY(variableFirstPage.deltas.first().windows.first().successor
                .has_value());

    DisplayPageRequest variableSecond = variableFirst;
    variableSecond.sequenceWindows = {{
        variableSequence.locator, 1, 1,
        variableFirstPage.deltas.first().windows.first().successor}};
    const DisplayPageResult variableSecondPage =
        variableDocument.requestDisplayPage(variableSecond);
    QVERIFY2(variableSecondPage.success(),
             qPrintable(runtimeDiagnostics(variableSecondPage.diagnostics)));
    QCOMPARE(variableSecondPage.deltas.first().tree->references.size(), 1);
}

void BrecoLangRuntimeTests::shippedElfReferenceExampleNavigatesSample() {
    QFile schema(QStringLiteral(
        BRECO_SOURCE_DIR "/examples/elf64_references.breco"));
    QVERIFY2(schema.open(QIODevice::ReadOnly), qPrintable(schema.errorString()));
    const CompileResult compiled = compileBrecoLang(
        QString::fromUtf8(schema.readAll()), schema.fileName());
    QVERIFY2(compiled.success(),
             qPrintable(compilerDiagnostics(compiled.diagnostics)));

    QFile sample(QStringLiteral(
        BRECO_SOURCE_DIR "/examples/samples/elf64-reference-sample.o"));
    QVERIFY2(sample.open(QIODevice::ReadOnly), qPrintable(sample.errorString()));
    const QByteArray bytes = sample.readAll();
    QCOMPARE(bytes.size(), 944);

    DecodeRequest request;
    request.program = compiled.program;
    request.entryName = QStringLiteral("ELF64References");
    request.documentGeneration = 906;
    request.inputs = {std::make_shared<BorrowedWindowSource>(
        bytes, sample.fileName())};
    DecodeDocument document({96}, 906);
    const DecodeResult resolved = document.resolve(request);
    QVERIFY2(resolved.success(),
             qPrintable(runtimeDiagnostics(resolved.diagnostics)));
    QVERIFY(resolved.shape != nullptr);
    const std::shared_ptr<const DecodedTree> outline =
        resolved.shape->outline;
    QVERIFY(outline != nullptr);
    QCOMPARE(outline->references.size(), 2);
    const ReferenceHandle file = outline->references.at(0);
    const ReferenceHandle header = outline->references.at(1);
    QCOMPARE(file.target.logicalOffset, 0ULL);
    QCOMPARE(file.target.regionLength, 944ULL);
    QCOMPARE(file.strength, ReferenceStrength::Follow);
    QCOMPARE(header.target.logicalOffset, 0ULL);
    QCOMPARE(header.target.regionLength, 64ULL);
    QCOMPARE(header.strength, ReferenceStrength::Weak);

    const DecodedValueId root = outline->nodes.first().value;
    const auto unsignedField = [&](QStringView name) -> std::optional<quint64> {
        const DecodedValueId value =
            field(*outline, *compiled.program, root, name);
        if (value == kInvalidId) {
            return std::nullopt;
        }
        return outline->values.at(value).unsignedValue;
    };
    QCOMPARE(unsignedField(u"file_key"), std::optional<quint64>(0));
    QCOMPARE(unsignedField(u"file_address"), std::optional<quint64>(0));
    QCOMPARE(unsignedField(u"file_length"), std::optional<quint64>(944));
    QCOMPARE(unsignedField(u"machine_via_deref"), std::optional<quint64>(62));
    const DecodedValueId fileIsNull =
        field(*outline, *compiled.program, root, u"file_is_null");
    QVERIFY(fileIsNull != kInvalidId);
    QVERIFY(!outline->values.at(fileIsNull).booleanValue);

    const ReferenceDesc& fileDesc =
        compiled.program->references.at(file.referenceTemplate);
    const ReferenceDesc& headerDesc =
        compiled.program->references.at(header.referenceTemplate);
    QCOMPARE(fileDesc.address.base, AddressBaseKind::Input);
    QCOMPARE(fileDesc.coverage, ReferenceCoverage::WholeRegion);
    QCOMPARE(headerDesc.address.base, AddressBaseKind::EntryRoot);
    QCOMPARE(headerDesc.coverage, ReferenceCoverage::DecodedStorage);

    DisplayPageRequest fileRequest;
    fileRequest.document = {96};
    fileRequest.root = file.targetLocator();
    const DisplayPageResult filePage =
        document.requestDisplayPage(fileRequest);
    QVERIFY2(filePage.success(),
             qPrintable(runtimeDiagnostics(filePage.diagnostics)));
    QCOMPARE(filePage.deltas.first().tree->references.size(), 1);
    const ReferenceHandle sectionTable =
        filePage.deltas.first().tree->references.first();
    QCOMPARE(sectionTable.target.logicalOffset, 368ULL);
    QCOMPARE(sectionTable.target.regionLength, 9ULL * 64ULL);
    QVERIFY(sectionTable.owner.isReferenceTarget());
    const ReferenceDesc& tableDesc =
        compiled.program->references.at(sectionTable.referenceTemplate);
    QCOMPARE(tableDesc.address.base, AddressBaseKind::ContainingEntity);
    QCOMPARE(tableDesc.strength, ReferenceStrength::Follow);
    QCOMPARE(tableDesc.coverage, ReferenceCoverage::DecodedStorage);

    DisplayPageRequest tableRequest;
    tableRequest.document = {96};
    tableRequest.root = sectionTable.targetLocator();
    const DisplayPageResult tablePage =
        document.requestDisplayPage(tableRequest);
    QVERIFY2(tablePage.success(),
             qPrintable(runtimeDiagnostics(tablePage.diagnostics)));
    QCOMPARE(tablePage.deltas.first().shape->sequences.size(), 1);
    const ResolvedSequenceShape sequence =
        tablePage.deltas.first().shape->sequences.first();
    QCOMPARE(sequence.indexKind, SequenceIndexKind::Arithmetic);
    QCOMPARE(sequence.itemCount, 9ULL);
    const QVector<ReferenceHandle>& contents =
        tablePage.deltas.first().tree->references;
    QCOMPARE(contents.size(), 9);
    QVERIFY(contents.at(0).isNull);
    QVERIFY(!contents.at(1).isNull);
    QCOMPARE(contents.at(1).target.logicalOffset, 64ULL);
    QCOMPARE(contents.at(1).target.regionLength, 6ULL);
    QCOMPARE(contents.at(1).strength, ReferenceStrength::Weak);
    QVERIFY(contents.at(3).isNull);  // The real SHT_NOBITS .bss section.
    const ReferenceDesc& contentDesc =
        compiled.program->references.at(contents.at(1).referenceTemplate);
    QCOMPARE(contentDesc.address.base, AddressBaseKind::EntryRoot);
    QCOMPARE(contentDesc.coverage, ReferenceCoverage::WholeRegion);

    DisplayPageRequest textRequest;
    textRequest.document = {96};
    textRequest.root = contents.at(1).targetLocator();
    const DisplayPageResult textPage =
        document.requestDisplayPage(textRequest);
    QVERIFY2(textPage.success(),
             qPrintable(runtimeDiagnostics(textPage.diagnostics)));
    const std::shared_ptr<const DecodedTree> textTree =
        textPage.deltas.first().tree;
    QCOMPARE(textTree->nodes.first().offset, 64ULL);
    QCOMPARE(textTree->nodes.first().length, 6ULL);
    QCOMPARE(bytes.mid(64, 6), QByteArray::fromHex("b82a000000c3"));

    QBuffer json;
    QVERIFY(json.open(QIODevice::WriteOnly));
    DecodeRequest stream = request;
    stream.mode = DecodeMode::Streaming;
    stream.output = &json;
    const DecodeResult streamed = decodeBrecoProgram(stream);
    QVERIFY2(streamed.success(),
             qPrintable(runtimeDiagnostics(streamed.diagnostics)));
    const QJsonDocument jsonDocument = QJsonDocument::fromJson(json.data());
    QVERIFY(jsonDocument.isObject());
    QCOMPARE(jsonDocument.object().value(QStringLiteral("machine_via_deref"))
                 .toInt(),
             62);
}

}  // namespace

QTEST_APPLESS_MAIN(BrecoLangRuntimeTests)

#include "brecolang_runtime_tests.moc"

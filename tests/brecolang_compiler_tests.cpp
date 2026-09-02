#include <QSet>
#include <QTest>
#include <QDirIterator>
#include <QFile>

#include "brecolang/compiler/Compiler.h"
#include "brecolang/compiler/Lexer.h"
#include "brecolang/compiler/Parser.h"

namespace {

using namespace breco::lang;

const QString kCompleteProgram = QString::fromUtf8(R"BRECO(
language breco 0.1

inputs {
    input ppdw "PPDW (main entrypoint)" {
        default
        description "Packed pulse descriptor records"
    }
    input idx "IDX (index)" {
        description "Maps each primary record to a bulk-data range"
    }
    input iqdw "IQDW (bulk data)" {
        description "Framed sample data"
    }
}

limits {
    max_parse_depth 128
    max_loop_iterations 1_000_000
    max_nodes 5_000_000
    max_probe_bytes 1_MiB
    max_transform_output 256_MiB
}

const WORD_BYTES: u64 = 4
const IF_DATA_HEADER_BASIC_WORDS: u64 = 3
const AMMOS_FRAME_HEADER_WORDS: u64 = 3
const PPDW_RECORD_BYTES: u64 = 8
const INDEX_RECORD_BYTES: u64 = 16

enum FrameType: u32le {
    SplitIQ16 = 2
    SplitIQ32Float = 5
    UnsplitIQ16 = 0x201
}

enum PDWRevision: u8 {
    V2019 = 1
    V2024 = 2
}

record FixedPrefix {
    first: u8
    second: u32le
}

record IFDataHeaderExtendedTail {
    marker: u32le
}

record IFDataHeader {
    require remaining >= IF_DATA_HEADER_BASIC_WORDS * WORD_BYTES
        else "IF data header is shorter than its fixed prefix"
    datablock_count: u32le
    datablock_length_words: u32le
    data_status_word: bitfield u32le {
        samples_are_dbfs: bit 30
        user_flags: bits 7..0
    }
    antenna_voltage_reference_tenths_dbuv: i32le
    computed antenna_voltage_reference_dbuv: f64 =
        antenna_voltage_reference_tenths_dbuv / 10.0
    extended: IFDataHeaderExtendedTail when remaining >= 20
    preserve remaining as future_header_extension
}

record IFDataBlock16(words: u32) {
    samples: region bytes(words * WORD_BYTES) {
        raw remaining as data
    }
}

record PackedPDW2019 {
    timestamp: u32le
    flags: u32le
}

record PackedPDW2024 {
    timestamp: u64le
}

record SplitIQDWIndexRecord {
    iq_data_offset_bytes: u64le
    iq_data_length_bytes: u64le
}

record AMMOSFrame {
    identify {
        magic: u32le
        frame_length_words: u32le
        frame_type: FrameType
        match magic == 0xFB746572
            else "not a frame"
    }
    commit
    data_header: IFDataHeader
    body: region bytes((frame_length_words - AMMOS_FRAME_HEADER_WORDS) * WORD_BYTES) {
        select frame_type {
            FrameType.SplitIQ16 => {
                data_blocks: repeat data_header.datablock_count {
                    block: IFDataBlock16(data_header.datablock_length_words)
                }
            }
            default => raw remaining as unknown_frame_payload
        }
        preserve remaining as undecoded_frame_bytes
    }
    check frame_length_words >= AMMOS_FRAME_HEADER_WORDS
        else "frame length is smaller than its header"
}

record IQDWRange {
    frames: repeat { frame: AMMOSFrame } until at_end
}

record SplitRecording(revision: PDWRevision) {
    pulses: while remaining >= PPDW_RECORD_BYTES with {
        expected_iqdw_offset: u64 = 0
    } {
        pdw: select revision {
            PDWRevision.V2019 => PackedPDW2019
            PDWRevision.V2024 => PackedPDW2024
        }
        index: SplitIQDWIndexRecord from idx at iteration * INDEX_RECORD_BYTES
        continue when index.iq_data_length_bytes == 0
        iq_data: IQDWRange from iqdw at index.iq_data_offset_bytes
            within bytes(index.iq_data_length_bytes)
        break when at_end
    }
}

entry SplitRecording2019 from ppdw {
    recording: SplitRecording(PDWRevision.V2019)
}
default entry SplitRecording2019

entry FramesAtCurrentOffset from iqdw {
    frames: many AMMOSFrame
    preserve remaining as unmatched_tail
}

record ZeroPaddingRun {
    identify {
        zero: u32le
        match zero == 0 else "not zero padding"
    }
    commit
}

record RecoverableIQDWItem {
    item: one_of { as AMMOSFrame as ZeroPaddingRun }
}

entry RecoverIQDW from iqdw {
    items: recover RecoverableIQDWItem {
        sync one_of {
            bytes [0x72, 0x65, 0x74, 0xFB]
            bytes [0x00, 0x00, 0x00, 0x00]
        }
        step 1 byte
        max_probe 1_MiB
        gaps as Noise
    }
}

record ExperimentalNestedTLV(depth: u32) {
    identify {
        tag: u8
        payload_length: u16le
        match payload_length <= remaining else "TLV length exceeds region"
    }
    commit
    require depth < 64 else "experimental nesting limit reached"
    payload: region bytes(payload_length) {
        content: select {
            when tag == 0x7F => {
                children: many ExperimentalNestedTLV(depth + 1)
            }
            else => raw remaining as value
        }
    }
}

outform FrameSummary(root: FramesAtCurrentOffset) text {
    emit "index,input,offset,frame_type,length_words,valid\n"
    for frame, index in root.frames {
        let kind = enum_name(frame.frame_type)
        if frame.@valid {
            emit "${index},${frame.@input},${frame.@offset},${csv(kind)},${frame.frame_length_words},true\n"
        } else {
            emit "${index},${frame.@input},${frame.@offset},${csv(kind)},${frame.frame_length_words},false\n"
        }
    }
    if root.unmatched_tail.@length > 0 {
        emit "# unmatched_tail=${root.unmatched_tail.@length} bytes\n"
    }
}

outform FrameIndex(root: FramesAtCurrentOffset) binary {
    for frame in root.frames {
        emit u64le(frame.@offset)
        emit u32le(frame.frame_length_words)
        emit u32le(int(frame.frame_type))
    }
    if root.unmatched_tail.@length > 0 {
        emit root.unmatched_tail.@bytes
    }
}
)BRECO");

class BrecoLangCompilerTests : public QObject {
    Q_OBJECT

private slots:
    void lexerRecognizesCoreTokensAndUnits();
    void completeLanguageProgramCompiles();
    void omittedBoilerplateUsesImplicitDefaults();
    void anonymousRecordFieldsLowerToOrdinaryRecords();
    void resolvedProgramContainsTransactionalAndOutformPlans();
    void extentAndEffectAnalysisPropagatesUsefulFacts();
    void loopScanPlanIsConservativeAndKeepsItemExtent();
    void referencesLowerToExplicitIrAndKeepStructuralBatchPlan();
    void parserAndResolverBatchIndependentDiagnostics();
    void parserRecoversToLaterTopLevelDeclarations();
    void inlineSelectsMergeFieldsAndBuildAggregateMetadata();
    void shippedExamplesCompile();
};

QString diagnosticsText(const QVector<Diagnostic>& diagnostics) {
    QStringList lines;
    for (const Diagnostic& diagnostic : diagnostics) {
        lines.push_back(QStringLiteral("%1: %2 at %3")
                            .arg(diagnostic.code, diagnostic.message)
                            .arg(diagnostic.span.start));
    }
    return lines.join(QLatin1Char('\n'));
}

void BrecoLangCompilerTests::lexerRecognizesCoreTokensAndUnits() {
    const QString source = QStringLiteral(
        "record X { bits: bitfield u32le { hi: bits 7..0 } } "
        "const LIMIT = 1_MiB // comment\n"
        "outform F(root: X) text { let role = root.@input emit \"${role}\" }");
    const LexResult result = lexBrecoLang(source);
    QVERIFY2(result.diagnostics.isEmpty(),
             qPrintable(diagnosticsText(result.diagnostics)));
    QVERIFY(std::any_of(result.tokens.cbegin(), result.tokens.cend(),
                        [](const Token& token) {
                            return token.kind == TokenKind::DotDot;
                        }));
    QVERIFY(std::any_of(result.tokens.cbegin(), result.tokens.cend(),
                        [](const Token& token) {
                            return token.kind == TokenKind::At;
                        }));
    quint64 value = 0;
    QVERIFY(parseUnsignedLiteral(QStringView(u"1_MiB"), &value));
    QCOMPARE(value, 1024ULL * 1024ULL);
}

void BrecoLangCompilerTests::completeLanguageProgramCompiles() {
    const CompileResult result =
        compileBrecoLang(kCompleteProgram, QStringLiteral("complete.breco"));
    QVERIFY2(result.success(), qPrintable(diagnosticsText(result.diagnostics)));
    QVERIFY(result.syntax != nullptr);
    QCOMPARE(result.program->languageVersion, QStringLiteral("0.1"));
    QCOMPARE(result.program->inputs.size(), 3);
    QCOMPARE(result.program->enums.size(), 2);
    QCOMPARE(result.program->entries.size(), 3);
    QCOMPARE(result.program->outforms.size(), 2);
    QCOMPARE(result.program->limits.maxProbeBytes, 1024ULL * 1024ULL);
    QCOMPARE(result.program->limits.maxTransformOutput,
             256ULL * 1024ULL * 1024ULL);
    QCOMPARE(result.program->symbol(result.program->defaultEntry),
             QStringLiteral("SplitRecording2019"));
}

void BrecoLangCompilerTests::omittedBoilerplateUsesImplicitDefaults() {
    const CompileResult implicit = compileBrecoLang(QString::fromUtf8(R"BRECO(
record Vis {
    computed MyString: string = "Test"
    computed MyNumber: i16 = 123
    A: u8
    B: u8
    C: u8
}
)BRECO"));
    QVERIFY2(implicit.success(),
             qPrintable(diagnosticsText(implicit.diagnostics)));
    QCOMPARE(implicit.program->languageVersion, QStringLiteral("0.1"));
    QCOMPARE(implicit.program->inputs.size(), 1);
    QCOMPARE(implicit.program->symbol(implicit.program->inputs.first().name),
             QStringLiteral("data"));
    QVERIFY(implicit.program->inputs.first().isDefault);
    QCOMPARE(implicit.program->records.size(), 1);
    QCOMPARE(implicit.program->entries.size(), 1);
    QCOMPARE(implicit.program->symbol(implicit.program->entries.first().name),
             QStringLiteral("Vis"));
    QCOMPARE(implicit.program->entries.first().resultType,
             implicit.program->records.first().type);
    QCOMPARE(implicit.program->defaultEntry,
             implicit.program->entries.first().name);

    const CompileResult implicitInput = compileBrecoLang(QString::fromUtf8(R"BRECO(
entry Main from data { value: u8 }
)BRECO"));
    QVERIFY2(implicitInput.success(),
             qPrintable(diagnosticsText(implicitInput.diagnostics)));
    QCOMPARE(implicitInput.program->inputs.size(), 1);
    QCOMPARE(implicitInput.program->entries.size(), 1);

    const CompileResult ambiguous = compileBrecoLang(QString::fromUtf8(R"BRECO(
record First { value: u8 }
record Second { value: u8 }
)BRECO"));
    QVERIFY2(ambiguous.success(),
             qPrintable(diagnosticsText(ambiguous.diagnostics)));
    QVERIFY(ambiguous.program->entries.isEmpty());
    QCOMPARE(ambiguous.program->defaultEntry, kInvalidId);

    const CompileResult emptyInputs = compileBrecoLang(QString::fromUtf8(R"BRECO(
inputs { }
record Only { value: u8 }
)BRECO"));
    QVERIFY(!emptyInputs.success());
    QVERIFY(diagnosticsText(emptyInputs.diagnostics)
                .contains(QStringLiteral("BR0201")));

    const CompileResult wrongLanguage =
        compileBrecoLang(QStringLiteral("language other 0.1"));
    QVERIFY(!wrongLanguage.success());
    QVERIFY(diagnosticsText(wrongLanguage.diagnostics)
                .contains(QStringLiteral("BP0101")));

    const CompileResult wrongVersion =
        compileBrecoLang(QStringLiteral("language breco 0.2"));
    QVERIFY(!wrongVersion.success());
    QVERIFY(diagnosticsText(wrongVersion.diagnostics)
                .contains(QStringLiteral("BP0102")));
}

void BrecoLangCompilerTests::anonymousRecordFieldsLowerToOrdinaryRecords() {
    const CompileResult compiled = compileBrecoLang(QString::fromUtf8(R"BRECO(
record TopRecord {
    enabled: u8
    SomeField: {
        X: u8
        Nested: { Y: u16le }
    }
    Empty: { }
    Optional: { Z: u8 } when enabled != 0
    External: { W: u8 } from data at 0 within bytes(1)
    Items: repeat 4 {
        Item: { A: u8 B: u8 }
    }
}
)BRECO"));
    QVERIFY2(compiled.success(),
             qPrintable(diagnosticsText(compiled.diagnostics)));
    QCOMPARE(compiled.program->entries.size(), 1);

    const RecordDesc* topRecord = nullptr;
    for (const RecordDesc& record : compiled.program->records) {
        if (compiled.program->symbol(record.name) == QStringLiteral("TopRecord")) {
            topRecord = &record;
            break;
        }
    }
    QVERIFY(topRecord != nullptr);
    QCOMPARE(compiled.program->entries.first().resultType, topRecord->type);

    const auto findField = [&](TypeId owner,
                               QStringView name) -> const FieldDesc* {
        const TypeDesc& type = compiled.program->types.at(owner);
        for (quint32 i = 0; i < type.fields.count; ++i) {
            const FieldDesc& field = compiled.program->fields.at(
                compiled.program->fieldRefs.at(type.fields.first + i));
            if (compiled.program->symbol(field.name) == name) {
                return &field;
            }
        }
        return nullptr;
    };
    const auto recordForType = [&](TypeId type) -> const RecordDesc* {
        for (const RecordDesc& record : compiled.program->records) {
            if (record.type == type) {
                return &record;
            }
        }
        return nullptr;
    };

    const FieldDesc* someField = findField(topRecord->type, u"SomeField");
    QVERIFY(someField != nullptr);
    QCOMPARE(compiled.program->types.at(someField->type).kind,
             TypeKind::Record);
    QCOMPARE(compiled.program->symbol(
                 compiled.program->types.at(someField->type).name),
             QStringLiteral("$anon_record_TopRecord.SomeField[%1]")
                 .arg(someField->type));
    QCOMPARE(compiled.program->statements.at(someField->statement).kind,
             StatementKind::Field);
    const RecordDesc* someRecord = recordForType(someField->type);
    QVERIFY(someRecord != nullptr);
    QCOMPARE(compiled.program->extents.at(someRecord->extent).exactBytes,
             std::optional<quint64>(3));

    const FieldDesc* nested = findField(someField->type, u"Nested");
    QVERIFY(nested != nullptr);
    QCOMPARE(compiled.program->types.at(nested->type).kind, TypeKind::Record);
    QVERIFY(nested->type != someField->type);
    QCOMPARE(compiled.program->symbol(
                 compiled.program->types.at(nested->type).name),
             QStringLiteral("$anon_record_TopRecord.SomeField.Nested[%1]")
                 .arg(nested->type));

    const FieldDesc* empty = findField(topRecord->type, u"Empty");
    QVERIFY(empty != nullptr);
    const RecordDesc* emptyRecord = recordForType(empty->type);
    QVERIFY(emptyRecord != nullptr);
    QCOMPARE(compiled.program->extents.at(emptyRecord->extent).exactBytes,
             std::optional<quint64>(0));

    const FieldDesc* optional = findField(topRecord->type, u"Optional");
    QVERIFY(optional != nullptr);
    QVERIFY(optional->optional);
    QCOMPARE(compiled.program->types.at(optional->type).kind,
             TypeKind::Optional);
    QCOMPARE(compiled.program->types.at(
                 compiled.program->types.at(optional->type).elementType)
                 .kind,
             TypeKind::Record);

    const FieldDesc* external = findField(topRecord->type, u"External");
    QVERIFY(external != nullptr);
    const Statement& externalStatement =
        compiled.program->statements.at(external->statement);
    QVERIFY(externalStatement.input != kInvalidId);
    QVERIFY(externalStatement.expression != kInvalidId);
    QVERIFY(externalStatement.secondaryExpression != kInvalidId);

    const Statement* items = nullptr;
    for (const Statement& statement : compiled.program->statements) {
        if (compiled.program->symbol(statement.name) == QStringLiteral("Items")) {
            items = &statement;
            break;
        }
    }
    QVERIFY(items != nullptr);
    QCOMPARE(items->loopScanPlan, LoopScanPlan::BatchAdvance);

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
             qPrintable(diagnosticsText(coexistence.diagnostics)));
}

void BrecoLangCompilerTests::resolvedProgramContainsTransactionalAndOutformPlans() {
    const CompileResult result = compileBrecoLang(kCompleteProgram);
    QVERIFY2(result.success(), qPrintable(diagnosticsText(result.diagnostics)));
    QSet<StatementKind> kinds;
    for (const Statement& statement : result.program->statements) {
        kinds.insert(statement.kind);
    }
    QVERIFY(kinds.contains(StatementKind::Identify));
    QVERIFY(kinds.contains(StatementKind::Commit));
    QVERIFY(kinds.contains(StatementKind::Require));
    QVERIFY(kinds.contains(StatementKind::Check));
    QVERIFY(kinds.contains(StatementKind::Match));
    QVERIFY(kinds.contains(StatementKind::BitfieldField));
    QVERIFY(kinds.contains(StatementKind::Region));
    QVERIFY(kinds.contains(StatementKind::Repeat));
    QVERIFY(kinds.contains(StatementKind::While));
    QVERIFY(kinds.contains(StatementKind::Many));
    QVERIFY(kinds.contains(StatementKind::Select));
    QVERIFY(kinds.contains(StatementKind::OneOf));
    QVERIFY(kinds.contains(StatementKind::Recover));
    QVERIFY(kinds.contains(StatementKind::Preserve));
    QVERIFY(kinds.contains(StatementKind::Raw));
    QVERIFY(kinds.contains(StatementKind::Emit));
    QVERIFY(kinds.contains(StatementKind::Let));
    QVERIFY(kinds.contains(StatementKind::If));
    QVERIFY(kinds.contains(StatementKind::For));

    bool hasInputMetadata = false;
    for (const Expression& expression : result.program->expressions) {
        if (expression.kind == ExpressionKind::MetadataMember &&
            result.program->symbol(expression.symbol) == QStringLiteral("input")) {
            hasInputMetadata = true;
            break;
        }
    }
    QVERIFY(hasInputMetadata);
    QCOMPARE(result.program->outforms.at(0).mode, OutformMode::Text);
    QCOMPARE(result.program->outforms.at(1).mode, OutformMode::Binary);
    QVERIFY(!result.program->bytePatterns.isEmpty());
}

void BrecoLangCompilerTests::extentAndEffectAnalysisPropagatesUsefulFacts() {
    const CompileResult result = compileBrecoLang(kCompleteProgram);
    QVERIFY2(result.success(), qPrintable(diagnosticsText(result.diagnostics)));

    const RecordDesc* fixed = nullptr;
    const RecordDesc* split = nullptr;
    for (const RecordDesc& record : result.program->records) {
        if (result.program->symbol(record.name) == QStringLiteral("FixedPrefix")) {
            fixed = &record;
        } else if (result.program->symbol(record.name) ==
                   QStringLiteral("SplitRecording")) {
            split = &record;
        }
    }
    QVERIFY(fixed != nullptr);
    const ExtentSummary& fixedExtent = result.program->extents.at(fixed->extent);
    QVERIFY(fixedExtent.exactBytes.has_value());
    QCOMPARE(*fixedExtent.exactBytes, 5ULL);
    QCOMPARE(fixedExtent.fixedPrefixBytes, 5ULL);
    QCOMPARE(fixedExtent.parentAdvance, ParentAdvance::Contiguous);

    QVERIFY(split != nullptr);
    const ExtentSummary& splitExtent = result.program->extents.at(split->extent);
    QVERIFY(splitExtent.requiresRandomAccess);
    QCOMPARE(splitExtent.parentAdvance, ParentAdvance::MultiInput);
    QVERIFY(!splitExtent.exactBytes.has_value());
}

void BrecoLangCompilerTests::loopScanPlanIsConservativeAndKeepsItemExtent() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Run from data {
    fast: repeat 100 { value: u8 }
    checked: repeat 2 { value: u8 check value != 0 else "zero" }
    controlled: repeat 2 { value: u8 break when iteration == 0 }
}
)BRECO");
    const CompileResult result = compileBrecoLang(source);
    QVERIFY2(result.success(), qPrintable(diagnosticsText(result.diagnostics)));
    const Statement* fast = nullptr;
    const Statement* checked = nullptr;
    const Statement* controlled = nullptr;
    for (const Statement& statement : result.program->statements) {
        const QString name = result.program->symbol(statement.name);
        if (name == QStringLiteral("fast")) fast = &statement;
        if (name == QStringLiteral("checked")) checked = &statement;
        if (name == QStringLiteral("controlled")) controlled = &statement;
    }
    QVERIFY(fast != nullptr);
    QCOMPARE(fast->loopScanPlan, LoopScanPlan::BatchAdvance);
    QVERIFY(fast->itemExtent != kInvalidId);
    QCOMPARE(*result.program->extents.at(fast->itemExtent).exactBytes, 1ULL);
    QVERIFY(fast->staticItemTemplate != kInvalidId);
    QVERIFY(checked != nullptr);
    QCOMPARE(checked->loopScanPlan, LoopScanPlan::ExecuteItems);
    QVERIFY(controlled != nullptr);
    QCOMPARE(controlled->loopScanPlan, LoopScanPlan::ExecuteItems);
}

void BrecoLangCompilerTests::referencesLowerToExplicitIrAndKeepStructuralBatchPlan() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
record Target(id: u32) { value: u8 }
entry Run from data {
    items: repeat 2000000 {
        carrier: u32le
        target: ref Target(carrier)
            from data at root_offset(carrier + 4)
            within bytes(1)
            key carrier
            follow
            cover region
            rewrite { carrier = target.@relocated_key; }
    }
}
)BRECO");
    const CompileResult result = compileBrecoLang(source);
    QVERIFY2(result.success(), qPrintable(diagnosticsText(result.diagnostics)));
    QCOMPARE(result.program->references.size(), 1);
    QCOMPARE(result.program->referenceRewrites.size(), 1);
    const ReferenceDesc& reference = result.program->references.first();
    QCOMPARE(reference.address.base, AddressBaseKind::EntryRoot);
    QCOMPARE(reference.strength, ReferenceStrength::Follow);
    QCOMPARE(reference.coverage, ReferenceCoverage::WholeRegion);
    QCOMPARE(reference.keyExpressions.count, 1U);
    QCOMPARE(reference.rewriteRules.count, 1U);

    const Statement* loop = nullptr;
    const Statement* referenceStatement = nullptr;
    for (const Statement& statement : result.program->statements) {
        if (result.program->symbol(statement.name) == QStringLiteral("items")) {
            loop = &statement;
        } else if (result.program->symbol(statement.name) ==
                   QStringLiteral("target")) {
            referenceStatement = &statement;
        }
    }
    QVERIFY(loop != nullptr);
    QCOMPARE(loop->loopScanPlan, LoopScanPlan::BatchAdvance);
    QCOMPARE(loop->referenceEffectScanPlan,
             ReferenceEffectScanPlan::ExecuteItems);
    QVERIFY(referenceStatement != nullptr);
    QCOMPARE(referenceStatement->kind, StatementKind::Reference);
    const ExtentSummary& extent =
        result.program->extents.at(referenceStatement->extent);
    QCOMPARE(extent.exactBytes, std::optional<quint64>(0));
    QVERIFY(extent.hasReferenceEffects);

    const CompileResult invalid = compileBrecoLang(QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
record T { value: u8 }
entry Bad from data {
    pointer: ref T from data at 3 within bytes(1)
}
)BRECO"));
    QVERIFY(!invalid.success());
    const QString messages = diagnosticsText(invalid.diagnostics);
    QVERIFY(messages.contains(QStringLiteral("explicit address base")) ||
            messages.contains(QStringLiteral("input_offset")));
    QVERIFY(messages.contains(QStringLiteral("follow")));

    const CompileResult invalidRewrite =
        compileBrecoLang(QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
record T { value: u8 }
entry BadRewrite from data {
    computed carrier: u64 = 0
    pointer: ref T
        from data at input_offset(0)
        within bytes(1)
        weak
        rewrite { carrier = target.@relocated_key; }
}
)BRECO"));
    QVERIFY(!invalidRewrite.success());
    QVERIFY(diagnosticsText(invalidRewrite.diagnostics)
                .contains(QStringLiteral("source-backed field")));
}

void BrecoLangCompilerTests::parserAndResolverBatchIndependentDiagnostics() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs {
    input primary { default }
    input primary { default }
    input second { default }
}
limits {
    max_nodes 0
    unknown_limit 5
}
enum Broken: f64 {
    Same = 1
    Same = 2
}
record Bad {
    require missing_name > 0 else "missing"
    first: MissingType
    first: u8
    match true else "outside identify"
    stop: while true { break }
}
entry Run from absent {
    item: Bad
}
default entry DoesNotExist
outform BrokenBinary(root: Run) binary {
    emit "not bytes"
    if 7 { emit bytes [0] }
    for item in 9 { emit bytes [1] }
}
)BRECO");
    const CompileResult result = compileBrecoLang(source);
    QVERIFY(!result.success());
    QVERIFY(result.syntax != nullptr);
    QVERIFY2(result.diagnostics.size() >= 9,
             qPrintable(diagnosticsText(result.diagnostics)));
    QSet<QString> codes;
    for (const Diagnostic& diagnostic : result.diagnostics) {
        codes.insert(diagnostic.code);
    }
    QVERIFY(codes.contains(QStringLiteral("BR0200")));
    QVERIFY(codes.contains(QStringLiteral("BR0202")));
    QVERIFY(codes.contains(QStringLiteral("BR0211")));
    QVERIFY(codes.contains(QStringLiteral("BR0213")));
    QVERIFY(codes.contains(QStringLiteral("BR0300")));
    QVERIFY(codes.contains(QStringLiteral("BR0515")));
    QVERIFY(codes.contains(QStringLiteral("BR0540")));
}

void BrecoLangCompilerTests::parserRecoversToLaterTopLevelDeclarations() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
const = 7
enum Damaged u8 { A = }
record Good { value: u8 }
entry GoodEntry from data { good: Good }
outform GoodText(root: GoodEntry) text { emit "${root.good.value}" }
)BRECO");
    const ParseSyntaxResult parsed = parseBrecoLang(source);
    QVERIFY(parsed.diagnostics.size() >= 2);
    QVERIFY(std::any_of(parsed.syntax.records.cbegin(), parsed.syntax.records.cend(),
                        [](const SyntaxRecord& record) {
                            return record.name == QStringLiteral("Good");
                        }));
    QVERIFY(std::any_of(parsed.syntax.entries.cbegin(), parsed.syntax.entries.cend(),
                        [](const SyntaxEntry& entry) {
                            return entry.name == QStringLiteral("GoodEntry");
                        }));
    QVERIFY(std::any_of(parsed.syntax.outforms.cbegin(), parsed.syntax.outforms.cend(),
                        [](const SyntaxOutform& outform) {
                            return outform.name == QStringLiteral("GoodText");
                        }));
}

void BrecoLangCompilerTests::inlineSelectsMergeFieldsAndBuildAggregateMetadata() {
    const QString source = QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
record InlineChoice {
    tag: u8
    select tag {
        1 => { value: u16le left: u8 }
        default => { value: u32le }
    }
    computed has_left: bool = present(left)
    computed early_value_count: u64 = count(value)
    select {
        when tag > 0 => { value: u8 }
        else => { }
    }
    computed value_count: u64 = count(value)
    boxed: select tag {
        1 => { selected: u8 }
        default => { fallback: u8 }
    }
}
entry Main from data { root: InlineChoice }
outform Values(root: InlineChoice) text {
    for item in root.value { emit "${item}" }
}
)BRECO");
    const CompileResult result = compileBrecoLang(source);
    QVERIFY2(result.success(), qPrintable(diagnosticsText(result.diagnostics)));

    int inlineSelects = 0;
    int boxedSelects = 0;
    for (const Statement& statement : result.program->statements) {
        if (statement.kind != StatementKind::Select) {
            continue;
        }
        if (statement.inlineSelect) {
            ++inlineSelects;
            QVERIFY(statement.name == kInvalidId);
            QVERIFY(statement.selectCases.count >= 2);
        } else {
            ++boxedSelects;
            QVERIFY(statement.name != kInvalidId);
        }
    }
    QCOMPARE(inlineSelects, 2);
    QCOMPARE(boxedSelects, 1);
    QCOMPARE(result.program->aggregateFields.size(), 1);
    QVERIFY(std::any_of(result.program->selectYields.cbegin(),
                        result.program->selectYields.cend(),
                        [](const SelectYield& yield) {
                            return yield.aggregate != kInvalidId;
                        }));

    const CompileResult directType = compileBrecoLang(QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Main from data {
    tag: u8
    select tag { 1 => u8 default => { value: u8 } }
}
)BRECO"));
    QVERIFY(!directType.success());
    QVERIFY(diagnosticsText(directType.diagnostics).contains(
        QStringLiteral("bare select type arm"), Qt::CaseInsensitive));

    const CompileResult collision = compileBrecoLang(QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Main from data {
    value: u8
    select { when true => { value: u8 } else => { } }
}
)BRECO"));
    QVERIFY(!collision.success());
    QVERIFY(diagnosticsText(collision.diagnostics).contains(
        QStringLiteral("Duplicate field 'value'")));

    const CompileResult ambiguous = compileBrecoLang(QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Main from data {
    select { when true => { value: u8 } else => { } }
    computed invalid_scalar_use: u64 = value + 1
    select { when false => { value: u8 } else => { } }
}
)BRECO"));
    QVERIFY(!ambiguous.success());
    QVERIFY(diagnosticsText(ambiguous.diagnostics).contains(
        QStringLiteral("numeric operands")));

    const CompileResult identifyAmbiguous =
        compileBrecoLang(QString::fromUtf8(R"BRECO(
language breco 0.1
inputs { input data { default } }
entry Main from data {
    select { when true => { value: u8 } else => { } }
    computed invalid_scalar_use: u64 = value + 1
    identify {
        select { when false => { value: u8 } else => { } }
        match true else "unreachable"
    }
}
)BRECO"));
    QVERIFY(!identifyAmbiguous.success());
    QVERIFY(diagnosticsText(identifyAmbiguous.diagnostics).contains(
        QStringLiteral("numeric operands")));
}

void BrecoLangCompilerTests::shippedExamplesCompile() {
    int count = 0;
    const QStringList roots{
        QStringLiteral(BRECO_SOURCE_DIR "/examples"),
        QStringLiteral(BRECO_SOURCE_DIR "/../HES-Breco/HES-BrecoLang"),
        QStringLiteral(BRECO_SOURCE_DIR "/../HES-Breco/IQDW_BrecoLang")};
    for (const QString& root : roots) {
        QDirIterator files(root, {QStringLiteral("*.breco")}, QDir::Files,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            QFile file(path);
            QVERIFY2(file.open(QIODevice::ReadOnly),
                     qPrintable(file.errorString()));
            const CompileResult result =
                compileBrecoLang(QString::fromUtf8(file.readAll()), path);
            QVERIFY2(result.success(),
                     qPrintable(path + QStringLiteral(": ") +
                                (result.diagnostics.isEmpty()
                                     ? QStringLiteral("unknown compiler failure")
                                     : result.diagnostics.first().message)));
            ++count;
        }
    }
    QVERIFY(count >= 12);
}

}  // namespace

QTEST_APPLESS_MAIN(BrecoLangCompilerTests)

#include "brecolang_compiler_tests.moc"

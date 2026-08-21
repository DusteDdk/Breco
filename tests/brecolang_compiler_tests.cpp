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
        data_blocks: select frame_type {
            FrameType.SplitIQ16 => {
                blocks: repeat data_header.datablock_count {
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
    void resolvedProgramContainsTransactionalAndOutformPlans();
    void extentAndEffectAnalysisPropagatesUsefulFacts();
    void parserAndResolverBatchIndependentDiagnostics();
    void parserRecoversToLaterTopLevelDeclarations();
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

void BrecoLangCompilerTests::shippedExamplesCompile() {
    QDirIterator files(QStringLiteral(BRECO_SOURCE_DIR "/examples"),
                       {QStringLiteral("*.breco")}, QDir::Files,
                       QDirIterator::Subdirectories);
    int count = 0;
    while (files.hasNext()) {
        const QString path = files.next();
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        const CompileResult result =
            compileBrecoLang(QString::fromUtf8(file.readAll()), path);
        QVERIFY2(result.success(),
                 qPrintable(path + QStringLiteral(": ") +
                            (result.diagnostics.isEmpty()
                                 ? QStringLiteral("unknown compiler failure")
                                 : result.diagnostics.first().message)));
        ++count;
    }
    QVERIFY(count >= 5);
}

}  // namespace

QTEST_APPLESS_MAIN(BrecoLangCompilerTests)

#include "brecolang_compiler_tests.moc"

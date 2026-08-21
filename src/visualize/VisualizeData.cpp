#include "visualize/VisualizeData.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "brecolang/compiler/Compiler.h"
#include "brecolang/runtime/ByteSource.h"
#include "brecolang/runtime/DecodeTarget.h"
#include "brecolang/runtime/Interpreter.h"

namespace breco {

namespace {

struct PreparedProgram {
    QString source;
    std::shared_ptr<const lang::BrecoProgram> program;
    QString error;
};

QString firstCompilerError(const lang::CompileResult& result) {
    for (const lang::Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.severity == lang::DiagnosticSeverity::Error) {
            return diagnostic.message;
        }
    }
    return QStringLiteral("BrecoLang compilation failed");
}

QString firstRuntimeError(const lang::DecodeResult& result) {
    for (const lang::RuntimeDiagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.severity == lang::DiagnosticSeverity::Error) {
            return diagnostic.message;
        }
    }
    return QStringLiteral("BrecoLang decode failed");
}

QString effectiveSource(QStringView source) {
    return source.trimmed().isEmpty() ? builtinVisualizeProgramSource()
                                     : source.toString();
}

PreparedProgram prepareProgram(QString source) {
    lang::CompileResult compiled = lang::compileBrecoLang(source);
    if (!compiled.success()) {
        return {std::move(source), {}, firstCompilerError(compiled)};
    }
    if (!compiled.program->inputs.isEmpty()) {
        return {std::move(source), std::move(compiled.program), {}};
    }

    source += QStringLiteral(
        "\ninputs { input __visualize_data \"Visualize\" { default } }\n");
    compiled = lang::compileBrecoLang(source);
    if (!compiled.success()) {
        return {std::move(source), {}, firstCompilerError(compiled)};
    }
    return {std::move(source), std::move(compiled.program), {}};
}

const lang::RecordDesc* findRecord(const lang::BrecoProgram& program,
                                   QStringView name) {
    for (const lang::RecordDesc& record : program.records) {
        if (program.symbol(record.name) == name) {
            return &record;
        }
    }
    return nullptr;
}

lang::SymbolId symbolId(const lang::BrecoProgram& program, QStringView name) {
    for (lang::SymbolId id = 0;
         id < static_cast<lang::SymbolId>(program.symbols.size()); ++id) {
        if (program.symbols.at(id) == name) {
            return id;
        }
    }
    return lang::kInvalidId;
}

QString primaryInputName(const lang::BrecoProgram& program) {
    lang::InputId selected = lang::kInvalidId;
    for (lang::InputId id = 0;
         id < static_cast<lang::InputId>(program.inputs.size()); ++id) {
        if (program.inputs.at(id).isDefault) {
            selected = id;
            break;
        }
    }
    if (selected == lang::kInvalidId && program.inputs.size() == 1) {
        selected = 0;
    }
    return selected == lang::kInvalidId
               ? QString()
               : program.symbol(program.inputs.at(selected).name);
}

QString uniqueEntryName(const lang::BrecoProgram& program) {
    QString candidate = QStringLiteral("__BrecoVisualizeItems");
    int suffix = 2;
    while (symbolId(program, candidate) != lang::kInvalidId) {
        candidate = QStringLiteral("__BrecoVisualizeItems%1").arg(suffix++);
    }
    return candidate;
}

PreparedProgram prepareManyProgram(const PreparedProgram& base,
                                   QStringView recordName,
                                   QString* entryName) {
    if (!base.program) {
        return base;
    }
    const QString inputName = primaryInputName(*base.program);
    if (inputName.isEmpty()) {
        return {base.source, {},
                QStringLiteral(
                    "The Visualize program requires one input or a default input")};
    }
    *entryName = uniqueEntryName(*base.program);
    QString source = base.source;
    source +=
        QStringLiteral("\nentry %1 from %2 { items: many %3 }\n")
            .arg(*entryName, inputName, recordName.toString());
    lang::CompileResult compiled = lang::compileBrecoLang(source);
    if (!compiled.success()) {
        return {std::move(source), {}, firstCompilerError(compiled)};
    }
    return {std::move(source), std::move(compiled.program), {}};
}

const lang::DecodedValue* valueAt(const lang::DecodedTree& tree,
                                  lang::DecodedValueId id) {
    return id < static_cast<lang::DecodedValueId>(tree.values.size())
               ? &tree.values.at(id)
               : nullptr;
}

const lang::DecodedFieldValue* fieldValue(
    const lang::BrecoProgram& program, const lang::DecodedTree& tree,
    lang::DecodedValueId object, QStringView name) {
    const lang::SymbolId symbol = symbolId(program, name);
    return symbol == lang::kInvalidId ? nullptr
                                      : tree.findField(object, symbol);
}

std::optional<double> numericValue(const lang::DecodedTree& tree,
                                   lang::DecodedValueId id) {
    const lang::DecodedValue* value = valueAt(tree, id);
    if (value == nullptr) {
        return std::nullopt;
    }
    switch (value->kind) {
        case lang::DecodedValueKind::UnsignedInteger:
            return static_cast<double>(value->unsignedValue);
        case lang::DecodedValueKind::SignedInteger:
            return static_cast<double>(value->signedValue);
        case lang::DecodedValueKind::FloatingPoint:
            return value->floatingValue;
        case lang::DecodedValueKind::Boolean:
            return value->booleanValue ? 1.0 : 0.0;
        default:
            return std::nullopt;
    }
}

std::optional<double> fieldNumber(const lang::BrecoProgram& program,
                                  const lang::DecodedTree& tree,
                                  lang::DecodedValueId object,
                                  QStringView name) {
    const lang::DecodedFieldValue* field =
        fieldValue(program, tree, object, name);
    return field == nullptr ? std::nullopt : numericValue(tree, field->value);
}

std::optional<QString> fieldString(const lang::BrecoProgram& program,
                                   const lang::DecodedTree& tree,
                                   lang::DecodedValueId object,
                                   QStringView name) {
    const lang::DecodedFieldValue* field =
        fieldValue(program, tree, object, name);
    const lang::DecodedValue* value =
        field == nullptr ? nullptr : valueAt(tree, field->value);
    if (value == nullptr || value->kind != lang::DecodedValueKind::String ||
        value->payload >= static_cast<quint32>(tree.valueStrings.size())) {
        return std::nullopt;
    }
    return tree.valueStrings.at(value->payload);
}

std::optional<lang::DecodedValueId> fieldObject(
    const lang::BrecoProgram& program, const lang::DecodedTree& tree,
    lang::DecodedValueId object, QStringView name) {
    const lang::DecodedFieldValue* field =
        fieldValue(program, tree, object, name);
    const lang::DecodedValue* value =
        field == nullptr ? nullptr : valueAt(tree, field->value);
    if (value == nullptr || value->kind != lang::DecodedValueKind::Object) {
        return std::nullopt;
    }
    return field->value;
}

CartesianStyle styleFromString(QString style, CartesianStyle fallback) {
    style = style.trimmed().toLower();
    if (style == QStringLiteral("dot")) {
        return CartesianStyle::Dot;
    }
    if (style == QStringLiteral("area")) {
        return CartesianStyle::Area;
    }
    if (style == QStringLiteral("skin")) {
        return CartesianStyle::Skin;
    }
    if (style == QStringLiteral("bar")) {
        return CartesianStyle::Bar;
    }
    if (style == QStringLiteral("line")) {
        return CartesianStyle::Line;
    }
    return fallback;
}

lang::DecodeResult decodeRecord(
    const std::shared_ptr<const lang::BrecoProgram>& program,
    QStringView recordName, const QByteArray& bytes, quint64 baseOffset,
    QString* error) {
    const lang::ResolvedDecodeTarget target = lang::resolveDecodeTarget(
        program, lang::DecodeTargetKind::Record, recordName, error);
    if (!target.isValid()) {
        return {};
    }
    lang::DecodeRequest request;
    request.program = target.program;
    request.entryName = target.entryName;
    request.mode = lang::DecodeMode::Tree;
    request.inputs.resize(target.program->inputs.size());
    request.inputs[target.primaryInput] =
        std::make_shared<lang::BorrowedWindowSource>(
            bytes, QStringLiteral("visualize-window"), baseOffset);
    lang::DecodeResult result = lang::decodeBrecoProgram(request);
    if ((!result.success() || !result.tree) && error != nullptr) {
        *error = firstRuntimeError(result);
    }
    return result;
}

lang::DecodeResult decodeEntry(
    const std::shared_ptr<const lang::BrecoProgram>& program,
    QStringView entryName, const QByteArray& bytes, quint64 baseOffset,
    QString* error, quint64 startOffset = 0) {
    const lang::ResolvedDecodeTarget target = lang::resolveDecodeTarget(
        program, lang::DecodeTargetKind::Entry, entryName, error);
    if (!target.isValid()) {
        return {};
    }
    lang::DecodeRequest request;
    request.program = target.program;
    request.entryName = target.entryName;
    request.mode = lang::DecodeMode::Tree;
    request.startOffset = startOffset;
    request.inputs.resize(target.program->inputs.size());
    request.inputs[target.primaryInput] =
        std::make_shared<lang::BorrowedWindowSource>(
            bytes, QStringLiteral("visualize-window"), baseOffset);
    lang::DecodeResult result = lang::decodeBrecoProgram(request);
    if ((!result.success() || !result.tree) && error != nullptr) {
        *error = firstRuntimeError(result);
    }
    return result;
}

void applyConfigurationObject(const lang::BrecoProgram& program,
                              const lang::DecodedTree& tree,
                              lang::DecodedValueId object,
                              VisualizeConfiguration* config) {
    if (const auto bytes =
            fieldNumber(program, tree, object, u"NumBytesOnNoSelection");
        bytes.has_value() && std::isfinite(*bytes) && *bytes >= 0.0) {
        config->numBytesOnNoSelection =
            static_cast<quint64>(qBound(
                0.0, *bytes, static_cast<double>(kMaximumVisualizationBytes)));
    }
    if (const auto style = fieldString(program, tree, object, u"Style");
        style.has_value()) {
        config->style = styleFromString(*style, config->style);
    }
    if (const auto chart = fieldObject(program, tree, object, u"Chart");
        chart.has_value()) {
        if (const auto ticks =
                fieldNumber(program, tree, *chart, u"tickDistance");
            ticks.has_value() && std::isfinite(*ticks) && *ticks >= 0.0) {
            config->tickDistance = *ticks;
        }
    }
}

void applyStyleFallbackForMode(VisualizationMode mode,
                               VisualizeConfiguration* config) {
    if (config->style != CartesianStyle::Bar ||
        mode == VisualizationMode::Cartesian2D) {
        return;
    }
    config->style = mode == VisualizationMode::Cartesian3D
                        ? CartesianStyle::Dot
                        : CartesianStyle::Line;
}

QVector<lang::DecodedValueId> manyItems(
    const lang::BrecoProgram& program, const lang::DecodedTree& tree,
    lang::DecodedValueId root) {
    const lang::DecodedFieldValue* items =
        fieldValue(program, tree, root, u"items");
    const lang::DecodedValue* sequence =
        items == nullptr ? nullptr : valueAt(tree, items->value);
    if (sequence == nullptr ||
        (sequence->kind != lang::DecodedValueKind::Sequence &&
         sequence->kind != lang::DecodedValueKind::Aggregate) ||
        sequence->elements.first >
            static_cast<quint32>(tree.valueRefs.size()) ||
        sequence->elements.count >
            static_cast<quint32>(tree.valueRefs.size()) -
                sequence->elements.first) {
        return {};
    }
    QVector<lang::DecodedValueId> result;
    result.reserve(sequence->elements.count);
    for (quint32 i = 0; i < sequence->elements.count; ++i) {
        result.push_back(tree.valueRefs.at(sequence->elements.first + i));
    }
    return result;
}

const lang::FieldDesc* declaredField(const lang::BrecoProgram& program,
                                     lang::TypeId recordType,
                                     QStringView name) {
    if (recordType >= static_cast<lang::TypeId>(program.types.size())) {
        return nullptr;
    }
    const lang::TypeDesc& type = program.types.at(recordType);
    if (type.fields.first > static_cast<quint32>(program.fieldRefs.size()) ||
        type.fields.count >
            static_cast<quint32>(program.fieldRefs.size()) -
                type.fields.first) {
        return nullptr;
    }
    for (quint32 i = 0; i < type.fields.count; ++i) {
        const quint32 fieldId =
            program.fieldRefs.at(type.fields.first + i);
        if (fieldId >= static_cast<quint32>(program.fields.size())) {
            continue;
        }
        const lang::FieldDesc& field =
            program.fields.at(fieldId);
        if (program.symbol(field.name) == name) {
            return &field;
        }
    }
    return nullptr;
}

int bitmapBitsPerPixel(const lang::BrecoProgram& program,
                       const lang::RecordDesc& bitmap) {
    const lang::FieldDesc* color =
        declaredField(program, bitmap.type, u"Color");
    if (color == nullptr) {
        return 1;
    }
    if (declaredField(program, color->type, u"a") != nullptr) {
        return 32;
    }
    if (declaredField(program, color->type, u"b") != nullptr) {
        return 24;
    }
    if (declaredField(program, color->type, u"g") != nullptr) {
        return 16;
    }
    return 8;
}

QColor decodedColor(const lang::BrecoProgram& program,
                    const lang::DecodedTree& tree,
                    lang::DecodedValueId object, int bitsPerPixel) {
    const auto color = fieldObject(program, tree, object, u"Color");
    if (!color.has_value()) {
        return QColor(Qt::white);
    }
    const int r = qBound(0, static_cast<int>(
                                fieldNumber(program, tree, *color, u"r")
                                    .value_or(0.0)),
                         255);
    if (bitsPerPixel == 8) {
        return QColor(r, 0, 0);
    }
    const int g = qBound(0, static_cast<int>(
                                fieldNumber(program, tree, *color, u"g")
                                    .value_or(0.0)),
                         255);
    const int b = qBound(0, static_cast<int>(
                                fieldNumber(program, tree, *color, u"b")
                                    .value_or(0.0)),
                         255);
    const int a = qBound(0, static_cast<int>(
                                fieldNumber(program, tree, *color, u"a")
                                    .value_or(255.0)),
                         255);
    return QColor(r, g, b, bitsPerPixel == 32 ? a : 255);
}

qint64 coordinateValue(double value) {
    if (!std::isfinite(value)) {
        return 0;
    }
    if (value <= static_cast<double>(std::numeric_limits<qint64>::min())) {
        return std::numeric_limits<qint64>::min();
    }
    if (value >= static_cast<double>(std::numeric_limits<qint64>::max())) {
        return std::numeric_limits<qint64>::max();
    }
    return static_cast<qint64>(value);
}

void appendBitmapItems(const lang::BrecoProgram& program,
                       const lang::DecodedTree& tree,
                       const QVector<lang::DecodedValueId>& items,
                       VisualizationDecodeResult* result) {
    result->bitmapPixels.reserve(result->bitmapPixels.size() + items.size());
    for (const lang::DecodedValueId item : items) {
        VisualizeBitmapPixel pixel;
        pixel.color =
            decodedColor(program, tree, item, result->bitmapBitsPerPixel);
        if (result->bitmapHasPlot) {
            const auto plot = fieldObject(program, tree, item, u"Plot");
            if (!plot.has_value()) {
                continue;
            }
            pixel.x = coordinateValue(
                fieldNumber(program, tree, *plot, u"x").value_or(0.0));
            pixel.y = coordinateValue(
                fieldNumber(program, tree, *plot, u"y").value_or(0.0));
        }
        result->bitmapPixels.push_back(pixel);
    }
}

void appendCartesianItems(const lang::BrecoProgram& program,
                          const lang::DecodedTree& tree,
                          const QVector<lang::DecodedValueId>& items,
                          VisualizationMode mode, double* nextImplicitX,
                          VisualizationDecodeResult* result) {
    result->points.reserve(result->points.size() + items.size());
    for (const lang::DecodedValueId item : items) {
        const auto points = fieldObject(program, tree, item, u"Points");
        if (!points.has_value()) {
            continue;
        }
        const auto x = fieldNumber(program, tree, *points, u"x");
        const auto y = fieldNumber(program, tree, *points, u"y");
        const auto z = fieldNumber(program, tree, *points, u"z");
        if (!y.has_value() ||
            (mode == VisualizationMode::Cartesian3D &&
             (!x.has_value() || !z.has_value()))) {
            continue;
        }
        VisualizePoint point;
        point.x = x.value_or(*nextImplicitX);
        point.y = *y;
        point.z = z.value_or(0.0);
        point.color = decodedColor(program, tree, item, 32);
        if (!fieldObject(program, tree, item, u"Color").has_value()) {
            point.color = QColor(30, 144, 255);
        }
        result->points.push_back(point);
        *nextImplicitX = point.x + 1.0;
    }
}

}  // namespace

QString builtinVisualizeProgramSource() {
    return QString::fromUtf8(R"breco(// Default values and fallbacks, overwritten when a mode record contains the
// same computed field.
record VisCfg {
    // Zero reads the entire file from offset zero when there is no selection.
    computed NumBytesOnNoSelection: u32 = 1024
    // Valid styles: dot, line, area, skin, and Cart2D-only bar.
    // Skin draws one vertex as a dot, two as a line, then connects every new
    // vertex to the previous two to form a triangle strip.
    computed Style: string = "line"
}

record Cart2D {
    // Optional. Without Chart no ticks are drawn. A positive tickDistance
    // draws 1x4 px ticks on both axes at this data-unit interval.
    Chart: {
        computed tickDistance: u32 = 8
    }
    // x is optional in user programs; when absent it increases once per y.
    Points: {
        x: u8
        y: u8
    }
    // An optional Color record may contain r, g, b, and a fields.
}

record Cart3D {
    computed Style: string = "dot"
    Points: {
        x: u8
        y: u8
        z: u8
    }
    // An optional Chart record supplies tickDistance. An optional Color
    // record may contain r, g, b, and a fields.
}

record Bitmap {
    // Optional. Without Color the bitmap is 1 bpp. Declaring r, g, b, and a
    // progressively selects 8, 16, 24, and 32 bpp.
    Color: {
        r: u8
        g: u8
        b: u8
        a: u8
    }
    // Optional. Without Plot, pixels are packed sequentially using the image
    // rectangle's row/column packing. With Plot, each record is placed at x,y.
    Plot: {
        x: u8
        y: u8
    }
}
)breco");
}

VisualizationWindow resolveVisualizationWindow(
    std::optional<QPair<quint64, quint64>> selection,
    quint64 fallbackOffset, quint64 fileSize, quint64 maximumBytes,
    quint64 defaultBytes) {
    VisualizationWindow result;
    if (fileSize == 0 || maximumBytes == 0) {
        return result;
    }

    quint64 start = qMin(fallbackOffset, fileSize - 1);
    quint64 requested = 0;
    bool defaultWindow = true;
    if (selection.has_value()) {
        const quint64 first = qMin(selection->first, selection->second);
        const quint64 second = qMax(selection->first, selection->second);
        start = qMin(first, fileSize - 1);
        if (second > first + 1) {
            requested = qMin(second, fileSize) - start;
            defaultWindow = false;
        }
    }

    const quint64 remaining = fileSize - start;
    if (defaultWindow) {
        if (defaultBytes == 0) {
            result.start = 0;
            result.length = fileSize;
            return result;
        }
        requested = qMin(defaultBytes, remaining);
    }
    result.start = start;
    result.length = std::min({requested, remaining, maximumBytes});
    result.truncated = !defaultWindow && requested > result.length;
    return result;
}

VisualizationConfigurationResult readVisualizationConfiguration(
    QStringView source) {
    const PreparedProgram user = prepareProgram(effectiveSource(source));
    if (!user.program) {
        return {{}, user.error};
    }

    PreparedProgram selected = user;
    if (findRecord(*user.program, u"VisCfg") == nullptr) {
        selected = prepareProgram(builtinVisualizeProgramSource());
    }
    if (!selected.program) {
        return {{}, selected.error};
    }

    QString error;
    const lang::DecodeResult decoded =
        decodeRecord(selected.program, u"VisCfg", {}, 0, &error);
    if (!decoded.success() || !decoded.tree) {
        return {{}, error};
    }
    VisualizeConfiguration config;
    applyConfigurationObject(*selected.program, *decoded.tree,
                             decoded.rootValue, &config);
    return {config, {}};
}

VisualizationDecodeResult decodeVisualization(
    QStringView source, const QByteArray& bytes, quint64 baseOffset,
    VisualizationMode mode) {
    VisualizationDecodeResult result;
    const VisualizationConfigurationResult global =
        readVisualizationConfiguration(source);
    if (!global.success()) {
        result.error = global.error;
        return result;
    }
    result.config = global.config;

    PreparedProgram selected = prepareProgram(effectiveSource(source));
    if (!selected.program) {
        result.error = selected.error;
        return result;
    }

    const QString recordName =
        mode == VisualizationMode::Cartesian2D
            ? QStringLiteral("Cart2D")
            : (mode == VisualizationMode::Cartesian3D
                   ? QStringLiteral("Cart3D")
                   : QStringLiteral("Bitmap"));
    if (findRecord(*selected.program, recordName) == nullptr) {
        selected = prepareProgram(builtinVisualizeProgramSource());
        result.usedBuiltinRecord = true;
    }
    if (!selected.program) {
        result.error = selected.error;
        return result;
    }

    const lang::RecordDesc* selectedRecord =
        findRecord(*selected.program, recordName);
    if (selectedRecord == nullptr) {
        result.error =
            QStringLiteral("Missing Visualize record '%1'").arg(recordName);
        return result;
    }

    if (mode == VisualizationMode::Bitmap) {
        result.bitmapBitsPerPixel =
            bitmapBitsPerPixel(*selected.program, *selectedRecord);
        result.bitmapHasPlot =
            declaredField(*selected.program, selectedRecord->type, u"Plot") !=
            nullptr;
        if (!result.bitmapHasPlot && result.bitmapBitsPerPixel == 1) {
            QString configurationError;
            const lang::DecodeResult configuration = decodeRecord(
                selected.program, u"Bitmap", bytes, baseOffset,
                &configurationError);
            if (configuration.success() && configuration.tree) {
                applyConfigurationObject(
                    *selected.program, *configuration.tree,
                    configuration.rootValue, &result.config);
            }
            result.bitmapPackedBits = bytes;
            applyStyleFallbackForMode(mode, &result.config);
            return result;
        }
    }

    QString entryName;
    const PreparedProgram many =
        prepareManyProgram(selected, recordName, &entryName);
    if (!many.program) {
        result.error = many.error;
        return result;
    }
    constexpr qsizetype kDecodeChunkBytes = 512 * 1024;
    qsizetype position = 0;
    double nextImplicitX = 0.0;
    bool appliedModeConfiguration = false;
    while (position < bytes.size()) {
        const qsizetype remaining = bytes.size() - position;
        qsizetype chunkBytes = qMin(kDecodeChunkBytes, remaining);
        lang::DecodeResult decoded;
        QVector<lang::DecodedValueId> items;
        QString error;

        for (;;) {
            const QByteArray chunk = QByteArray::fromRawData(
                bytes.constData(), position + chunkBytes);
            error.clear();
            decoded = decodeEntry(many.program, entryName, chunk,
                                  baseOffset, &error,
                                  static_cast<quint64>(position));
            if (decoded.success() && decoded.tree) {
                items = manyItems(*many.program, *decoded.tree,
                                  decoded.rootValue);
                if (!items.isEmpty() || chunkBytes == remaining) {
                    break;
                }
            }
            if (chunkBytes == remaining) {
                result.error =
                    error.isEmpty()
                        ? QStringLiteral(
                              "%1 consumed no complete records at offset 0x%2")
                              .arg(recordName)
                              .arg(baseOffset + position, 0, 16)
                        : error;
                return result;
            }
            chunkBytes = qMin(remaining, chunkBytes * 2);
        }

        if (!appliedModeConfiguration && !items.isEmpty()) {
            applyConfigurationObject(*many.program, *decoded.tree,
                                     items.first(), &result.config);
            appliedModeConfiguration = true;
        }
        if (mode == VisualizationMode::Bitmap) {
            appendBitmapItems(*many.program, *decoded.tree, items, &result);
        } else {
            appendCartesianItems(*many.program, *decoded.tree, items, mode,
                                 &nextImplicitX, &result);
        }

        const quint64 endOffset = decoded.endOffset;
        const quint64 position64 = static_cast<quint64>(position);
        const quint64 consumed64 =
            endOffset >= position64 ? endOffset - position64 : 0;
        if (consumed64 == 0 ||
            consumed64 > static_cast<quint64>(chunkBytes)) {
            if (chunkBytes == remaining &&
                (!result.points.isEmpty() ||
                 !result.bitmapPixels.isEmpty())) {
                break;
            }
            result.error =
                QStringLiteral("%1 made no progress at offset 0x%2")
                    .arg(recordName)
                    .arg(baseOffset + position, 0, 16);
            return result;
        }
        position += static_cast<qsizetype>(consumed64);
    }

    if (mode == VisualizationMode::Bitmap) {
        applyStyleFallbackForMode(mode, &result.config);
        return result;
    }
    if (result.points.isEmpty()) {
        result.error =
            mode == VisualizationMode::Cartesian3D
                ? QStringLiteral(
                      "Cart3D yielded no Points with numeric x, y, and z")
                : QStringLiteral("Cart2D yielded no Points with numeric y");
    }
    applyStyleFallbackForMode(mode, &result.config);
    return result;
}

}  // namespace breco

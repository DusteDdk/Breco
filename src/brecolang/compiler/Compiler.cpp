#include "brecolang/compiler/Compiler.h"

#include "brecolang/compiler/Parser.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace breco::lang {

const QString& BrecoProgram::symbol(SymbolId id) const {
    static const QString empty;
    return id < static_cast<SymbolId>(symbols.size()) ? symbols.at(id) : empty;
}

namespace {

struct Binding {
    TypeId type = kInvalidId;
    quint32 slot = kInvalidId;
};

struct ResolveContext {
    QVector<QHash<QString, Binding>> scopes;
    quint32 nextSlot = 0;
    int loopDepth = 0;
    int identifyDepth = 0;
    bool hasIdentified = false;
    bool outform = false;
    OutformMode outformMode = OutformMode::Text;

    std::optional<Binding> find(QStringView name) const {
        for (qsizetype i = scopes.size(); i > 0; --i) {
            const auto found = scopes.at(i - 1).constFind(name.toString());
            if (found != scopes.at(i - 1).constEnd()) {
                return *found;
            }
        }
        return std::nullopt;
    }
};

struct ResolvedBlock {
    QVector<StatementId> statements;
    ExtentSummary extent;
};

bool isError(const Diagnostic& diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::Error;
}

bool checkedAdd(quint64 left, quint64 right, quint64* result) {
    if (left > std::numeric_limits<quint64>::max() - right) {
        return false;
    }
    *result = left + right;
    return true;
}

bool checkedMultiply(quint64 left, quint64 right, quint64* result) {
    if (left != 0 && right > std::numeric_limits<quint64>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

class Resolver {
public:
    Resolver(const SyntaxFile& syntax, QVector<Diagnostic> diagnostics)
        : m_syntax(syntax), m_diagnostics(std::move(diagnostics)),
          m_program(std::make_shared<BrecoProgram>()) {
        m_program->languageVersion = syntax.languageVersion;
    }

    CompileResult run() {
        installBuiltins();
        declareTopLevelNames();
        resolveInputs();
        resolveConstants();
        resolveLimits();
        resolveEnums();
        resolveRecords();
        resolveEntries();
        resolveOutforms();
        resolveDefaultEntry();

        CompileResult result;
        result.syntax = std::make_shared<SyntaxFile>(m_syntax);
        result.diagnostics = std::move(m_diagnostics);
        if (!std::any_of(result.diagnostics.cbegin(), result.diagnostics.cend(),
                         isError)) {
            result.program = std::move(m_program);
        }
        return result;
    }

private:
    SymbolId intern(const QString& text) {
        const auto found = m_symbolIds.constFind(text);
        if (found != m_symbolIds.constEnd()) {
            return *found;
        }
        const SymbolId id = static_cast<SymbolId>(m_program->symbols.size());
        m_program->symbols.push_back(text);
        m_symbolIds.insert(text, id);
        return id;
    }

    SourceSpanId addSpan(SourceSpan span) {
        const SourceSpanId id =
            static_cast<SourceSpanId>(m_program->sourceSpans.size());
        m_program->sourceSpans.push_back(span);
        return id;
    }

    void report(const QString& code, const QString& message, SourceSpan span,
                QVector<RelatedDiagnostic> related = {}) {
        m_diagnostics.push_back(
            {DiagnosticSeverity::Error, code, message, span, std::move(related)});
    }

    void warn(const QString& code, const QString& message, SourceSpan span) {
        m_diagnostics.push_back(
            {DiagnosticSeverity::Warning, code, message, span, {}});
    }

    IdRange appendRefs(QVector<quint32>* destination,
                       const QVector<quint32>& values) {
        const IdRange range{static_cast<quint32>(destination->size()),
                            static_cast<quint32>(values.size())};
        *destination += values;
        return range;
    }

    TypeId addType(TypeDesc type) {
        const TypeId id = static_cast<TypeId>(m_program->types.size());
        m_program->types.push_back(std::move(type));
        m_pendingFields.resize(m_program->types.size());
        return id;
    }

    TypeId addBuiltin(const QString& name, TypeKind kind, quint16 bitWidth = 0,
                      Endianness endianness = Endianness::None) {
        TypeDesc type;
        type.kind = kind;
        type.name = intern(name);
        type.bitWidth = bitWidth;
        type.endianness = endianness;
        const TypeId id = addType(type);
        m_typeIds.insert(name, id);
        return id;
    }

    void installBuiltins() {
        m_voidType = addBuiltin(QStringLiteral("void"), TypeKind::Void);
        m_boolType = addBuiltin(QStringLiteral("bool"), TypeKind::Boolean, 1);
        m_stringType = addBuiltin(QStringLiteral("string"), TypeKind::String);
        m_bytesType = addBuiltin(QStringLiteral("bytes"), TypeKind::Bytes);
        m_nodeType = addBuiltin(QStringLiteral("node"), TypeKind::Node);
        m_spanType = addBuiltin(QStringLiteral("span"), TypeKind::Span);

        addBuiltin(QStringLiteral("u8"), TypeKind::UnsignedInteger, 8);
        addBuiltin(QStringLiteral("i8"), TypeKind::SignedInteger, 8);
        const struct IntegerBuiltin {
            const char* name;
            TypeKind kind;
            quint16 width;
            Endianness endianness;
        } integers[] = {
            {"u16le", TypeKind::UnsignedInteger, 16, Endianness::Little},
            {"u16be", TypeKind::UnsignedInteger, 16, Endianness::Big},
            {"u32le", TypeKind::UnsignedInteger, 32, Endianness::Little},
            {"u32be", TypeKind::UnsignedInteger, 32, Endianness::Big},
            {"u64le", TypeKind::UnsignedInteger, 64, Endianness::Little},
            {"u64be", TypeKind::UnsignedInteger, 64, Endianness::Big},
            {"i16le", TypeKind::SignedInteger, 16, Endianness::Little},
            {"i16be", TypeKind::SignedInteger, 16, Endianness::Big},
            {"i32le", TypeKind::SignedInteger, 32, Endianness::Little},
            {"i32be", TypeKind::SignedInteger, 32, Endianness::Big},
            {"i64le", TypeKind::SignedInteger, 64, Endianness::Little},
            {"i64be", TypeKind::SignedInteger, 64, Endianness::Big},
        };
        for (const IntegerBuiltin& integer : integers) {
            addBuiltin(QLatin1String(integer.name), integer.kind, integer.width,
                       integer.endianness);
        }
        addBuiltin(QStringLiteral("u16"), TypeKind::UnsignedInteger, 16);
        addBuiltin(QStringLiteral("u32"), TypeKind::UnsignedInteger, 32);
        addBuiltin(QStringLiteral("u64"), TypeKind::UnsignedInteger, 64);
        addBuiltin(QStringLiteral("i16"), TypeKind::SignedInteger, 16);
        addBuiltin(QStringLiteral("i32"), TypeKind::SignedInteger, 32);
        addBuiltin(QStringLiteral("i64"), TypeKind::SignedInteger, 64);
        addBuiltin(QStringLiteral("f32"), TypeKind::FloatingPoint, 32);
        addBuiltin(QStringLiteral("f32le"), TypeKind::FloatingPoint, 32,
                   Endianness::Little);
        addBuiltin(QStringLiteral("f32be"), TypeKind::FloatingPoint, 32,
                   Endianness::Big);
        addBuiltin(QStringLiteral("f64"), TypeKind::FloatingPoint, 64);
        addBuiltin(QStringLiteral("f64le"), TypeKind::FloatingPoint, 64,
                   Endianness::Little);
        addBuiltin(QStringLiteral("f64be"), TypeKind::FloatingPoint, 64,
                   Endianness::Big);
        m_u64Type = m_typeIds.value(QStringLiteral("u64le"));
        m_i64Type = m_typeIds.value(QStringLiteral("i64le"));
    }

    bool declareName(const QString& name, SourceSpan span, QStringView kind) {
        const auto found = m_topLevelSpans.constFind(name);
        if (found != m_topLevelSpans.constEnd()) {
            report(QStringLiteral("BR0001"),
                   QStringLiteral("Duplicate top-level name '%1'").arg(name), span,
                   {{*found, QStringLiteral("Previous declaration is here")}});
            return false;
        }
        m_topLevelSpans.insert(name, span);
        m_topLevelKinds.insert(name, kind.toString());
        return true;
    }

    void declareTopLevelNames() {
        for (qsizetype i = 0; i < m_syntax.constants.size(); ++i) {
            const SyntaxConstant& constant = m_syntax.constants.at(i);
            if (declareName(constant.name, constant.nameSpan, u"constant")) {
                m_constantSyntax.insert(constant.name, i);
            }
        }
        for (qsizetype i = 0; i < m_syntax.enums.size(); ++i) {
            const SyntaxEnum& declaration = m_syntax.enums.at(i);
            if (!declareName(declaration.name, declaration.nameSpan, u"enum")) {
                continue;
            }
            TypeDesc type;
            type.kind = TypeKind::Enum;
            type.name = intern(declaration.name);
            const TypeId typeId = addType(type);
            m_typeIds.insert(declaration.name, typeId);
            m_enumSyntax.insert(declaration.name, i);
        }
        m_recordStatus.resize(m_syntax.records.size());
        for (qsizetype i = 0; i < m_syntax.records.size(); ++i) {
            const SyntaxRecord& record = m_syntax.records.at(i);
            if (!declareName(record.name, record.nameSpan, u"record")) {
                continue;
            }
            TypeDesc type;
            type.kind = TypeKind::Record;
            type.name = intern(record.name);
            const TypeId typeId = addType(type);
            m_typeIds.insert(record.name, typeId);
            m_recordSyntax.insert(record.name, i);
            m_recordTypeIds.resize(m_syntax.records.size(), kInvalidId);
            m_recordTypeIds[i] = typeId;
        }
        m_recordDescriptors.resize(m_syntax.records.size(), kInvalidId);
        m_entryTypeIds.resize(m_syntax.entries.size(), kInvalidId);
        for (qsizetype i = 0; i < m_syntax.entries.size(); ++i) {
            const SyntaxEntry& entry = m_syntax.entries.at(i);
            if (!declareName(entry.name, entry.nameSpan, u"entry")) {
                continue;
            }
            TypeDesc type;
            type.kind = TypeKind::Shape;
            type.name = intern(entry.name);
            const TypeId typeId = addType(type);
            m_typeIds.insert(entry.name, typeId);
            m_entrySyntax.insert(entry.name, i);
            m_entryTypeIds[i] = typeId;
        }
        for (const SyntaxOutform& outform : m_syntax.outforms) {
            const auto found = m_outformSpans.constFind(outform.name);
            if (found != m_outformSpans.constEnd()) {
                report(QStringLiteral("BR0002"),
                       QStringLiteral("Duplicate outform name '%1'")
                           .arg(outform.name),
                       outform.nameSpan,
                       {{*found, QStringLiteral("Previous outform is here")}});
            } else {
                m_outformSpans.insert(outform.name, outform.nameSpan);
            }
        }
    }

    TypeId resolveType(const SyntaxType& syntaxType) {
        const auto found = m_typeIds.constFind(syntaxType.name);
        if (found == m_typeIds.constEnd()) {
            report(QStringLiteral("BR0100"),
                   QStringLiteral("Unknown type '%1'").arg(syntaxType.name),
                   syntaxType.span);
            return kInvalidId;
        }
        return *found;
    }

    TypeId sequenceType(TypeId element) {
        const auto found = m_sequenceTypes.constFind(element);
        if (found != m_sequenceTypes.constEnd()) {
            return *found;
        }
        TypeDesc type;
        type.kind = TypeKind::Sequence;
        type.elementType = element;
        const TypeId id = addType(type);
        m_sequenceTypes.insert(element, id);
        return id;
    }

    TypeId optionalType(TypeId element) {
        const auto found = m_optionalTypes.constFind(element);
        if (found != m_optionalTypes.constEnd()) {
            return *found;
        }
        TypeDesc type;
        type.kind = TypeKind::Optional;
        type.elementType = element;
        const TypeId id = addType(type);
        m_optionalTypes.insert(element, id);
        return id;
    }

    TypeId variantType(const QVector<TypeId>& alternatives) {
        TypeDesc type;
        type.kind = TypeKind::Variant;
        type.alternatives = appendRefs(&m_program->typeRefs, alternatives);
        return addType(type);
    }

    TypeId newShape(const QString& displayName = {}) {
        TypeDesc type;
        type.kind = TypeKind::Shape;
        if (!displayName.isEmpty()) {
            type.name = intern(displayName);
        }
        return addType(type);
    }

    void addPendingField(TypeId owner, const QString& name, TypeId type,
                         StatementId statement, bool optional = false) {
        if (owner == kInvalidId || owner >= static_cast<TypeId>(m_pendingFields.size())) {
            return;
        }
        for (quint32 fieldId : m_pendingFields.at(owner)) {
            if (m_program->symbol(m_program->fields.at(fieldId).name) == name) {
                report(QStringLiteral("BR0101"),
                       QStringLiteral("Duplicate field '%1'").arg(name),
                       m_program->sourceSpans.at(
                           m_program->statements.at(statement).sourceSpan));
                return;
            }
        }
        FieldDesc field;
        field.name = intern(name);
        field.type = optional ? optionalType(type) : type;
        field.statement = statement;
        field.optional = optional;
        const quint32 fieldId = static_cast<quint32>(m_program->fields.size());
        m_program->fields.push_back(field);
        m_pendingFields[owner].push_back(fieldId);
    }

    void finalizeTypeFields(TypeId type) {
        if (type == kInvalidId || type >= static_cast<TypeId>(m_pendingFields.size())) {
            return;
        }
        m_program->types[type].fields =
            appendRefs(&m_program->fieldRefs, m_pendingFields.at(type));
    }

    const FieldDesc* findField(TypeId type, QStringView name) {
        if (type == kInvalidId || type >= static_cast<TypeId>(m_program->types.size())) {
            return nullptr;
        }
        const TypeDesc& descriptor = m_program->types.at(type);
        if (descriptor.kind == TypeKind::Optional) {
            return findField(descriptor.elementType, name);
        }
        if (descriptor.kind == TypeKind::Variant) {
            const FieldDesc* result = nullptr;
            for (quint32 i = 0; i < descriptor.alternatives.count; ++i) {
                const TypeId alternative =
                    m_program->typeRefs.at(descriptor.alternatives.first + i);
                const FieldDesc* candidate = findField(alternative, name);
                if (candidate == nullptr) {
                    continue;
                }
                if (result != nullptr && result->type != candidate->type) {
                    return nullptr;
                }
                result = candidate;
            }
            return result;
        }
        if (descriptor.kind == TypeKind::Record) {
            const QString typeName = m_program->symbol(descriptor.name);
            const auto record = m_recordSyntax.constFind(typeName);
            if (record != m_recordSyntax.constEnd()) {
                ensureRecordResolved(*record);
            }
        }
        for (quint32 fieldId : m_pendingFields.value(type)) {
            const FieldDesc& field = m_program->fields.at(fieldId);
            if (m_program->symbol(field.name) == name) {
                return &field;
            }
        }
        return nullptr;
    }

    void resolveInputs() {
        int defaults = 0;
        for (const SyntaxInput& syntaxInput : m_syntax.inputs) {
            if (m_inputIds.contains(syntaxInput.name)) {
                report(QStringLiteral("BR0200"),
                       QStringLiteral("Duplicate input '%1'").arg(syntaxInput.name),
                       syntaxInput.nameSpan);
                continue;
            }
            InputDesc input;
            input.name = intern(syntaxInput.name);
            input.label = intern(syntaxInput.label);
            input.description = intern(syntaxInput.description);
            input.isDefault = syntaxInput.isDefault;
            if (input.isDefault) {
                ++defaults;
            }
            const InputId id = static_cast<InputId>(m_program->inputs.size());
            m_program->inputs.push_back(input);
            m_inputIds.insert(syntaxInput.name, id);
        }
        if (m_program->inputs.isEmpty()) {
            report(QStringLiteral("BR0201"),
                   QStringLiteral("At least one input must be declared"), {0, 0});
        }
        if (defaults > 1) {
            report(QStringLiteral("BR0202"),
                   QStringLiteral("Only one input may be the default input"),
                   m_syntax.inputs.first().nameSpan);
        }
    }

    std::optional<quint64> constantInteger(SyntaxExpressionId id,
                                           QSet<QString>* active = nullptr) {
        if (id == kInvalidSyntaxExpression ||
            id >= static_cast<SyntaxExpressionId>(m_syntax.expressions.size())) {
            return std::nullopt;
        }
        const SyntaxExpression& expression = m_syntax.expressions.at(id);
        switch (expression.kind) {
            case SyntaxExpressionKind::UnsignedInteger:
                return expression.unsignedValue;
            case SyntaxExpressionKind::Identifier: {
                const auto known = m_constantValues.constFind(expression.text);
                if (known != m_constantValues.constEnd()) {
                    return *known;
                }
                const auto syntaxIndex = m_constantSyntax.constFind(expression.text);
                if (syntaxIndex == m_constantSyntax.constEnd()) {
                    return std::nullopt;
                }
                QSet<QString> localActive;
                if (active == nullptr) {
                    active = &localActive;
                }
                if (active->contains(expression.text)) {
                    report(QStringLiteral("BR0203"),
                           QStringLiteral("Constant cycle involving '%1'")
                               .arg(expression.text),
                           expression.span);
                    return std::nullopt;
                }
                active->insert(expression.text);
                const auto value = constantInteger(
                    m_syntax.constants.at(*syntaxIndex).value, active);
                active->remove(expression.text);
                if (value.has_value()) {
                    m_constantValues.insert(expression.text, *value);
                }
                return value;
            }
            case SyntaxExpressionKind::Unary: {
                if (expression.operands.isEmpty()) {
                    return std::nullopt;
                }
                const auto operand = constantInteger(expression.operands.first(), active);
                if (!operand.has_value()) {
                    return std::nullopt;
                }
                if (expression.unaryOperator == SyntaxUnaryOperator::LogicalNot) {
                    return *operand == 0 ? 1 : 0;
                }
                return std::nullopt;
            }
            case SyntaxExpressionKind::Binary: {
                if (expression.operands.size() != 2) {
                    return std::nullopt;
                }
                const auto left = constantInteger(expression.operands.at(0), active);
                const auto right = constantInteger(expression.operands.at(1), active);
                if (!left.has_value() || !right.has_value()) {
                    return std::nullopt;
                }
                quint64 result = 0;
                switch (expression.binaryOperator) {
                    case SyntaxBinaryOperator::Add:
                        return checkedAdd(*left, *right, &result)
                                   ? std::optional<quint64>(result)
                                   : std::nullopt;
                    case SyntaxBinaryOperator::Subtract:
                        return *left >= *right ? std::optional<quint64>(*left - *right)
                                               : std::nullopt;
                    case SyntaxBinaryOperator::Multiply:
                        return checkedMultiply(*left, *right, &result)
                                   ? std::optional<quint64>(result)
                                   : std::nullopt;
                    case SyntaxBinaryOperator::Divide:
                        return *right != 0 ? std::optional<quint64>(*left / *right)
                                           : std::nullopt;
                    case SyntaxBinaryOperator::Remainder:
                        return *right != 0 ? std::optional<quint64>(*left % *right)
                                           : std::nullopt;
                    case SyntaxBinaryOperator::Equal: return *left == *right;
                    case SyntaxBinaryOperator::NotEqual: return *left != *right;
                    case SyntaxBinaryOperator::Less: return *left < *right;
                    case SyntaxBinaryOperator::LessEqual: return *left <= *right;
                    case SyntaxBinaryOperator::Greater: return *left > *right;
                    case SyntaxBinaryOperator::GreaterEqual: return *left >= *right;
                    case SyntaxBinaryOperator::LogicalAnd:
                        return (*left != 0) && (*right != 0);
                    case SyntaxBinaryOperator::LogicalOr:
                        return (*left != 0) || (*right != 0);
                    case SyntaxBinaryOperator::Range: return std::nullopt;
                }
                return std::nullopt;
            }
            default: return std::nullopt;
        }
    }

    void resolveConstants() {
        ResolveContext context;
        context.scopes.push_back({});
        for (const SyntaxConstant& syntaxConstant : m_syntax.constants) {
            ConstantDesc constant;
            constant.name = intern(syntaxConstant.name);
            constant.value = resolveExpression(syntaxConstant.value, &context);
            constant.type = syntaxConstant.hasExplicitType
                                ? resolveType(syntaxConstant.type)
                                : expressionType(constant.value);
            m_program->constants.push_back(constant);
            m_constantIds.insert(syntaxConstant.name,
                                 static_cast<quint32>(m_program->constants.size() - 1));
            const auto value = constantInteger(syntaxConstant.value);
            if (value.has_value()) {
                m_constantValues.insert(syntaxConstant.name, *value);
            } else {
                report(QStringLiteral("BR0204"),
                       QStringLiteral("Constant '%1' is not a constant integer expression")
                           .arg(syntaxConstant.name),
                       syntaxConstant.nameSpan);
            }
        }
    }

    void resolveLimits() {
        QSet<QString> seen;
        for (const SyntaxLimit& limit : m_syntax.limits) {
            if (seen.contains(limit.name)) {
                report(QStringLiteral("BR0210"),
                       QStringLiteral("Duplicate limit '%1'").arg(limit.name),
                       limit.nameSpan);
                continue;
            }
            seen.insert(limit.name);
            const auto value = constantInteger(limit.value);
            if (!value.has_value() || *value == 0) {
                report(QStringLiteral("BR0211"),
                       QStringLiteral("Limit '%1' must be a positive constant integer")
                           .arg(limit.name),
                       limit.nameSpan);
                continue;
            }
            if (limit.name == QStringLiteral("max_parse_depth")) {
                if (*value > std::numeric_limits<quint32>::max()) {
                    report(QStringLiteral("BR0212"),
                           QStringLiteral("max_parse_depth is too large"),
                           limit.nameSpan);
                } else {
                    m_program->limits.maxParseDepth = static_cast<quint32>(*value);
                }
            } else if (limit.name == QStringLiteral("max_loop_iterations")) {
                m_program->limits.maxLoopIterations = *value;
            } else if (limit.name == QStringLiteral("max_nodes")) {
                m_program->limits.maxNodes = *value;
            } else if (limit.name == QStringLiteral("max_probe_bytes")) {
                m_program->limits.maxProbeBytes = *value;
            } else if (limit.name == QStringLiteral("max_transform_output")) {
                m_program->limits.maxTransformOutput = *value;
            } else {
                report(QStringLiteral("BR0213"),
                       QStringLiteral("Unknown limit '%1'").arg(limit.name),
                       limit.nameSpan);
            }
        }
    }

    void resolveEnums() {
        ResolveContext context;
        context.scopes.push_back({});
        for (const SyntaxEnum& syntaxEnum : m_syntax.enums) {
            const TypeId enumType = m_typeIds.value(syntaxEnum.name, kInvalidId);
            if (enumType == kInvalidId) {
                continue;
            }
            const TypeId underlying = resolveType(syntaxEnum.underlyingType);
            if (!isIntegerType(underlying)) {
                report(QStringLiteral("BR0220"),
                       QStringLiteral("Enum underlying type must be an integer"),
                       syntaxEnum.underlyingType.span);
            }
            const quint32 first = static_cast<quint32>(m_program->enumValues.size());
            QSet<QString> names;
            QSet<quint64> values;
            for (const SyntaxEnumMember& syntaxMember : syntaxEnum.members) {
                const auto value = constantInteger(syntaxMember.value);
                if (!value.has_value()) {
                    report(QStringLiteral("BR0221"),
                           QStringLiteral("Enum value must be a constant integer"),
                           syntaxMember.nameSpan);
                    continue;
                }
                if (names.contains(syntaxMember.name)) {
                    report(QStringLiteral("BR0222"),
                           QStringLiteral("Duplicate enum member '%1'")
                               .arg(syntaxMember.name),
                           syntaxMember.nameSpan);
                    continue;
                }
                if (values.contains(*value)) {
                    warn(QStringLiteral("BR1220"),
                         QStringLiteral("Duplicate numeric enum value %1").arg(*value),
                         syntaxMember.nameSpan);
                }
                names.insert(syntaxMember.name);
                values.insert(*value);
                m_program->enumValues.push_back(
                    {intern(syntaxMember.name), *value, addSpan(syntaxMember.nameSpan)});
                m_enumValues.insert(syntaxEnum.name + QLatin1Char('.') + syntaxMember.name,
                                    qMakePair(enumType, *value));
            }
            EnumDesc declaration;
            declaration.name = intern(syntaxEnum.name);
            declaration.type = enumType;
            declaration.underlyingType = underlying;
            declaration.values =
                {first, static_cast<quint32>(m_program->enumValues.size()) - first};
            m_program->enums.push_back(declaration);
            m_program->types[enumType].elementType = underlying;
        }
    }

    bool isIntegerType(TypeId type) const {
        if (type == kInvalidId || type >= static_cast<TypeId>(m_program->types.size())) {
            return false;
        }
        const TypeKind kind = m_program->types.at(type).kind;
        return kind == TypeKind::UnsignedInteger || kind == TypeKind::SignedInteger ||
               kind == TypeKind::Enum || kind == TypeKind::Bitfield;
    }

    bool isNumericType(TypeId type) const {
        return isIntegerType(type) ||
               (type != kInvalidId && type < static_cast<TypeId>(m_program->types.size()) &&
                m_program->types.at(type).kind == TypeKind::FloatingPoint);
    }

    bool isBooleanType(TypeId type) const { return type == m_boolType; }

    TypeId expressionType(ExpressionId expression) const {
        return expression != kInvalidId &&
                       expression < static_cast<ExpressionId>(m_program->expressions.size())
                   ? m_program->expressions.at(expression).type
                   : kInvalidId;
    }

    ExpressionId appendExpression(Expression expression,
                                  const QVector<ExpressionId>& operands = {}) {
        expression.operands = appendRefs(&m_program->expressionRefs, operands);
        const ExpressionId id =
            static_cast<ExpressionId>(m_program->expressions.size());
        m_program->expressions.push_back(std::move(expression));
        return id;
    }

    ExpressionId resolveExpression(SyntaxExpressionId syntaxId,
                                   ResolveContext* context) {
        if (syntaxId == kInvalidSyntaxExpression ||
            syntaxId >= static_cast<SyntaxExpressionId>(m_syntax.expressions.size())) {
            return kInvalidId;
        }
        const SyntaxExpression& syntax = m_syntax.expressions.at(syntaxId);
        Expression expression;
        expression.sourceSpan = addSpan(syntax.span);
        expression.unsignedValue = syntax.unsignedValue;
        expression.floatingValue = syntax.floatingValue;
        expression.booleanValue = syntax.booleanValue;
        expression.unaryOperator = syntax.unaryOperator;
        expression.binaryOperator = syntax.binaryOperator;

        switch (syntax.kind) {
            case SyntaxExpressionKind::UnsignedInteger:
                expression.kind = ExpressionKind::UnsignedInteger;
                expression.type = m_u64Type;
                return appendExpression(expression);
            case SyntaxExpressionKind::FloatingPoint:
                expression.kind = ExpressionKind::FloatingPoint;
                expression.type = m_typeIds.value(QStringLiteral("f64"));
                return appendExpression(expression);
            case SyntaxExpressionKind::String:
                expression.kind = ExpressionKind::String;
                expression.type = m_stringType;
                expression.symbol = intern(syntax.text);
                return appendExpression(expression);
            case SyntaxExpressionKind::Boolean:
                expression.kind = ExpressionKind::Boolean;
                expression.type = m_boolType;
                return appendExpression(expression);
            case SyntaxExpressionKind::Identifier:
                return resolveIdentifier(syntax, context, expression);
            case SyntaxExpressionKind::Member:
            case SyntaxExpressionKind::MetadataMember:
                return resolveMember(syntax, context, expression);
            case SyntaxExpressionKind::Unary:
                return resolveUnary(syntax, context, expression);
            case SyntaxExpressionKind::Binary:
                return resolveBinary(syntax, context, expression);
            case SyntaxExpressionKind::Call:
                return resolveCall(syntax, context, expression);
            case SyntaxExpressionKind::ByteArray:
                return resolveByteArray(syntax, context, expression);
            case SyntaxExpressionKind::InterpolatedString:
                return resolveInterpolatedString(syntax, context, expression);
            case SyntaxExpressionKind::Invalid: break;
        }
        expression.kind = ExpressionKind::Invalid;
        expression.type = kInvalidId;
        return appendExpression(expression);
    }

    ExpressionId resolveIdentifier(const SyntaxExpression& syntax,
                                   ResolveContext* context,
                                   Expression expression) {
        if (const auto binding = context->find(syntax.text); binding.has_value()) {
            expression.kind = ExpressionKind::Slot;
            expression.type = binding->type;
            expression.slot = binding->slot;
            expression.symbol = intern(syntax.text);
            return appendExpression(expression);
        }
        const auto constant = m_constantIds.constFind(syntax.text);
        if (constant != m_constantIds.constEnd()) {
            expression.kind = ExpressionKind::Constant;
            expression.type = m_program->constants.at(*constant).type;
            expression.unsignedValue = m_constantValues.value(syntax.text);
            expression.symbol = intern(syntax.text);
            return appendExpression(expression);
        }
        const auto type = m_typeIds.constFind(syntax.text);
        if (type != m_typeIds.constEnd() &&
            m_program->types.at(*type).kind == TypeKind::Enum) {
            expression.kind = ExpressionKind::TypeName;
            expression.type = *type;
            expression.symbol = intern(syntax.text);
            return appendExpression(expression);
        }
        if (syntax.text == QStringLiteral("remaining") ||
            syntax.text == QStringLiteral("iteration")) {
            expression.kind = ExpressionKind::Slot;
            expression.type = m_u64Type;
            expression.symbol = intern(syntax.text);
            return appendExpression(expression);
        }
        if (syntax.text == QStringLiteral("at_end")) {
            expression.kind = ExpressionKind::Slot;
            expression.type = m_boolType;
            expression.symbol = intern(syntax.text);
            return appendExpression(expression);
        }
        report(QStringLiteral("BR0300"),
               QStringLiteral("Unknown name '%1'").arg(syntax.text), syntax.span);
        expression.kind = ExpressionKind::Invalid;
        expression.type = kInvalidId;
        return appendExpression(expression);
    }

    ExpressionId resolveMember(const SyntaxExpression& syntax,
                               ResolveContext* context,
                               Expression expression) {
        if (syntax.operands.isEmpty()) {
            return appendExpression(expression);
        }
        const ExpressionId base = resolveExpression(syntax.operands.first(), context);
        const Expression& baseExpression = m_program->expressions.at(base);
        expression.symbol = intern(syntax.text);
        if (syntax.kind == SyntaxExpressionKind::MetadataMember) {
            expression.kind = ExpressionKind::MetadataMember;
            expression.type = metadataType(syntax.text, syntax.span);
            return appendExpression(expression, {base});
        }
        if (baseExpression.kind == ExpressionKind::TypeName) {
            const QString qualified =
                m_program->symbol(baseExpression.symbol) + QLatin1Char('.') + syntax.text;
            const auto enumValue = m_enumValues.constFind(qualified);
            if (enumValue == m_enumValues.constEnd()) {
                report(QStringLiteral("BR0301"),
                       QStringLiteral("Unknown enum member '%1'").arg(qualified),
                       syntax.span);
                expression.kind = ExpressionKind::Invalid;
                expression.type = kInvalidId;
            } else {
                expression.kind = ExpressionKind::EnumValue;
                expression.type = enumValue->first;
                expression.unsignedValue = enumValue->second;
            }
            return appendExpression(expression, {base});
        }
        const FieldDesc* field = findField(baseExpression.type, syntax.text);
        if (field == nullptr) {
            report(QStringLiteral("BR0302"),
                   QStringLiteral("Type has no field '%1'").arg(syntax.text),
                   syntax.span);
            expression.kind = ExpressionKind::Invalid;
            expression.type = kInvalidId;
        } else {
            expression.kind = ExpressionKind::Member;
            expression.type = field->type;
        }
        return appendExpression(expression, {base});
    }

    TypeId metadataType(const QString& name, SourceSpan span) {
        static const QSet<QString> strings{
            QStringLiteral("name"), QStringLiteral("type"),
            QStringLiteral("input"), QStringLiteral("path"),
            QStringLiteral("error")};
        if (strings.contains(name)) {
            return m_stringType;
        }
        if (name == QStringLiteral("valid")) {
            return m_boolType;
        }
        if (name == QStringLiteral("offset") || name == QStringLiteral("length")) {
            return m_u64Type;
        }
        if (name == QStringLiteral("bytes")) {
            return m_bytesType;
        }
        if (name == QStringLiteral("children")) {
            return sequenceType(m_nodeType);
        }
        if (name == QStringLiteral("spans")) {
            return sequenceType(m_spanType);
        }
        if (name == QStringLiteral("value")) {
            return m_nodeType;
        }
        report(QStringLiteral("BR0303"),
               QStringLiteral("Unknown node metadata property '@%1'").arg(name), span);
        return kInvalidId;
    }

    ExpressionId resolveUnary(const SyntaxExpression& syntax,
                              ResolveContext* context,
                              Expression expression) {
        const ExpressionId operand = resolveExpression(syntax.operands.value(0), context);
        const TypeId operandType = expressionType(operand);
        expression.kind = ExpressionKind::Unary;
        expression.type = syntax.unaryOperator == SyntaxUnaryOperator::LogicalNot
                              ? m_boolType
                              : operandType;
        if (syntax.unaryOperator == SyntaxUnaryOperator::LogicalNot &&
            !isBooleanType(operandType)) {
            report(QStringLiteral("BR0310"),
                   QStringLiteral("Logical negation requires a Boolean operand"),
                   syntax.span);
        } else if (syntax.unaryOperator == SyntaxUnaryOperator::Negate &&
                   !isNumericType(operandType)) {
            report(QStringLiteral("BR0311"),
                   QStringLiteral("Numeric negation requires a numeric operand"),
                   syntax.span);
        }
        return appendExpression(expression, {operand});
    }

    ExpressionId resolveBinary(const SyntaxExpression& syntax,
                               ResolveContext* context,
                               Expression expression) {
        const ExpressionId left = resolveExpression(syntax.operands.value(0), context);
        const ExpressionId right = resolveExpression(syntax.operands.value(1), context);
        const TypeId leftType = expressionType(left);
        const TypeId rightType = expressionType(right);
        expression.kind = ExpressionKind::Binary;
        switch (syntax.binaryOperator) {
            case SyntaxBinaryOperator::Equal:
            case SyntaxBinaryOperator::NotEqual:
            case SyntaxBinaryOperator::Less:
            case SyntaxBinaryOperator::LessEqual:
            case SyntaxBinaryOperator::Greater:
            case SyntaxBinaryOperator::GreaterEqual:
            case SyntaxBinaryOperator::LogicalAnd:
            case SyntaxBinaryOperator::LogicalOr:
                expression.type = m_boolType;
                break;
            default:
                expression.type =
                    isFloatingType(leftType) || isFloatingType(rightType)
                        ? m_typeIds.value(QStringLiteral("f64"))
                        : leftType;
                break;
        }
        if ((syntax.binaryOperator == SyntaxBinaryOperator::LogicalAnd ||
             syntax.binaryOperator == SyntaxBinaryOperator::LogicalOr) &&
            (!isBooleanType(leftType) || !isBooleanType(rightType))) {
            report(QStringLiteral("BR0312"),
                   QStringLiteral("Logical operators require Boolean operands"),
                   syntax.span);
        }
        if (syntax.binaryOperator == SyntaxBinaryOperator::Add ||
            syntax.binaryOperator == SyntaxBinaryOperator::Subtract ||
            syntax.binaryOperator == SyntaxBinaryOperator::Multiply ||
            syntax.binaryOperator == SyntaxBinaryOperator::Divide ||
            syntax.binaryOperator == SyntaxBinaryOperator::Remainder) {
            if (!isNumericType(leftType) || !isNumericType(rightType)) {
                report(QStringLiteral("BR0313"),
                       QStringLiteral("Arithmetic operators require numeric operands"),
                       syntax.span);
            }
        }
        return appendExpression(expression, {left, right});
    }

    bool isFloatingType(TypeId type) const {
        return type != kInvalidId && type < static_cast<TypeId>(m_program->types.size()) &&
               m_program->types.at(type).kind == TypeKind::FloatingPoint;
    }

    ExpressionId resolveCall(const SyntaxExpression& syntax,
                             ResolveContext* context,
                             Expression expression) {
        if (syntax.operands.isEmpty()) {
            expression.kind = ExpressionKind::Invalid;
            return appendExpression(expression);
        }
        const SyntaxExpression& calleeSyntax =
            m_syntax.expressions.at(syntax.operands.first());
        if (calleeSyntax.kind != SyntaxExpressionKind::Identifier) {
            report(QStringLiteral("BR0320"),
                   QStringLiteral("Only named functions may be called"), syntax.span);
            expression.kind = ExpressionKind::Invalid;
            expression.type = kInvalidId;
            return appendExpression(expression);
        }
        const QString function = calleeSyntax.text;
        QVector<ExpressionId> arguments;
        for (qsizetype i = 1; i < syntax.operands.size(); ++i) {
            arguments.push_back(resolveExpression(syntax.operands.at(i), context));
        }
        expression.kind = ExpressionKind::Call;
        expression.symbol = intern(function);
        static const QSet<QString> textFunctions{
            QStringLiteral("str"),       QStringLiteral("dec"),
            QStringLiteral("hex"),       QStringLiteral("hex_bytes"),
            QStringLiteral("csv"),       QStringLiteral("json"),
            QStringLiteral("enum_name"), QStringLiteral("upper"),
            QStringLiteral("lower")};
        static const QSet<QString> byteFunctions{
            QStringLiteral("u8"),      QStringLiteral("i8"),
            QStringLiteral("u16le"),   QStringLiteral("u16be"),
            QStringLiteral("i16le"),   QStringLiteral("i16be"),
            QStringLiteral("u32le"),   QStringLiteral("u32be"),
            QStringLiteral("i32le"),   QStringLiteral("i32be"),
            QStringLiteral("u64le"),   QStringLiteral("u64be"),
            QStringLiteral("i64le"),   QStringLiteral("i64be"),
            QStringLiteral("f32le"),   QStringLiteral("f32be"),
            QStringLiteral("f64le"),   QStringLiteral("f64be"),
            QStringLiteral("utf8"),    QStringLiteral("utf16le"),
            QStringLiteral("utf16be")};
        if (textFunctions.contains(function)) {
            expression.type = m_stringType;
        } else if (byteFunctions.contains(function)) {
            expression.type = m_bytesType;
        } else if (function == QStringLiteral("present")) {
            expression.type = m_boolType;
        } else if (function == QStringLiteral("count") ||
                   function == QStringLiteral("int")) {
            expression.type = m_u64Type;
        } else {
            report(QStringLiteral("BR0321"),
                   QStringLiteral("Unknown function '%1'").arg(function),
                   calleeSyntax.span);
            expression.type = kInvalidId;
        }
        if (arguments.size() != 1) {
            report(QStringLiteral("BR0322"),
                   QStringLiteral("Function '%1' expects one argument").arg(function),
                   syntax.span);
        }
        return appendExpression(expression, arguments);
    }

    ExpressionId resolveByteArray(const SyntaxExpression& syntax,
                                  ResolveContext* context,
                                  Expression expression) {
        QVector<ExpressionId> bytes;
        for (SyntaxExpressionId operand : syntax.operands) {
            const auto value = constantInteger(operand);
            if (!value.has_value() || *value > 255) {
                report(QStringLiteral("BR0330"),
                       QStringLiteral("Byte array elements must be constants from 0 to 255"),
                       m_syntax.expressions.at(operand).span);
            }
            bytes.push_back(resolveExpression(operand, context));
        }
        expression.kind = ExpressionKind::ByteArray;
        expression.type = m_bytesType;
        return appendExpression(expression, bytes);
    }

    ExpressionId resolveInterpolatedString(const SyntaxExpression& syntax,
                                           ResolveContext* context,
                                           Expression expression) {
        QVector<ExpressionId> operands;
        for (SyntaxExpressionId operand : syntax.operands) {
            const ExpressionId resolved = resolveExpression(operand, context);
            const TypeId type = expressionType(resolved);
            if (type == m_bytesType || type == m_nodeType ||
                (type != kInvalidId &&
                 (m_program->types.at(type).kind == TypeKind::Record ||
                  m_program->types.at(type).kind == TypeKind::Shape ||
                  m_program->types.at(type).kind == TypeKind::Sequence))) {
                report(QStringLiteral("BR0340"),
                       QStringLiteral("Value requires explicit formatting before interpolation"),
                       m_syntax.expressions.at(operand).span);
            }
            operands.push_back(resolved);
        }
        QVector<SymbolId> parts;
        for (const QString& part : syntax.textParts) {
            parts.push_back(intern(part));
        }
        expression.kind = ExpressionKind::InterpolatedString;
        expression.type = m_stringType;
        expression.textParts = appendRefs(&m_program->textPartSymbols, parts);
        return appendExpression(expression, operands);
    }

    quint32 addExtent(ExtentSummary extent) {
        const quint32 id = static_cast<quint32>(m_program->extents.size());
        m_program->extents.push_back(std::move(extent));
        return id;
    }

    ExtentSummary extentForType(TypeId type) {
        ExtentSummary extent;
        if (type == kInvalidId || type >= static_cast<TypeId>(m_program->types.size())) {
            extent.mayFail = true;
            return extent;
        }
        const TypeDesc& descriptor = m_program->types.at(type);
        switch (descriptor.kind) {
            case TypeKind::UnsignedInteger:
            case TypeKind::SignedInteger:
            case TypeKind::FloatingPoint: {
                const quint64 bytes = descriptor.bitWidth / 8;
                extent.minBytes = bytes;
                extent.maxBytes = bytes;
                extent.exactBytes = bytes;
                extent.fixedPrefixBytes = bytes;
                extent.parentAdvance = ParentAdvance::Contiguous;
                extent.mayFail = true;
                return extent;
            }
            case TypeKind::Enum:
            case TypeKind::Bitfield:
                return extentForType(descriptor.elementType);
            case TypeKind::Record: {
                const QString name = m_program->symbol(descriptor.name);
                const auto found = m_recordSyntax.constFind(name);
                if (found != m_recordSyntax.constEnd()) {
                    ensureRecordResolved(*found);
                }
                const auto extentId = m_typeExtents.constFind(type);
                return extentId != m_typeExtents.constEnd()
                           ? m_program->extents.at(*extentId)
                           : extent;
            }
            case TypeKind::Optional: {
                extent = extentForType(descriptor.elementType);
                extent.minBytes = 0;
                extent.exactBytes.reset();
                extent.fixedPrefixBytes = 0;
                return extent;
            }
            default: return extent;
        }
    }

    ParentAdvance mergeAdvance(ParentAdvance left, ParentAdvance right) const {
        if (left == ParentAdvance::None) {
            return right;
        }
        if (right == ParentAdvance::None) {
            return left;
        }
        if (left == right) {
            return left;
        }
        return ParentAdvance::MultiInput;
    }

    ExtentSummary appendExtent(const ExtentSummary& left,
                               const ExtentSummary& right) {
        ExtentSummary result;
        if (!checkedAdd(left.minBytes, right.minBytes, &result.minBytes)) {
            result.minBytes = std::numeric_limits<quint64>::max();
        }
        if (left.maxBytes.has_value() && right.maxBytes.has_value()) {
            quint64 maximum = 0;
            if (checkedAdd(*left.maxBytes, *right.maxBytes, &maximum)) {
                result.maxBytes = maximum;
            }
        }
        if (left.exactBytes.has_value() && right.exactBytes.has_value()) {
            quint64 exact = 0;
            if (checkedAdd(*left.exactBytes, *right.exactBytes, &exact)) {
                result.exactBytes = exact;
            }
        }
        result.fixedPrefixBytes = left.fixedPrefixBytes;
        if (left.exactBytes.has_value() &&
            left.fixedPrefixBytes == *left.exactBytes &&
            (left.parentAdvance == ParentAdvance::Contiguous ||
             left.parentAdvance == ParentAdvance::None) &&
            right.parentAdvance == ParentAdvance::Contiguous) {
            checkedAdd(left.fixedPrefixBytes, right.fixedPrefixBytes,
                       &result.fixedPrefixBytes);
        }
        result.parentAdvance = mergeAdvance(left.parentAdvance, right.parentAdvance);
        result.mayFail = left.mayFail || right.mayFail;
        result.mayNoMatch = left.mayNoMatch || right.mayNoMatch;
        result.requiresRandomAccess =
            left.requiresRandomAccess || right.requiresRandomAccess;
        return result;
    }

    ExtentSummary repeatedExtent(const ExtentSummary& body,
                                 std::optional<quint64> count) {
        ExtentSummary result = body;
        result.fixedPrefixBytes = 0;
        if (!count.has_value()) {
            result.minBytes = 0;
            result.maxBytes.reset();
            result.exactBytes.reset();
            return result;
        }
        quint64 value = 0;
        if (checkedMultiply(body.minBytes, *count, &value)) {
            result.minBytes = value;
        } else {
            result.minBytes = std::numeric_limits<quint64>::max();
        }
        if (body.maxBytes.has_value() &&
            checkedMultiply(*body.maxBytes, *count, &value)) {
            result.maxBytes = value;
        } else {
            result.maxBytes.reset();
        }
        if (body.exactBytes.has_value() &&
            checkedMultiply(*body.exactBytes, *count, &value)) {
            result.exactBytes = value;
            if (body.parentAdvance == ParentAdvance::Contiguous) {
                result.fixedPrefixBytes = value;
            }
        } else {
            result.exactBytes.reset();
        }
        return result;
    }

    ExtentSummary alternativeExtent(const QVector<ExtentSummary>& alternatives) {
        ExtentSummary result;
        if (alternatives.isEmpty()) {
            return result;
        }
        result = alternatives.first();
        for (qsizetype i = 1; i < alternatives.size(); ++i) {
            const ExtentSummary& next = alternatives.at(i);
            result.minBytes = qMin(result.minBytes, next.minBytes);
            if (result.maxBytes.has_value() && next.maxBytes.has_value()) {
                result.maxBytes = qMax(*result.maxBytes, *next.maxBytes);
            } else {
                result.maxBytes.reset();
            }
            if (!result.exactBytes.has_value() || !next.exactBytes.has_value() ||
                *result.exactBytes != *next.exactBytes) {
                result.exactBytes.reset();
            }
            result.fixedPrefixBytes = qMin(result.fixedPrefixBytes,
                                           next.fixedPrefixBytes);
            result.parentAdvance =
                mergeAdvance(result.parentAdvance, next.parentAdvance);
            result.mayFail = result.mayFail || next.mayFail;
            result.mayNoMatch = result.mayNoMatch || next.mayNoMatch;
            result.requiresRandomAccess =
                result.requiresRandomAccess || next.requiresRandomAccess;
        }
        return result;
    }

    void resolveRecords() {
        m_program->records.resize(m_syntax.records.size());
        for (qsizetype i = 0; i < m_syntax.records.size(); ++i) {
            ensureRecordResolved(i);
        }
    }

    void ensureRecordResolved(qsizetype index) {
        if (index < 0 || index >= m_syntax.records.size()) {
            return;
        }
        if (m_recordStatus.at(index) == 2) {
            return;
        }
        if (m_recordStatus.at(index) == 1) {
            return;
        }
        const TypeId recordType = m_recordTypeIds.value(index, kInvalidId);
        if (recordType == kInvalidId) {
            m_recordStatus[index] = 2;
            return;
        }
        m_recordStatus[index] = 1;
        const SyntaxRecord& syntaxRecord = m_syntax.records.at(index);
        ResolveContext context;
        context.scopes.push_back({});

        const quint32 firstParameter =
            static_cast<quint32>(m_program->parameters.size());
        QSet<QString> parameterNames;
        for (const SyntaxParameter& syntaxParameter : syntaxRecord.parameters) {
            if (parameterNames.contains(syntaxParameter.name)) {
                report(QStringLiteral("BR0400"),
                       QStringLiteral("Duplicate parameter '%1'")
                           .arg(syntaxParameter.name),
                       syntaxParameter.nameSpan);
                continue;
            }
            parameterNames.insert(syntaxParameter.name);
            const TypeId type = resolveType(syntaxParameter.type);
            const quint32 slot = context.nextSlot++;
            context.scopes.last().insert(syntaxParameter.name, {type, slot});
            m_program->parameters.push_back(
                {intern(syntaxParameter.name), type, slot});
        }

        ResolvedBlock block =
            resolveBlock(syntaxRecord.statements, &context, recordType);
        finalizeTypeFields(recordType);
        const quint32 extentId = addExtent(block.extent);
        m_typeExtents.insert(recordType, extentId);

        RecordDesc record;
        record.name = intern(syntaxRecord.name);
        record.type = recordType;
        record.parameters =
            {firstParameter,
             static_cast<quint32>(m_program->parameters.size()) - firstParameter};
        record.statements = appendRefs(&m_program->statementRefs, block.statements);
        record.slotCount = context.nextSlot;
        record.extent = extentId;
        m_program->records[index] = record;
        m_recordStatus[index] = 2;
    }

    void resolveEntries() {
        for (qsizetype index = 0; index < m_syntax.entries.size(); ++index) {
            const SyntaxEntry& syntaxEntry = m_syntax.entries.at(index);
            const TypeId resultType = m_entryTypeIds.value(index, kInvalidId);
            ResolveContext context;
            context.scopes.push_back({});
            ResolvedBlock block =
                resolveBlock(syntaxEntry.statements, &context, resultType);
            finalizeTypeFields(resultType);
            const quint32 extentId = addExtent(block.extent);
            m_typeExtents.insert(resultType, extentId);

            EntryDesc entry;
            entry.name = intern(syntaxEntry.name);
            entry.resultType = resultType;
            const auto input = m_inputIds.constFind(syntaxEntry.inputName);
            if (input == m_inputIds.constEnd()) {
                report(QStringLiteral("BR0410"),
                       QStringLiteral("Unknown entry input '%1'")
                           .arg(syntaxEntry.inputName),
                       syntaxEntry.inputSpan);
            } else {
                entry.input = *input;
            }
            entry.statements =
                appendRefs(&m_program->statementRefs, block.statements);
            entry.slotCount = context.nextSlot;
            entry.extent = extentId;
            m_program->entries.push_back(entry);
        }
    }

    void resolveOutforms() {
        for (const SyntaxOutform& syntaxOutform : m_syntax.outforms) {
            OutformDesc outform;
            outform.name = intern(syntaxOutform.name);
            outform.parameterName = intern(syntaxOutform.parameterName);
            outform.targetType = resolveType(syntaxOutform.targetType);
            outform.mode = syntaxOutform.mode;
            outform.sourceSpan = addSpan(syntaxOutform.nameSpan);
            outform.sourcePath = intern(m_syntax.sourcePath);

            ResolveContext context;
            context.outform = true;
            context.outformMode = syntaxOutform.mode;
            context.scopes.push_back({});
            context.scopes.last().insert(
                syntaxOutform.parameterName,
                {outform.targetType, context.nextSlot++});
            ResolvedBlock block =
                resolveBlock(syntaxOutform.statements, &context, kInvalidId);
            outform.statements =
                appendRefs(&m_program->statementRefs, block.statements);
            outform.slotCount = context.nextSlot;
            m_program->outforms.push_back(outform);
        }
    }

    void resolveDefaultEntry() {
        if (m_syntax.defaultEntry.isEmpty()) {
            return;
        }
        for (const EntryDesc& entry : m_program->entries) {
            if (m_program->symbol(entry.name) == m_syntax.defaultEntry) {
                m_program->defaultEntry = entry.name;
                return;
            }
        }
        report(QStringLiteral("BR0420"),
               QStringLiteral("Unknown default entry '%1'")
                   .arg(m_syntax.defaultEntry),
               m_syntax.defaultEntrySpan);
    }

    ResolvedBlock resolveBlock(const QVector<SyntaxStatementId>& syntaxStatements,
                               ResolveContext* context, TypeId ownerType) {
        ResolvedBlock block;
        block.extent.maxBytes = 0;
        block.extent.exactBytes = 0;
        for (SyntaxStatementId syntaxStatement : syntaxStatements) {
            ExtentSummary statementExtent;
            const StatementId statement =
                resolveStatement(syntaxStatement, context, ownerType,
                                 &statementExtent);
            if (statement != kInvalidId) {
                block.statements.push_back(statement);
                block.extent = appendExtent(block.extent, statementExtent);
            }
        }
        return block;
    }

    StatementKind resolvedKind(SyntaxStatementKind kind) const {
        return static_cast<StatementKind>(kind);
    }

    StatementId resolveStatement(SyntaxStatementId syntaxId,
                                 ResolveContext* context, TypeId ownerType,
                                 ExtentSummary* extentOut) {
        if (syntaxId >= static_cast<SyntaxStatementId>(m_syntax.statements.size())) {
            return kInvalidId;
        }
        const SyntaxStatement& syntax = m_syntax.statements.at(syntaxId);
        Statement statement;
        statement.kind = resolvedKind(syntax.kind);
        statement.sourceSpan = addSpan(syntax.span);
        statement.name = syntax.name.isEmpty() ? kInvalidId : intern(syntax.name);
        statement.message = syntax.message.isEmpty() ? kInvalidId : intern(syntax.message);
        statement.gapsName = syntax.gapsName.isEmpty() ? kInvalidId : intern(syntax.gapsName);
        statement.stepBytes = syntax.stepBytes;
        const StatementId statementId =
            static_cast<StatementId>(m_program->statements.size());
        m_program->statements.push_back(statement);

        ExtentSummary extent;
        extent.maxBytes = 0;
        extent.exactBytes = 0;
        TypeId fieldType = kInvalidId;
        QVector<ExpressionId> typeArguments;
        for (SyntaxExpressionId argument : syntax.type.arguments) {
            typeArguments.push_back(resolveExpression(argument, context));
        }
        statement.arguments = appendRefs(&m_program->expressionRefs, typeArguments);

        switch (syntax.kind) {
            case SyntaxStatementKind::Field: {
                fieldType = resolveType(syntax.type);
                extent = extentForType(fieldType);
                if (syntax.expression != kInvalidSyntaxExpression) {
                    statement.expression = resolveExpression(syntax.expression, context);
                }
                if (syntax.condition != kInvalidSyntaxExpression) {
                    statement.condition = resolveBoolean(syntax.condition, context,
                                                         QStringLiteral("field condition"));
                    extent.minBytes = 0;
                    extent.exactBytes.reset();
                    extent.fixedPrefixBytes = 0;
                }
                if (syntax.secondaryExpression != kInvalidSyntaxExpression) {
                    statement.secondaryExpression =
                        resolveExpression(syntax.secondaryExpression, context);
                }
                if (!syntax.sourceInput.isEmpty()) {
                    const auto input = m_inputIds.constFind(syntax.sourceInput);
                    if (input == m_inputIds.constEnd()) {
                        report(QStringLiteral("BR0500"),
                               QStringLiteral("Unknown input '%1'")
                                   .arg(syntax.sourceInput),
                               syntax.sourceInputSpan);
                    } else {
                        statement.input = *input;
                    }
                    extent.minBytes = 0;
                    extent.maxBytes = 0;
                    extent.exactBytes = 0;
                    extent.fixedPrefixBytes = 0;
                    extent.parentAdvance = ParentAdvance::ExternalInput;
                    extent.requiresRandomAccess = true;
                }
                break;
            }
            case SyntaxStatementKind::ComputedField:
                fieldType = resolveType(syntax.type);
                statement.expression = resolveExpression(syntax.expression, context);
                checkAssignable(fieldType, expressionType(statement.expression),
                                syntax.span);
                break;
            case SyntaxStatementKind::BitfieldField:
                fieldType = resolveBitfieldType(syntax, context, &statement);
                extent = extentForType(fieldType);
                break;
            case SyntaxStatementKind::Identify: {
                ResolveContext child = *context;
                ++child.identifyDepth;
                ResolvedBlock body = resolveBlock(syntax.statements, &child, ownerType);
                context->nextSlot = qMax(context->nextSlot, child.nextSlot);
                context->scopes = child.scopes;
                context->hasIdentified = true;
                statement.statements =
                    appendRefs(&m_program->statementRefs, body.statements);
                extent = body.extent;
                extent.mayNoMatch = true;
                break;
            }
            case SyntaxStatementKind::Commit:
                if (!context->hasIdentified) {
                    warn(QStringLiteral("BR1500"),
                         QStringLiteral("commit has no enclosing identify block"),
                         syntax.span);
                }
                break;
            case SyntaxStatementKind::Require:
            case SyntaxStatementKind::Check:
            case SyntaxStatementKind::Match:
                statement.expression = resolveBoolean(
                    syntax.expression, context, QStringLiteral("validation expression"));
                extent.mayFail = syntax.kind != SyntaxStatementKind::Check;
                extent.mayNoMatch = syntax.kind == SyntaxStatementKind::Match ||
                                    context->identifyDepth > 0;
                if (syntax.kind == SyntaxStatementKind::Match &&
                    context->identifyDepth == 0) {
                    report(QStringLiteral("BR0501"),
                           QStringLiteral("match is only valid inside identify"),
                           syntax.span);
                }
                break;
            case SyntaxStatementKind::Preserve:
            case SyntaxStatementKind::Raw:
                fieldType = m_bytesType;
                extent.maxBytes.reset();
                extent.exactBytes.reset();
                extent.parentAdvance = ParentAdvance::Contiguous;
                break;
            case SyntaxStatementKind::Region: {
                statement.expression = resolveExpression(syntax.expression, context);
                TypeId shape = newShape(syntax.name + QStringLiteral("$region"));
                ResolveContext child = *context;
                child.scopes.push_back({});
                ResolvedBlock body = resolveBlock(syntax.statements, &child, shape);
                context->nextSlot = qMax(context->nextSlot, child.nextSlot);
                finalizeTypeFields(shape);
                statement.statements =
                    appendRefs(&m_program->statementRefs, body.statements);
                fieldType = shape;
                const auto size = constantInteger(syntax.expression);
                if (size.has_value()) {
                    extent.minBytes = *size;
                    extent.maxBytes = *size;
                    extent.exactBytes = *size;
                    extent.fixedPrefixBytes = *size;
                }
                extent.parentAdvance = ParentAdvance::Contiguous;
                extent.mayFail = true;
                extent.requiresRandomAccess = body.extent.requiresRandomAccess;
                break;
            }
            case SyntaxStatementKind::Repeat:
            case SyntaxStatementKind::While: {
                const bool isWhile = syntax.kind == SyntaxStatementKind::While;
                if (syntax.expression != kInvalidSyntaxExpression) {
                    statement.expression = resolveExpression(syntax.expression, context);
                }
                ResolveContext child = *context;
                child.scopes.push_back({});
                ++child.loopDepth;
                statement.iterationSlot = child.nextSlot++;
                child.scopes.last().insert(QStringLiteral("iteration"),
                                           {m_u64Type, statement.iterationSlot});
                QVector<StatementId> initializers;
                for (SyntaxStatementId initializer : syntax.initializers) {
                    ExtentSummary ignored;
                    initializers.push_back(resolveStatement(initializer, &child,
                                                            kInvalidId, &ignored));
                }
                statement.initializers =
                    appendRefs(&m_program->statementRefs, initializers);
                if (syntax.condition != kInvalidSyntaxExpression) {
                    statement.condition =
                        isWhile
                            ? resolveBoolean(syntax.condition, &child,
                                             QStringLiteral("while condition"))
                            : resolveBoolean(syntax.condition, &child,
                                             QStringLiteral("until condition"));
                }
                const TypeId shape = newShape(syntax.name + QStringLiteral("$item"));
                ResolvedBlock body = resolveBlock(syntax.statements, &child, shape);
                context->nextSlot = qMax(context->nextSlot, child.nextSlot);
                finalizeTypeFields(shape);
                statement.statements =
                    appendRefs(&m_program->statementRefs, body.statements);
                fieldType = sequenceType(shape);
                const std::optional<quint64> count =
                    !isWhile && syntax.expression != kInvalidSyntaxExpression
                        ? constantInteger(syntax.expression)
                        : std::nullopt;
                extent = repeatedExtent(body.extent, count);
                if (syntax.condition != kInvalidSyntaxExpression || isWhile) {
                    extent.minBytes = 0;
                    extent.maxBytes.reset();
                    extent.exactBytes.reset();
                    extent.fixedPrefixBytes = 0;
                }
                break;
            }
            case SyntaxStatementKind::Many:
                fieldType = sequenceType(resolveType(syntax.type));
                extent = repeatedExtent(extentForType(
                                            m_program->types.at(fieldType).elementType),
                                        std::nullopt);
                extent.mayNoMatch = true;
                break;
            case SyntaxStatementKind::Select:
                fieldType = resolveSelect(syntax, context, &statement, &extent);
                break;
            case SyntaxStatementKind::OneOf:
                fieldType = resolveOneOf(syntax, context, &statement, &extent);
                break;
            case SyntaxStatementKind::Recover:
                fieldType = resolveRecover(syntax, context, &statement, &extent);
                break;
            case SyntaxStatementKind::Continue:
            case SyntaxStatementKind::Break:
                if (context->loopDepth == 0) {
                    report(QStringLiteral("BR0502"),
                           QStringLiteral("Loop control is only valid inside a loop"),
                           syntax.span);
                }
                if (syntax.condition != kInvalidSyntaxExpression) {
                    statement.condition = resolveBoolean(
                        syntax.condition, context,
                        QStringLiteral("loop-control condition"));
                }
                break;
            case SyntaxStatementKind::Emit:
                statement.expression = resolveExpression(syntax.expression, context);
                validateEmit(expressionType(statement.expression), context, syntax.span);
                break;
            case SyntaxStatementKind::Let:
                statement.expression = resolveExpression(syntax.expression, context);
                fieldType = syntax.type.name.isEmpty() ? expressionType(statement.expression)
                                                       : resolveType(syntax.type);
                checkAssignable(fieldType, expressionType(statement.expression),
                                syntax.span);
                statement.type = fieldType;
                statement.resultSlot =
                    addBinding(context, syntax.name, fieldType, syntax.nameSpan);
                break;
            case SyntaxStatementKind::If: {
                statement.condition = resolveBoolean(syntax.condition, context,
                                                     QStringLiteral("if condition"));
                ResolveContext thenContext = *context;
                thenContext.scopes.push_back({});
                ResolvedBlock thenBlock =
                    resolveBlock(syntax.statements, &thenContext, kInvalidId);
                ResolveContext elseContext = *context;
                elseContext.scopes.push_back({});
                ResolvedBlock elseBlock =
                    resolveBlock(syntax.elseStatements, &elseContext, kInvalidId);
                context->nextSlot = qMax(
                    context->nextSlot,
                    qMax(thenContext.nextSlot, elseContext.nextSlot));
                statement.statements =
                    appendRefs(&m_program->statementRefs, thenBlock.statements);
                statement.elseStatements =
                    appendRefs(&m_program->statementRefs, elseBlock.statements);
                break;
            }
            case SyntaxStatementKind::For:
                resolveOutformFor(syntax, context, &statement);
                break;
            case SyntaxStatementKind::Invalid: break;
        }

        statement.type = fieldType;
        if (syntax.kind == SyntaxStatementKind::Field ||
            syntax.kind == SyntaxStatementKind::ComputedField ||
            syntax.kind == SyntaxStatementKind::BitfieldField ||
            syntax.kind == SyntaxStatementKind::Preserve ||
            syntax.kind == SyntaxStatementKind::Raw ||
            syntax.kind == SyntaxStatementKind::Region ||
            syntax.kind == SyntaxStatementKind::Repeat ||
            syntax.kind == SyntaxStatementKind::While ||
            syntax.kind == SyntaxStatementKind::Many ||
            syntax.kind == SyntaxStatementKind::Select ||
            syntax.kind == SyntaxStatementKind::OneOf ||
            syntax.kind == SyntaxStatementKind::Recover) {
            const bool optional = syntax.condition != kInvalidSyntaxExpression &&
                                  syntax.kind == SyntaxStatementKind::Field;
            addPendingField(ownerType, syntax.name, fieldType, statementId, optional);
            statement.resultSlot = addBinding(
                context, syntax.name,
                optional ? optionalType(fieldType) : fieldType,
                syntax.nameSpan);
        }

        statement.extent = addExtent(extent);
        m_program->statements[statementId] = statement;
        *extentOut = extent;
        return statementId;
    }

    quint32 addBinding(ResolveContext* context, const QString& name, TypeId type,
                       SourceSpan span) {
        if (name.isEmpty()) {
            return kInvalidId;
        }
        if (context->scopes.isEmpty()) {
            context->scopes.push_back({});
        }
        if (context->scopes.last().contains(name)) {
            report(QStringLiteral("BR0510"),
                   QStringLiteral("Duplicate local or field name '%1'").arg(name), span);
            return kInvalidId;
        }
        const quint32 slot = context->nextSlot++;
        context->scopes.last().insert(name, {type, slot});
        return slot;
    }

    ExpressionId resolveBoolean(SyntaxExpressionId syntaxExpression,
                                ResolveContext* context,
                                const QString& description) {
        const ExpressionId expression =
            resolveExpression(syntaxExpression, context);
        if (!isBooleanType(expressionType(expression))) {
            report(QStringLiteral("BR0511"),
                   QStringLiteral("%1 must be Boolean").arg(description),
                   m_syntax.expressions.at(syntaxExpression).span);
        }
        return expression;
    }

    void checkAssignable(TypeId target, TypeId value, SourceSpan span) {
        if (target == kInvalidId || value == kInvalidId || target == value) {
            return;
        }
        if (isNumericType(target) && isNumericType(value)) {
            return;
        }
        report(QStringLiteral("BR0512"),
               QStringLiteral("Expression type is not assignable to declared type"), span);
    }

    void validateEmit(TypeId type, const ResolveContext* context, SourceSpan span) {
        if (!context->outform) {
            report(QStringLiteral("BR0513"),
                   QStringLiteral("emit is only valid inside an outform"), span);
            return;
        }
        if (context->outformMode == OutformMode::Text) {
            if (type == m_bytesType || type == m_nodeType ||
                (type != kInvalidId &&
                 (m_program->types.at(type).kind == TypeKind::Record ||
                  m_program->types.at(type).kind == TypeKind::Shape ||
                  m_program->types.at(type).kind == TypeKind::Sequence))) {
                report(QStringLiteral("BR0514"),
                       QStringLiteral("Text outform emit requires text or a scalar value"),
                       span);
            }
        } else if (type != m_bytesType && type != kInvalidId) {
            report(QStringLiteral("BR0515"),
                   QStringLiteral("Binary outform emit requires bytes"), span);
        }
    }

    TypeId resolveBitfieldType(const SyntaxStatement& syntax,
                               ResolveContext* context, Statement* statement) {
        const TypeId storage = resolveType(syntax.type);
        if (!isIntegerType(storage)) {
            report(QStringLiteral("BR0520"),
                   QStringLiteral("Bitfield storage type must be an integer"),
                   syntax.type.span);
        }
        TypeDesc type;
        type.kind = TypeKind::Bitfield;
        type.name = intern(syntax.name + QStringLiteral("$bitfield"));
        type.elementType = storage;
        const TypeId bitfieldType = addType(type);
        QVector<quint32> bitMemberIds;
        for (const SyntaxBitMember& syntaxMember : syntax.bitMembers) {
            const auto high = constantInteger(syntaxMember.highBit);
            const auto low = constantInteger(syntaxMember.lowBit);
            const quint64 width = storage != kInvalidId
                                      ? m_program->types.at(storage).bitWidth
                                      : 0;
            if (!high.has_value() || !low.has_value() || *high < *low ||
                *high >= width) {
                report(QStringLiteral("BR0521"),
                       QStringLiteral("Invalid bitfield range"), syntaxMember.span);
                continue;
            }
            BitMember member;
            member.name = intern(syntaxMember.name);
            member.sourceSpan = addSpan(syntaxMember.span);
            member.highBit = static_cast<quint8>(*high);
            member.lowBit = static_cast<quint8>(*low);
            const quint32 memberId =
                static_cast<quint32>(m_program->bitMembers.size());
            m_program->bitMembers.push_back(member);
            bitMemberIds.push_back(memberId);

            const TypeId memberType = *high == *low ? m_boolType : m_u64Type;
            FieldDesc field;
            field.name = member.name;
            field.type = memberType;
            field.statement = static_cast<StatementId>(m_program->statements.size() - 1);
            const quint32 fieldId = static_cast<quint32>(m_program->fields.size());
            m_program->fields.push_back(field);
            m_pendingFields[bitfieldType].push_back(fieldId);
        }
        statement->bitMembers =
            appendRefs(&m_program->bitMemberRefs, bitMemberIds);
        finalizeTypeFields(bitfieldType);
        Q_UNUSED(context);
        return bitfieldType;
    }

    TypeId resolveSelect(const SyntaxStatement& syntax, ResolveContext* context,
                         Statement* statement, ExtentSummary* extent) {
        if (syntax.expression != kInvalidSyntaxExpression) {
            statement->expression = resolveExpression(syntax.expression, context);
        }
        QVector<SelectCase> cases;
        QVector<TypeId> alternativeTypes;
        QVector<ExtentSummary> alternativeExtents;
        bool hasDefault = false;
        for (const SyntaxSelectCase& syntaxCase : syntax.selectCases) {
            SelectCase selectCase;
            selectCase.sourceSpan = addSpan(syntaxCase.span);
            selectCase.isDefault = syntaxCase.isDefault;
            selectCase.isConditional = syntaxCase.isConditional;
            hasDefault = hasDefault || selectCase.isDefault;
            if (syntaxCase.expression != kInvalidSyntaxExpression) {
                selectCase.expression = syntaxCase.isConditional
                                            ? resolveBoolean(syntaxCase.expression, context,
                                                             QStringLiteral("select condition"))
                                            : resolveExpression(syntaxCase.expression, context);
            }
            if (syntaxCase.isType) {
                selectCase.resultType = resolveType(syntaxCase.type);
                QVector<ExpressionId> arguments;
                for (SyntaxExpressionId argument : syntaxCase.type.arguments) {
                    arguments.push_back(resolveExpression(argument, context));
                }
                selectCase.arguments =
                    appendRefs(&m_program->expressionRefs, arguments);
                alternativeTypes.push_back(selectCase.resultType);
                alternativeExtents.push_back(extentForType(selectCase.resultType));
            } else {
                ResolveContext branch = *context;
                branch.scopes.push_back({});
                const TypeId shape = newShape(syntax.name + QStringLiteral("$case"));
                ResolvedBlock body = resolveBlock(syntaxCase.statements, &branch, shape);
                context->nextSlot = qMax(context->nextSlot, branch.nextSlot);
                finalizeTypeFields(shape);
                selectCase.resultType = shape;
                selectCase.statements =
                    appendRefs(&m_program->statementRefs, body.statements);
                alternativeTypes.push_back(shape);
                alternativeExtents.push_back(body.extent);
            }
            cases.push_back(selectCase);
        }
        if (!hasDefault) {
            warn(QStringLiteral("BR1520"),
                 QStringLiteral("select has no default or else branch"), syntax.span);
        }
        const quint32 first = static_cast<quint32>(m_program->selectCases.size());
        m_program->selectCases += cases;
        statement->selectCases =
            {first, static_cast<quint32>(m_program->selectCases.size()) - first};
        *extent = alternativeExtent(alternativeExtents);
        return variantType(alternativeTypes);
    }

    TypeId resolveOneOf(const SyntaxStatement& syntax, ResolveContext* context,
                        Statement* statement, ExtentSummary* extent) {
        QVector<Alternative> alternatives;
        QVector<TypeId> alternativeTypes;
        QVector<ExtentSummary> alternativeExtents;
        for (const SyntaxAlternative& syntaxAlternative : syntax.alternatives) {
            Alternative alternative;
            alternative.sourceSpan = addSpan(syntaxAlternative.span);
            alternative.type = resolveType(syntaxAlternative.type);
            QVector<ExpressionId> arguments;
            for (SyntaxExpressionId argument : syntaxAlternative.type.arguments) {
                arguments.push_back(resolveExpression(argument, context));
            }
            alternative.arguments =
                appendRefs(&m_program->expressionRefs, arguments);
            alternatives.push_back(alternative);
            alternativeTypes.push_back(alternative.type);
            ExtentSummary branchExtent = extentForType(alternative.type);
            branchExtent.mayNoMatch = true;
            alternativeExtents.push_back(branchExtent);
        }
        const quint32 first = static_cast<quint32>(m_program->alternatives.size());
        m_program->alternatives += alternatives;
        statement->alternatives =
            {first, static_cast<quint32>(m_program->alternatives.size()) - first};
        *extent = alternativeExtent(alternativeExtents);
        extent->mayNoMatch = true;
        return variantType(alternativeTypes);
    }

    TypeId resolveRecover(const SyntaxStatement& syntax, ResolveContext* context,
                          Statement* statement, ExtentSummary* extent) {
        const TypeId itemType = resolveType(syntax.type);
        if (syntax.condition != kInvalidSyntaxExpression) {
            statement->condition = resolveExpression(syntax.condition, context);
            const auto step = constantInteger(syntax.condition);
            if (!step.has_value() || *step == 0) {
                report(QStringLiteral("BR0530"),
                       QStringLiteral("Recovery step must be a positive constant"),
                       m_syntax.expressions.at(syntax.condition).span);
            } else {
                statement->stepBytes = *step;
            }
        }
        if (syntax.secondaryExpression != kInvalidSyntaxExpression) {
            statement->secondaryExpression =
                resolveExpression(syntax.secondaryExpression, context);
            const auto maxProbe = constantInteger(syntax.secondaryExpression);
            if (!maxProbe.has_value() || *maxProbe == 0) {
                report(QStringLiteral("BR0531"),
                       QStringLiteral("Recovery max_probe must be a positive constant"),
                       m_syntax.expressions.at(syntax.secondaryExpression).span);
            }
        }
        QVector<quint32> patterns;
        for (const SyntaxBytePattern& syntaxPattern : syntax.syncPatterns) {
            BytePattern pattern;
            pattern.sourceSpan = addSpan(syntaxPattern.span);
            for (SyntaxExpressionId syntaxByte : syntaxPattern.bytes) {
                const auto value = constantInteger(syntaxByte);
                if (!value.has_value() || *value > 255) {
                    report(QStringLiteral("BR0532"),
                           QStringLiteral("Sync bytes must be constants from 0 to 255"),
                           m_syntax.expressions.at(syntaxByte).span);
                } else {
                    pattern.bytes.push_back(static_cast<char>(*value));
                }
            }
            if (pattern.bytes.isEmpty()) {
                report(QStringLiteral("BR0533"),
                       QStringLiteral("Sync byte pattern may not be empty"),
                       syntaxPattern.span);
            }
            patterns.push_back(static_cast<quint32>(m_program->bytePatterns.size()));
            m_program->bytePatterns.push_back(std::move(pattern));
        }
        statement->bytePatterns =
            appendRefs(&m_program->bytePatternRefs, patterns);
        if (syntax.syncPatterns.isEmpty()) {
            report(QStringLiteral("BR0534"),
                   QStringLiteral("recover requires at least one sync pattern"),
                   syntax.span);
        }
        *extent = repeatedExtent(extentForType(itemType), std::nullopt);
        extent->mayFail = true;
        extent->requiresRandomAccess = true;
        return sequenceType(itemType);
    }

    void resolveOutformFor(const SyntaxStatement& syntax,
                           ResolveContext* context, Statement* statement) {
        statement->expression = resolveExpression(syntax.expression, context);
        TypeId iterable = expressionType(statement->expression);
        TypeId element = kInvalidId;
        if (iterable != kInvalidId) {
            const TypeDesc& type = m_program->types.at(iterable);
            if (type.kind == TypeKind::Optional) {
                iterable = type.elementType;
            }
            if (iterable != kInvalidId &&
                m_program->types.at(iterable).kind == TypeKind::Sequence) {
                element = m_program->types.at(iterable).elementType;
            }
        }
        if (element == kInvalidId) {
            report(QStringLiteral("BR0540"),
                   QStringLiteral("for loop expression must be a sequence"),
                   syntax.span);
            element = m_nodeType;
        }
        ResolveContext child = *context;
        child.scopes.push_back({});
        statement->iterationSlot =
            addBinding(&child, syntax.name, element, syntax.nameSpan);
        if (!syntax.secondaryName.isEmpty()) {
            statement->secondarySlot =
                addBinding(&child, syntax.secondaryName, m_u64Type,
                           syntax.secondaryNameSpan);
        }
        ++child.loopDepth;
        ResolvedBlock body = resolveBlock(syntax.statements, &child, kInvalidId);
        context->nextSlot = qMax(context->nextSlot, child.nextSlot);
        statement->statements =
            appendRefs(&m_program->statementRefs, body.statements);
    }

    const SyntaxFile& m_syntax;
    QVector<Diagnostic> m_diagnostics;
    std::shared_ptr<BrecoProgram> m_program;
    QHash<QString, SymbolId> m_symbolIds;
    QHash<QString, TypeId> m_typeIds;
    QHash<QString, SourceSpan> m_topLevelSpans;
    QHash<QString, QString> m_topLevelKinds;
    QHash<QString, SourceSpan> m_outformSpans;
    QHash<QString, qsizetype> m_constantSyntax;
    QHash<QString, qsizetype> m_enumSyntax;
    QHash<QString, qsizetype> m_recordSyntax;
    QHash<QString, qsizetype> m_entrySyntax;
    QHash<QString, InputId> m_inputIds;
    QHash<QString, quint32> m_constantIds;
    QHash<QString, quint64> m_constantValues;
    QHash<QString, QPair<TypeId, quint64>> m_enumValues;
    QHash<TypeId, TypeId> m_sequenceTypes;
    QHash<TypeId, TypeId> m_optionalTypes;
    QHash<TypeId, quint32> m_typeExtents;
    QVector<QVector<quint32>> m_pendingFields;
    QVector<int> m_recordStatus;
    QVector<TypeId> m_recordTypeIds;
    QVector<TypeId> m_entryTypeIds;
    QVector<quint32> m_recordDescriptors;

    TypeId m_voidType = kInvalidId;
    TypeId m_boolType = kInvalidId;
    TypeId m_stringType = kInvalidId;
    TypeId m_bytesType = kInvalidId;
    TypeId m_nodeType = kInvalidId;
    TypeId m_spanType = kInvalidId;
    TypeId m_u64Type = kInvalidId;
    TypeId m_i64Type = kInvalidId;
};

}  // namespace

CompileResult compileBrecoLang(QStringView source, const QString& sourcePath) {
    ParseSyntaxResult parsed = parseBrecoLang(source, sourcePath);
    return Resolver(parsed.syntax, std::move(parsed.diagnostics)).run();
}

}  // namespace breco::lang

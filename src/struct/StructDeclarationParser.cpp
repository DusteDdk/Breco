#include "struct/StructDeclarationParser.h"

#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <limits>
#include <utility>

namespace breco {

namespace {

class Parser {
public:
    explicit Parser(const QString& text, QString sourceFilePath = {})
        : m_text(text), m_currentSourceFilePath(std::move(sourceFilePath)) {}

    ParseResult parse() {
        ParseResult result;
        skipWhitespaceAndComments();
        if (atEnd()) {
            result.valid = false;
            result.errorMessage.clear();
            return result;
        }
        while (!atEnd()) {
            if (!parseTopLevelDeclaration(result)) {
                return result;
            }
            skipWhitespaceAndComments();
        }
        if (result.graph.entryNames().isEmpty()) {
            result.valid = false;
            setError(result, QStringLiteral("Declaration must name at least one struct or member"),
                     0, m_text.size());
            return result;
        }
        const QString defaultEntryName = result.graph.defaultEntryName();
        if (!defaultEntryName.isEmpty() &&
            !result.graph.isVisualizableEntryName(defaultEntryName)) {
            result.valid = false;
            setError(result,
                     QStringLiteral("Default entry '%1' is not available")
                         .arg(defaultEntryName),
                     m_defaultEntryRange.start, m_defaultEntryRange.end);
            return result;
        }
        for (const StructNode& node : result.graph.structs()) {
            for (const StructMember& member : node.members) {
                if (!member.attributes.sourceRole.isEmpty() &&
                    !result.graph.externalRoles().contains(member.attributes.sourceRole)) {
                    result.valid = false;
                    setError(result,
                             QStringLiteral("External source role '%1' is not declared")
                                 .arg(member.attributes.sourceRole),
                             member.nameRange.start, member.nameRange.end);
                    return result;
                }
            }
        }
        for (const StandaloneMemberNode& member : result.graph.standaloneMembers()) {
            if (!member.attributes.sourceRole.isEmpty() &&
                !result.graph.externalRoles().contains(member.attributes.sourceRole)) {
                result.valid = false;
                setError(result,
                         QStringLiteral("External source role '%1' is not declared")
                             .arg(member.attributes.sourceRole),
                         member.nameRange.start, member.nameRange.end);
                return result;
            }
        }
        result.valid = true;
        return result;
    }

private:
    TextRange makeRange(int start, int end) const {
        if (start < 0) {
            start = 0;
        }
        if (start > m_text.size()) {
            start = m_text.size();
        }
        if (end < start) {
            end = start;
        }
        if (end > m_text.size()) {
            end = m_text.size();
        }
        if (end == start && start < m_text.size()) {
            ++end;
        }
        return {start, end};
    }

    void setError(ParseResult& result, const QString& message, int start, int end) const {
        result.errorMessage = message;
        result.errorRange = makeRange(start, end);
    }

    bool atEnd() const { return m_pos >= m_text.size(); }
    QChar peek() const { return atEnd() ? QChar() : m_text.at(m_pos); }
    QChar consume() { return atEnd() ? QChar() : m_text.at(m_pos++); }

    void skipWhitespaceAndComments() {
        while (!atEnd()) {
            const QChar ch = peek();
            if (ch.isSpace()) {
                ++m_pos;
                continue;
            }
            if (peekAhead(QStringLiteral("//"))) {
                while (!atEnd() && peek() != QLatin1Char('\n')) {
                    ++m_pos;
                }
                continue;
            }
            if (peekAhead(QStringLiteral("/*"))) {
                ++m_pos;
                ++m_pos;
                while (!atEnd()) {
                    if (peekAhead(QStringLiteral("*/"))) {
                        ++m_pos;
                        ++m_pos;
                        break;
                    }
                    ++m_pos;
                }
                continue;
            }
            break;
        }
    }

    bool peekAhead(const QString& token) const {
        return m_text.mid(m_pos, token.size()) == token;
    }

    bool consumeKeyword(const QString& keyword) {
        skipWhitespaceAndComments();
        if (!peekAhead(keyword)) {
            return false;
        }
        const QChar after = m_pos + keyword.size() < m_text.size()
                                ? m_text.at(m_pos + keyword.size())
                                : QChar();
        if (!after.isNull() && (after.isLetterOrNumber() || after == QLatin1Char('_'))) {
            return false;
        }
        m_pos += keyword.size();
        return true;
    }

    bool parseIdentifier(QString* out, TextRange* nameRange = nullptr) {
        skipWhitespaceAndComments();
        if (atEnd() || !isIdentifierStart(peek())) {
            return false;
        }
        const int start = m_pos;
        ++m_pos;
        while (!atEnd() && isIdentifierPart(peek())) {
            ++m_pos;
        }
        *out = m_text.mid(start, m_pos - start);
        if (nameRange != nullptr) {
            *nameRange = {start, m_pos};
        }
        return true;
    }

    bool parseDecoration(Endianness* endianness, QString* decoration) {
        skipWhitespaceAndComments();
        if (peek() != QLatin1Char('<')) {
            return true;
        }
        ++m_pos;
        QString tag;
        while (!atEnd() && peek() != QLatin1Char('>')) {
            tag += consume();
        }
        if (atEnd() || peek() != QLatin1Char('>')) {
            return false;
        }
        ++m_pos;
        tag = tag.trimmed();
        if (tag == QStringLiteral("le")) {
            *endianness = Endianness::Little;
            *decoration = QStringLiteral("<le>");
        } else if (tag == QStringLiteral("be")) {
            *endianness = Endianness::Big;
            *decoration = QStringLiteral("<be>");
        } else {
            return false;
        }
        return true;
    }

    bool parsePostfixDecoration(Endianness* endianness, QString* decoration) {
        skipWhitespaceAndComments();
        if (peek() != QLatin1Char('<')) {
            return true;
        }
        const int saved = m_pos;
        ++m_pos;
        QString tag;
        while (!atEnd() && peek() != QLatin1Char('>')) {
            tag += consume();
        }
        if (atEnd() || peek() != QLatin1Char('>')) {
            return false;
        }
        ++m_pos;
        tag = tag.trimmed();
        if (tag == QStringLiteral("le")) {
            *endianness = Endianness::Little;
            *decoration = QStringLiteral("<le>");
        } else if (tag == QStringLiteral("be")) {
            *endianness = Endianness::Big;
            *decoration = QStringLiteral("<be>");
        } else {
            m_pos = saved;
            return true;
        }
        return true;
    }

    bool parseTypeSpecifier(StructureGraph& graph, ResolvedType* out, QString* displayName,
                            QString* attemptedTypeName = nullptr,
                            TextRange* typeRange = nullptr) {
        skipWhitespaceAndComments();
        const int typeStart = m_pos;
        Endianness endianness = Endianness::Native;
        QString decoration;
        if (!parseDecoration(&endianness, &decoration)) {
            if (typeRange != nullptr) {
                *typeRange = makeRange(typeStart, m_pos);
            }
            return false;
        }
        QString typeName;
        TextRange identifierRange;
        if (!parseIdentifier(&typeName, &identifierRange)) {
            if (typeRange != nullptr) {
                *typeRange = makeRange(typeStart, m_pos);
            }
            return false;
        }
        if (attemptedTypeName != nullptr) {
            *attemptedTypeName = typeName;
        }
        if (typeRange != nullptr) {
            *typeRange = makeRange(typeStart, identifierRange.end);
        }
        Endianness postfixEndianness = Endianness::Native;
        QString postfixDecoration;
        if (!parsePostfixDecoration(&postfixEndianness, &postfixDecoration)) {
            if (typeRange != nullptr) {
                *typeRange = makeRange(typeStart, m_pos);
            }
            return false;
        }
        if (typeRange != nullptr) {
            *typeRange =
                makeRange(typeStart, postfixDecoration.isEmpty() ? identifierRange.end : m_pos);
        }
        if (!postfixDecoration.isEmpty()) {
            endianness = postfixEndianness;
            decoration = postfixDecoration;
        }
        ResolvedType resolved;
        QString resolvedDisplay;
        if (!graph.resolveTypeName(typeName, &resolved, &resolvedDisplay)) {
            return false;
        }
        if (const auto* pod = std::get_if<PodType>(&resolved)) {
            PodType merged = *pod;
            if (decoration.isEmpty()) {
                merged.endianness = pod->endianness;
                merged.decoration = pod->decoration;
            } else {
                merged.endianness = endianness;
                merged.decoration = decoration;
            }
            *out = merged;
            if (displayName != nullptr) {
                if (!decoration.isEmpty()) {
                    *displayName = decoration + typeName;
                } else {
                    *displayName = typeName;
                }
            }
            return true;
        }
        if (const auto* string = std::get_if<StringType>(&resolved)) {
            StringType merged = *string;
            if (!decoration.isEmpty()) {
                if (string->encoding != StringEncoding::Utf16) {
                    return false;
                }
                merged.endianness = endianness;
                merged.decoration = decoration;
            }
            *out = merged;
            if (displayName != nullptr) {
                *displayName = decoration.isEmpty() ? typeName : decoration + typeName;
            }
            return true;
        }
        if (decoration.isEmpty()) {
            *out = resolved;
            if (displayName != nullptr) {
                *displayName = resolvedDisplay;
            }
            return true;
        }
        return false;
    }

    bool consumeSemicolon() {
        skipWhitespaceAndComments();
        if (peek() != QLatin1Char(';')) {
            return false;
        }
        ++m_pos;
        consumeSemicolon();
        return true;
    }

    bool consumeExpected(QChar expected) {
        skipWhitespaceAndComments();
        if (peek() != expected) {
            return false;
        }
        ++m_pos;
        return true;
    }

    bool parseIntegerExpression(ValueExpression* out) {
        skipWhitespaceAndComments();
        const int start = m_pos;
        bool negative = false;
        if (peek() == QLatin1Char('-')) {
            negative = true;
            ++m_pos;
        }
        int base = 10;
        if (peekAhead(QStringLiteral("0x")) || peekAhead(QStringLiteral("0X"))) {
            base = 16;
            m_pos += 2;
        }
        const int digitsStart = m_pos;
        while (!atEnd()) {
            const QChar ch = peek();
            const bool isDigit =
                base == 10 ? ch.isDigit()
                           : ch.isDigit() ||
                                 (ch.toLower() >= QLatin1Char('a') &&
                                  ch.toLower() <= QLatin1Char('f'));
            if (!isDigit) {
                break;
            }
            ++m_pos;
        }
        if (m_pos == digitsStart) {
            m_pos = start;
            return false;
        }
        bool ok = false;
        const quint64 magnitude =
            m_text.mid(digitsStart, m_pos - digitsStart).toULongLong(&ok, base);
        if (!ok) {
            m_pos = start;
            return false;
        }
        if (negative) {
            const quint64 signedLimit =
                static_cast<quint64>(std::numeric_limits<qint64>::max()) + 1U;
            if (magnitude > signedLimit) {
                m_pos = start;
                return false;
            }
            out->integerValue =
                magnitude == signedLimit
                    ? std::numeric_limits<qint64>::min()
                    : -static_cast<qint64>(magnitude);
            out->integerIsUnsigned = false;
        } else {
            out->unsignedIntegerValue = magnitude;
            out->integerIsUnsigned = true;
        }
        out->kind = ExpressionKind::Integer;
        out->range = {start, m_pos};
        return true;
    }

    bool parseStringExpression(ValueExpression* out) {
        skipWhitespaceAndComments();
        if (peek() != QLatin1Char('"')) {
            return false;
        }
        const int start = m_pos++;
        QString value;
        while (!atEnd() && peek() != QLatin1Char('"')) {
            QChar ch = consume();
            if (ch != QLatin1Char('\\')) {
                value += ch;
                continue;
            }
            if (atEnd()) {
                m_pos = start;
                return false;
            }
            const QChar escaped = consume();
            if (escaped == QLatin1Char('n')) {
                value += QLatin1Char('\n');
            } else if (escaped == QLatin1Char('r')) {
                value += QLatin1Char('\r');
            } else if (escaped == QLatin1Char('t')) {
                value += QLatin1Char('\t');
            } else if (escaped == QLatin1Char('0')) {
                value += QChar(0);
            } else if (escaped == QLatin1Char('"') || escaped == QLatin1Char('\\')) {
                value += escaped;
            } else {
                value += escaped;
            }
        }
        if (atEnd() || peek() != QLatin1Char('"')) {
            m_pos = start;
            return false;
        }
        ++m_pos;
        out->kind = ExpressionKind::String;
        out->stringValue = value;
        out->range = {start, m_pos};
        return true;
    }

    bool parseBooleanExpression(ValueExpression* out) {
        skipWhitespaceAndComments();
        const int start = m_pos;
        if (consumeKeyword(QStringLiteral("true"))) {
            out->booleanValue = true;
        } else if (consumeKeyword(QStringLiteral("false"))) {
            out->booleanValue = false;
        } else {
            m_pos = start;
            return false;
        }
        out->kind = ExpressionKind::Boolean;
        out->range = {start, m_pos};
        return true;
    }

    bool parseVariableExpression(ValueExpression* out) {
        skipWhitespaceAndComments();
        if (peek() != QLatin1Char('$')) {
            return false;
        }
        const int start = m_pos++;
        QString part;
        if (!parseIdentifier(&part)) {
            m_pos = start;
            return false;
        }
        QStringList path{part};
        while (!atEnd()) {
            skipWhitespaceAndComments();
            if (peek() != QLatin1Char('.')) {
                break;
            }
            ++m_pos;
            if (!parseIdentifier(&part)) {
                m_pos = start;
                return false;
            }
            path.push_back(part);
        }
        out->kind = ExpressionKind::Variable;
        out->variablePath = path;
        out->range = {start, m_pos};
        return true;
    }

    static void assignIntFromValue(const ValueExpression& value, IntExpression* out) {
        out->kind = value.kind == ExpressionKind::Variable
                        ? IntExpressionKind::Variable
                        : IntExpressionKind::Integer;
        out->integerValue = value.integerValue;
        out->unsignedIntegerValue = value.unsignedIntegerValue;
        out->integerIsUnsigned = value.integerIsUnsigned;
        out->variablePath = value.variablePath;
        out->range = value.range;
    }

    bool parseIntLiteralExpression(IntExpression* out) {
        ValueExpression value;
        if (!parseIntegerExpression(&value)) {
            return false;
        }
        assignIntFromValue(value, out);
        return true;
    }

    bool parseIntVariableExpression(IntExpression* out) {
        ValueExpression value;
        if (!parseVariableExpression(&value)) {
            return false;
        }
        assignIntFromValue(value, out);
        return true;
    }

    bool parseIntFactor(IntExpression* out) {
        skipWhitespaceAndComments();
        const int start = m_pos;
        if (peek() == QLatin1Char('-')) {
            ++m_pos;
            IntExpression operand;
            if (!parseIntFactor(&operand)) {
                m_pos = start;
                return false;
            }
            out->kind = IntExpressionKind::UnaryMinus;
            out->left = std::make_shared<IntExpression>(operand);
            out->range = makeRange(start, operand.range.end);
            return true;
        }
        if (peek() == QLatin1Char('(')) {
            ++m_pos;
            if (!parseIntExpression(out) ||
                !consumeExpected(QLatin1Char(')'))) {
                m_pos = start;
                return false;
            }
            out->range = makeRange(start, m_pos);
            return true;
        }
        return parseIntVariableExpression(out) || parseIntLiteralExpression(out);
    }

    bool parseIntTerm(IntExpression* out) {
        if (!parseIntFactor(out)) {
            return false;
        }
        while (true) {
            skipWhitespaceAndComments();
            const QChar op = peek();
            if (op != QLatin1Char('*') && op != QLatin1Char('/')) {
                return true;
            }
            ++m_pos;
            IntExpression right;
            if (!parseIntFactor(&right)) {
                return false;
            }
            IntExpression combined;
            combined.kind = IntExpressionKind::Binary;
            combined.binaryOp = op == QLatin1Char('*') ? IntBinaryOp::Multiply
                                                       : IntBinaryOp::Divide;
            combined.left = std::make_shared<IntExpression>(*out);
            combined.right = std::make_shared<IntExpression>(right);
            combined.range = makeRange(out->range.start, right.range.end);
            *out = combined;
        }
    }

    bool parseValueExpression(ValueExpression* out) {
        return parseVariableExpression(out) || parseStringExpression(out) ||
               parseIntegerExpression(out);
    }

    bool parseIntExpression(IntExpression* out) {
        if (!parseIntTerm(out)) {
            return false;
        }
        while (true) {
            skipWhitespaceAndComments();
            const QChar op = peek();
            if (op != QLatin1Char('+') && op != QLatin1Char('-')) {
                return true;
            }
            ++m_pos;
            IntExpression right;
            if (!parseIntTerm(&right)) {
                return false;
            }
            IntExpression combined;
            combined.kind = IntExpressionKind::Binary;
            combined.binaryOp = op == QLatin1Char('+') ? IntBinaryOp::Add
                                                       : IntBinaryOp::Subtract;
            combined.left = std::make_shared<IntExpression>(*out);
            combined.right = std::make_shared<IntExpression>(right);
            combined.range = makeRange(out->range.start, right.range.end);
            *out = combined;
        }
    }

    bool validateCountExpression(ParseResult& result,
                                 const IntExpression& expression,
                                 const QString& context) {
        if (expression.kind == IntExpressionKind::Integer &&
            !expression.integerIsUnsigned && expression.integerValue < 0) {
            setError(result, QStringLiteral("%1 cannot be negative").arg(context),
                     expression.range.start, expression.range.end);
            return false;
        }
        return true;
    }

    bool parseComparisonExpression(ParseResult& result, ComparisonExpression* out,
                                   const QString& context) {
        skipWhitespaceAndComments();
        const int start = m_pos;
        const QChar op = peek();
        if (op != QLatin1Char('=') && op != QLatin1Char('<') &&
            op != QLatin1Char('>')) {
            setError(result, QStringLiteral("Expected comparison operator in %1").arg(context),
                     m_pos, m_pos);
            return false;
        }
        ++m_pos;
        if (op == QLatin1Char('=')) {
            out->op = ComparisonOperator::Equal;
        } else if (op == QLatin1Char('<')) {
            out->op = ComparisonOperator::Less;
        } else {
            out->op = ComparisonOperator::Greater;
        }
        ValueExpression stringValue;
        if (parseStringExpression(&stringValue)) {
            out->right = stringValue;
        } else {
            IntExpression integer;
            if (!parseIntExpression(&integer)) {
                setError(result, QStringLiteral("Expected value in %1").arg(context), m_pos, m_pos);
                return false;
            }
            out->rightIntegerExpression = integer;
            ValueExpression legacyRight;
            legacyRight.kind = ExpressionKind::Integer;
            legacyRight.range = integer.range;
            out->right = legacyRight;
        }
        out->range = {start, m_pos};
        out->sourceText = m_text.mid(start, m_pos - start).trimmed();
        return true;
    }

    bool parseTwoSidedComparisonExpression(ParseResult& result, ComparisonExpression* out,
                                           const QString& context) {
        skipWhitespaceAndComments();
        const int start = m_pos;
        IntExpression left;
        if (!parseIntExpression(&left)) {
            setError(result, QStringLiteral("Expected left expression in %1").arg(context),
                     m_pos, m_pos);
            return false;
        }
        skipWhitespaceAndComments();
        const QChar op = peek();
        if (op != QLatin1Char('=') && op != QLatin1Char('<') &&
            op != QLatin1Char('>')) {
            setError(result, QStringLiteral("Expected comparison operator in %1").arg(context),
                     m_pos, m_pos);
            return false;
        }
        ++m_pos;
        if (op == QLatin1Char('=')) {
            out->op = ComparisonOperator::Equal;
        } else if (op == QLatin1Char('<')) {
            out->op = ComparisonOperator::Less;
        } else {
            out->op = ComparisonOperator::Greater;
        }
        IntExpression right;
        if (!parseIntExpression(&right)) {
            setError(result, QStringLiteral("Expected value in %1").arg(context), m_pos, m_pos);
            return false;
        }
        out->leftIntegerExpression = left;
        out->rightIntegerExpression = right;
        out->range = {start, m_pos};
        out->sourceText = m_text.mid(start, m_pos - start).trimmed();
        return true;
    }

    bool parseFieldDirectives(ParseResult& result, FieldAttributes* attributes) {
        while (true) {
            skipWhitespaceAndComments();
            if (peek() != QLatin1Char('/')) {
                return true;
            }
            const int directiveStart = m_pos++;
            QString directive;
            TextRange directiveRange;
            if (!parseIdentifier(&directive, &directiveRange)) {
                setError(result, QStringLiteral("Expected directive name after '/'"),
                         directiveStart, m_pos);
                return false;
            }
            if (!consumeExpected(QLatin1Char('('))) {
                setError(result,
                         QStringLiteral("Expected '(' after directive '/%1'").arg(directive),
                         m_pos, m_pos);
                return false;
            }
            if (directive == QStringLiteral("var")) {
                if (!attributes->variableName.isEmpty()) {
                    setError(result, QStringLiteral("Duplicate /var directive"),
                             directiveStart, m_pos);
                    return false;
                }
                QString variableName;
                if (!parseIdentifier(&variableName)) {
                    setError(result, QStringLiteral("Expected variable name in /var"),
                             m_pos, m_pos);
                    return false;
                }
                attributes->variableName = variableName;
            } else if (directive == QStringLiteral("repeat")) {
                if (attributes->repeatExpression.has_value()) {
                    setError(result, QStringLiteral("Duplicate /repeat directive"),
                             directiveStart, m_pos);
                    return false;
                }
                IntExpression expression;
                if (!parseIntExpression(&expression)) {
                    setError(result, QStringLiteral("Expected repeat count in /repeat"),
                             m_pos, m_pos);
                    return false;
                }
                if (!validateCountExpression(result, expression,
                                             QStringLiteral("/repeat count"))) {
                    return false;
                }
                attributes->repeatExpression = expression;
            } else if (directive == QStringLiteral("cond")) {
                if (attributes->conditionExpression.has_value()) {
                    setError(result, QStringLiteral("Duplicate /cond directive"),
                             directiveStart, m_pos);
                    return false;
                }
                ComparisonExpression comparison;
                ValueExpression boolean;
                if (parseBooleanExpression(&boolean)) {
                    comparison.op = ComparisonOperator::Equal;
                    comparison.right = boolean;
                    comparison.range = boolean.range;
                } else {
                    if (!parseComparisonExpression(result, &comparison,
                                                   QStringLiteral("/cond"))) {
                        return false;
                    }
                }
                attributes->conditionExpression = comparison;
            } else if (directive == QStringLiteral("when")) {
                if (attributes->whenExpression.has_value()) {
                    setError(result, QStringLiteral("Duplicate /when directive"),
                             directiveStart, m_pos);
                    return false;
                }
                ComparisonExpression comparison;
                if (!parseTwoSidedComparisonExpression(result, &comparison,
                                                       QStringLiteral("/when"))) {
                    return false;
                }
                attributes->whenExpression = comparison;
            } else if (directive == QStringLiteral("source")) {
                if (!attributes->sourceRole.isEmpty()) {
                    setError(result, QStringLiteral("Duplicate /source directive"),
                             directiveStart, m_pos);
                    return false;
                }
                QString role;
                if (!parseIdentifier(&role)) {
                    setError(result, QStringLiteral("Expected external role in /source"),
                             m_pos, m_pos);
                    return false;
                }
                attributes->sourceRole = role;
            } else {
                setError(result, QStringLiteral("Unknown directive '/%1'").arg(directive),
                         directiveRange.start, directiveRange.end);
                return false;
            }
            if (!consumeExpected(QLatin1Char(')'))) {
                setError(result,
                         QStringLiteral("Expected ')' after '/%1' directive").arg(directive),
                         m_pos, m_pos);
                return false;
            }
            const QString text = m_text.mid(directiveStart, m_pos - directiveStart).trimmed();
            if (!attributes->decoration.isEmpty()) {
                attributes->decoration += QLatin1Char(' ');
            }
            attributes->decoration += text;
        }
    }

    bool parseFieldModifiers(ParseResult& result, FieldAttributes* attributes) {
        while (true) {
            skipWhitespaceAndComments();
            if (peek() != QLatin1Char('<')) {
                return true;
            }
            const int modifierStart = m_pos++;
            QString modifier;
            TextRange modifierRange;
            if (!parseIdentifier(&modifier, &modifierRange)) {
                setError(result, QStringLiteral("Expected modifier name after '<'"),
                         modifierStart, m_pos);
                return false;
            }
            if (modifier != QStringLiteral("len") && modifier != QStringLiteral("max") &&
                modifier != QStringLiteral("until")) {
                setError(result, QStringLiteral("Unknown modifier '<%1>'").arg(modifier),
                         modifierRange.start, modifierRange.end);
                return false;
            }
            if (attributes->lengthMode != LengthMode::None) {
                setError(result, QStringLiteral("Only one length modifier is allowed"),
                         modifierStart, m_pos);
                return false;
            }
            if (!consumeExpected(QLatin1Char(':'))) {
                setError(result, QStringLiteral("Expected ':' in '<%1>' modifier").arg(modifier),
                         m_pos, m_pos);
                return false;
            }
            if (modifier == QStringLiteral("until")) {
                ComparisonExpression comparison;
                if (!parseComparisonExpression(result, &comparison,
                                               QStringLiteral("<until>"))) {
                    return false;
                }
                attributes->lengthMode = LengthMode::Until;
                attributes->untilExpression = comparison;
            } else {
                IntExpression expression;
                if (!parseIntExpression(&expression)) {
                    setError(result,
                             QStringLiteral("Expected length value in '<%1>' modifier")
                                 .arg(modifier),
                             m_pos, m_pos);
                    return false;
                }
                if (!validateCountExpression(
                        result, expression,
                        QStringLiteral("<%1> length").arg(modifier))) {
                    return false;
                }
                attributes->lengthMode = modifier == QStringLiteral("len")
                                             ? LengthMode::Fixed
                                             : LengthMode::Maximum;
                attributes->lengthExpression = expression;
            }
            if (!consumeExpected(QLatin1Char('>'))) {
                setError(result, QStringLiteral("Expected '>' after '<%1>' modifier").arg(modifier),
                         m_pos, m_pos);
                return false;
            }
            const QString text = m_text.mid(modifierStart, m_pos - modifierStart).trimmed();
            if (!attributes->decoration.isEmpty()) {
                attributes->decoration += QLatin1Char(' ');
            }
            attributes->decoration += text;
        }
    }

    bool parseBitIndex(ParseResult& result, int* bitIndex, const QString& context) {
        IntExpression expression;
        if (!parseIntLiteralExpression(&expression) || !expression.integerIsUnsigned ||
            expression.unsignedIntegerValue >
                static_cast<quint64>(std::numeric_limits<int>::max())) {
            setError(result, QStringLiteral("Expected bit index in %1").arg(context),
                     m_pos, m_pos);
            return false;
        }
        *bitIndex = static_cast<int>(expression.unsignedIntegerValue);
        return true;
    }

    bool parseBitfieldBlock(ParseResult& result, FieldAttributes* attributes) {
        skipWhitespaceAndComments();
        if (peek() != QLatin1Char('{')) {
            return true;
        }
        ++m_pos;
        skipWhitespaceAndComments();
        QSet<QString> names;
        while (peek() != QLatin1Char('}')) {
            if (atEnd()) {
                setError(result, QStringLiteral("Unclosed bitfield block"), m_pos, m_pos);
                return false;
            }
            QString keyword;
            TextRange keywordRange;
            if (!parseIdentifier(&keyword, &keywordRange) ||
                (keyword != QStringLiteral("bit") && keyword != QStringLiteral("bits"))) {
                setError(result, QStringLiteral("Expected 'bit' or 'bits' in bitfield block"),
                         keywordRange.start, keywordRange.end);
                return false;
            }
            int highBit = 0;
            int lowBit = 0;
            if (!parseBitIndex(result, &highBit, keyword)) {
                return false;
            }
            if (keyword == QStringLiteral("bits")) {
                if (!consumeExpected(QLatin1Char(':'))) {
                    setError(result, QStringLiteral("Expected ':' in bits range"), m_pos, m_pos);
                    return false;
                }
                if (!parseBitIndex(result, &lowBit, keyword)) {
                    return false;
                }
            } else {
                lowBit = highBit;
            }
            QString name;
            TextRange nameRange;
            if (!parseIdentifier(&name, &nameRange)) {
                setError(result, QStringLiteral("Expected bitfield member name"), m_pos, m_pos);
                return false;
            }
            if (names.contains(name)) {
                setError(result, QStringLiteral("Duplicate bitfield member '%1'").arg(name),
                         nameRange.start, nameRange.end);
                return false;
            }
            if (!consumeSemicolon()) {
                setError(result, QStringLiteral("Expected ';' after bitfield member '%1'")
                                      .arg(name),
                         m_pos, m_pos);
                return false;
            }
            names.insert(name);
            attributes->bitfields.push_back(BitfieldMember{highBit, lowBit, name, nameRange});
            skipWhitespaceAndComments();
        }
        ++m_pos;
        return true;
    }

    bool validateFieldAttributes(ParseResult& result, const ResolvedType& type,
                                 const FieldAttributes& attributes, int start, int end) {
        if (attributes.lengthMode != LengthMode::None &&
            std::holds_alternative<StructRefType>(type)) {
            setError(result, QStringLiteral("Length modifiers are not allowed on struct fields"),
                     start, end);
            return false;
        }
        if (!attributes.variableName.isEmpty() &&
            (attributes.hasDynamicExtent() ||
             (!std::holds_alternative<PodType>(type) &&
              !std::holds_alternative<ByteType>(type)))) {
            setError(result,
                     QStringLiteral("/var requires a scalar integer field"),
                     start, end);
            return false;
        }
        if (attributes.conditionExpression.has_value() &&
            attributes.repeatExpression.has_value()) {
            setError(result,
                     QStringLiteral("/cond is not allowed on repeated fields"),
                     start, end);
            return false;
        }
        if (attributes.whenExpression.has_value() &&
            attributes.repeatExpression.has_value()) {
            setError(result,
                     QStringLiteral("/when is not allowed on repeated fields"),
                     start, end);
            return false;
        }
        if (attributes.whenExpression.has_value() &&
            attributes.lengthMode != LengthMode::None) {
            setError(result,
                     QStringLiteral("/when is not allowed on length-modified fields"),
                     start, end);
            return false;
        }
        if (attributes.conditionExpression.has_value() &&
            attributes.lengthMode != LengthMode::None &&
            std::holds_alternative<PodType>(type)) {
            setError(result,
                     QStringLiteral("/cond cannot compare an integer array"),
                     start, end);
            return false;
        }
        if (attributes.conditionExpression.has_value() &&
            std::holds_alternative<StructRefType>(type) &&
            attributes.conditionExpression->right.kind != ExpressionKind::Boolean) {
            setError(result,
                     QStringLiteral("/cond on a struct requires true or false"),
                     start, end);
            return false;
        }
        if (!attributes.bitfields.isEmpty()) {
            const auto* pod = std::get_if<PodType>(&type);
            if (pod == nullptr || attributes.lengthMode != LengthMode::None ||
                attributes.repeatExpression.has_value()) {
                setError(result,
                         QStringLiteral("Bitfields require a scalar integer field"),
                         start, end);
                return false;
            }
            const int widthBits = podKindWidthBytes(pod->kind) * 8;
            QVector<bool> usedBits(widthBits, false);
            for (const BitfieldMember& member : attributes.bitfields) {
                if (member.highBit < member.lowBit || member.lowBit < 0 ||
                    member.highBit >= widthBits) {
                    setError(result,
                             QStringLiteral("Bitfield member '%1' is outside the field width")
                                 .arg(member.name),
                             member.nameRange.start, member.nameRange.end);
                    return false;
                }
                for (int bit = member.lowBit; bit <= member.highBit; ++bit) {
                    if (usedBits.at(bit)) {
                        setError(result,
                                 QStringLiteral("Bitfield member '%1' overlaps another member")
                                     .arg(member.name),
                                 member.nameRange.start, member.nameRange.end);
                        return false;
                    }
                    usedBits[bit] = true;
                }
            }
        }
        return true;
    }

    bool parseStructMembers(ParseResult& result, StructNode& structNode, const QString& contextName) {
        skipWhitespaceAndComments();
        QSet<QString> memberNames;
        QSet<QString> variableNames;
        while (peek() != QLatin1Char('}')) {
            if (atEnd()) {
                setError(result, QStringLiteral("Unclosed struct body"), m_pos, m_pos);
                return false;
            }
            StructMember member;
            const int memberStart = m_pos;
            if (peek() == QLatin1Char('/')) {
                const int saved = m_pos;
                ++m_pos;
                QString directive;
                if (parseIdentifier(&directive) && directive == QStringLiteral("assert")) {
                    if (!consumeExpected(QLatin1Char('('))) {
                        setError(result, QStringLiteral("Expected '(' after directive '/assert'"),
                                 m_pos, m_pos);
                        return false;
                    }
                    ComparisonExpression assertion;
                    if (!parseTwoSidedComparisonExpression(result, &assertion,
                                                           QStringLiteral("/assert"))) {
                        return false;
                    }
                    if (!consumeExpected(QLatin1Char(')'))) {
                        setError(result,
                                 QStringLiteral("Expected ')' after '/assert' directive"),
                                 m_pos, m_pos);
                        return false;
                    }
                    if (!consumeSemicolon()) {
                        setError(result, QStringLiteral("Expected ';' after /assert"),
                                 m_pos, m_pos);
                        return false;
                    }
                    structNode.assertions.push_back(assertion);
                    skipWhitespaceAndComments();
                    continue;
                }
                m_pos = saved;
            }
            if (!parseFieldDirectives(result, &member.attributes)) {
                return false;
            }
            QString attemptedTypeName;
            TextRange typeRange;
            if (!parseTypeSpecifier(result.graph, &member.type, &member.typeDisplayName,
                                    &attemptedTypeName, &typeRange)) {
                const QString typeName =
                    attemptedTypeName.isEmpty() ? QStringLiteral("?") : attemptedTypeName;
                setError(result,
                         QStringLiteral("Invalid member type '%1' in '%2'")
                             .arg(typeName, contextName),
                         typeRange.start < typeRange.end ? typeRange.start : memberStart,
                         typeRange.start < typeRange.end ? typeRange.end : m_pos);
                return false;
            }
            if (!parseFieldModifiers(result, &member.attributes)) {
                return false;
            }
            TextRange memberNameRange;
            if (!parseIdentifier(&member.name, &memberNameRange)) {
                skipWhitespaceAndComments();
                if (!member.attributes.repeatExpression.has_value() ||
                    peek() != QLatin1Char(';')) {
                    setError(result,
                             QStringLiteral("Expected member name in struct '%1'")
                                 .arg(contextName),
                             m_pos, m_pos);
                    return false;
                }
            }
            skipWhitespaceAndComments();
            const bool hasBitfieldBlock = peek() == QLatin1Char('{');
            if (!parseBitfieldBlock(result, &member.attributes)) {
                return false;
            }
            if (!validateFieldAttributes(result, member.type, member.attributes,
                                         memberStart, m_pos)) {
                return false;
            }
            if (!member.name.isEmpty() && memberNames.contains(member.name)) {
                setError(result,
                         QStringLiteral("Duplicate member name '%1' in struct '%2'")
                             .arg(member.name, contextName),
                         memberNameRange.start, memberNameRange.end);
                return false;
            }
            if (!member.attributes.variableName.isEmpty() &&
                variableNames.contains(member.attributes.variableName)) {
                setError(result,
                         QStringLiteral("Duplicate variable name '%1' in struct '%2'")
                             .arg(member.attributes.variableName, contextName),
                         memberStart, m_pos);
                return false;
            }
            member.nameRange = member.name.isEmpty() ? typeRange : memberNameRange;
            if (!hasBitfieldBlock && !consumeSemicolon()) {
                setError(result,
                         QStringLiteral("Expected ';' after member '%1'")
                             .arg(member.name.isEmpty() ? member.typeDisplayName : member.name),
                         m_pos, m_pos);
                return false;
            }
            if (!member.name.isEmpty()) {
                memberNames.insert(member.name);
            }
            if (!member.attributes.variableName.isEmpty()) {
                variableNames.insert(member.attributes.variableName);
            }
            structNode.members.push_back(member);
            skipWhitespaceAndComments();
        }
        return true;
    }

    bool parseStructBody(ParseResult& result, StructNode& structNode, const QString& contextName) {
        skipWhitespaceAndComments();
        if (peek() != QLatin1Char('{')) {
            setError(result,
                     QStringLiteral("Expected '{' in struct '%1'").arg(contextName),
                     m_pos, m_pos);
            return false;
        }
        ++m_pos;
        const int bodyStart = m_pos;
        structNode.bodyRange = {bodyStart, bodyStart};
        if (!parseStructMembers(result, structNode, contextName)) {
            return false;
        }
        ++m_pos;
        structNode.bodyRange.end = m_pos;
        skipWhitespaceAndComments();
        if (peek() == QLatin1Char(';')) {
            ++m_pos;
        }
        if (structNode.members.isEmpty()) {
            setError(result,
                     QStringLiteral("Struct '%1' must have members").arg(contextName),
                     bodyStart, m_pos);
            return false;
        }
        return true;
    }

    bool parseTypedefSimple(ParseResult& result) {
        const int typeStart = m_pos;
        Endianness endianness = Endianness::Native;
        QString decoration;
        if (!parseDecoration(&endianness, &decoration)) {
            setError(result, QStringLiteral("Invalid endianness decoration in typedef"),
                     typeStart, m_pos);
            return false;
        }
        QString baseName;
        TextRange baseRange;
        if (!parseIdentifier(&baseName, &baseRange)) {
            setError(result, QStringLiteral("Expected type name in typedef"), m_pos, m_pos);
            return false;
        }
        Endianness postfixEndianness = Endianness::Native;
        QString postfixDecoration;
        if (!parsePostfixDecoration(&postfixEndianness, &postfixDecoration)) {
            setError(result, QStringLiteral("Invalid postfix decoration in typedef"),
                     typeStart, m_pos);
            return false;
        }
        if (!postfixDecoration.isEmpty()) {
            endianness = postfixEndianness;
            decoration = postfixDecoration;
        }
        QString aliasName;
        TextRange aliasRange;
        if (!parseIdentifier(&aliasName, &aliasRange)) {
            setError(result, QStringLiteral("Expected typedef alias name"), m_pos, m_pos);
            return false;
        }
        if (!consumeSemicolon()) {
            setError(result, QStringLiteral("Expected ';' after typedef"), m_pos, m_pos);
            return false;
        }
        ResolvedType resolved;
        QString displayName;
        if (!result.graph.resolveTypeName(baseName, &resolved, &displayName)) {
            setError(result, QStringLiteral("Unknown type '%1' in typedef").arg(baseName),
                     baseRange.start, baseRange.end);
            return false;
        }
        if (const auto* pod = std::get_if<PodType>(&resolved)) {
            PodType merged = *pod;
            if (!decoration.isEmpty()) {
                merged.endianness = endianness;
                merged.decoration = decoration;
                displayName = decoration + baseName;
            }
            resolved = merged;
        } else if (const auto* string = std::get_if<StringType>(&resolved)) {
            if (!decoration.isEmpty() && string->encoding != StringEncoding::Utf16) {
                setError(result,
                         QStringLiteral("Endianness decoration is only allowed on utf16str"),
                         typeStart, baseRange.end);
                return false;
            }
            StringType merged = *string;
            if (!decoration.isEmpty()) {
                merged.endianness = endianness;
                merged.decoration = decoration;
                displayName = decoration + baseName;
            }
            resolved = merged;
        } else if (!decoration.isEmpty()) {
            setError(result,
                     QStringLiteral("Endianness decoration not allowed on this typedef"),
                     typeStart, baseRange.end);
            return false;
        }
        TypedefNode node;
        node.name = aliasName;
        node.type = resolved;
        node.typeDisplayName = displayName;
        node.nameRange = aliasRange;
        if (!result.graph.addTypedef(node)) {
            setError(result, QStringLiteral("Duplicate name '%1'").arg(aliasName),
                     aliasRange.start, aliasRange.end);
            return false;
        }
        return true;
    }

    bool parseTypedef(ParseResult& result) {
        const int typedefStart = m_pos;
        if (!consumeKeyword(QStringLiteral("typedef"))) {
            return false;
        }
        if (consumeKeyword(QStringLiteral("struct"))) {
            QString tagName;
            TextRange tagRange;
            skipWhitespaceAndComments();
            if (peek() != QLatin1Char('{')) {
                if (!parseIdentifier(&tagName, &tagRange)) {
                    setError(result, QStringLiteral("Expected struct tag or '{'"), m_pos, m_pos);
                    return false;
                }
            }
            const QString contextName =
                tagName.isEmpty() ? QStringLiteral("anonymous struct") : tagName;
            StructNode structNode;
            if (!parseStructBody(result, structNode, contextName)) {
                return false;
            }
            QString aliasName;
            TextRange aliasRange;
            if (!parseIdentifier(&aliasName, &aliasRange)) {
                setError(result, QStringLiteral("Expected typedef alias after struct body"),
                         m_pos, m_pos);
                return false;
            }
            if (!consumeSemicolon()) {
                setError(result, QStringLiteral("Expected ';' after typedef"), m_pos, m_pos);
                return false;
            }
            if (tagName.isEmpty()) {
                structNode.name = aliasName;
                structNode.nameRange = aliasRange;
            } else {
                structNode.name = tagName;
                structNode.nameRange = tagRange;
            }
            if (!result.graph.addStruct(structNode)) {
                setError(result, QStringLiteral("Duplicate name '%1'").arg(structNode.name),
                         structNode.nameRange.start, structNode.nameRange.end);
                return false;
            }
            if (!tagName.isEmpty() && tagName != aliasName) {
                TypedefNode typedefNode;
                typedefNode.name = aliasName;
                typedefNode.type = StructRefType{tagName};
                typedefNode.typeDisplayName = tagName;
                typedefNode.nameRange = aliasRange;
                if (!result.graph.addTypedef(typedefNode)) {
                    setError(result, QStringLiteral("Duplicate name '%1'").arg(aliasName),
                             aliasRange.start, aliasRange.end);
                    return false;
                }
            }
            return true;
        }
        m_pos = typedefStart;
        if (!consumeKeyword(QStringLiteral("typedef"))) {
            return false;
        }
        return parseTypedefSimple(result);
    }

    bool parseStruct(ParseResult& result) {
        if (!consumeKeyword(QStringLiteral("struct"))) {
            return false;
        }
        QString structName;
        TextRange nameRange;
        skipWhitespaceAndComments();
        if (peek() == QLatin1Char('{')) {
            setError(result,
                     QStringLiteral("Anonymous struct requires a typedef name, e.g. typedef struct { ... } alias;"),
                     m_pos, m_pos);
            return false;
        }
        if (!parseIdentifier(&structName, &nameRange)) {
            setError(result, QStringLiteral("Expected struct name"), m_pos, m_pos);
            return false;
        }
        StructNode structNode;
        structNode.name = structName;
        structNode.nameRange = nameRange;
        if (!parseStructBody(result, structNode, structName)) {
            return false;
        }
        if (!result.graph.addStruct(structNode)) {
            setError(result, QStringLiteral("Duplicate name '%1'").arg(structName),
                     nameRange.start, nameRange.end);
            return false;
        }
        return true;
    }

    bool parseStandaloneMember(ParseResult& result) {
        const int memberStart = m_pos;
        FieldAttributes attributes;
        if (!parseFieldDirectives(result, &attributes)) {
            return false;
        }
        ResolvedType type;
        QString displayName;
        QString attemptedTypeName;
        TextRange typeRange;
        if (!parseTypeSpecifier(result.graph, &type, &displayName, &attemptedTypeName,
                                &typeRange)) {
            if (!attemptedTypeName.isEmpty()) {
                setError(result,
                         QStringLiteral("Invalid member type '%1' in top-level declaration")
                             .arg(attemptedTypeName),
                         typeRange.start, typeRange.end);
            }
            return false;
        }
        if (!parseFieldModifiers(result, &attributes)) {
            return false;
        }
        QString name;
        TextRange nameRange;
        if (!parseIdentifier(&name, &nameRange)) {
            skipWhitespaceAndComments();
            if (!attributes.repeatExpression.has_value() || peek() != QLatin1Char(';')) {
                setError(result, QStringLiteral("Expected name in top-level declaration"),
                         m_pos, m_pos);
                return false;
            }
            name = displayName;
            nameRange = typeRange;
        }
        skipWhitespaceAndComments();
        const bool hasBitfieldBlock = peek() == QLatin1Char('{');
        if (!parseBitfieldBlock(result, &attributes)) {
            return false;
        }
        if (!validateFieldAttributes(result, type, attributes, memberStart, m_pos)) {
            return false;
        }
        if (!hasBitfieldBlock && !consumeSemicolon()) {
            setError(result, QStringLiteral("Expected ';' after '%1'").arg(name), m_pos, m_pos);
            return false;
        }
        StandaloneMemberNode node;
        node.name = name;
        node.type = type;
        node.typeDisplayName = displayName;
        node.attributes = attributes;
        node.nameRange = nameRange;
        if (!result.graph.addStandaloneMember(node)) {
            setError(result, QStringLiteral("Duplicate name '%1'").arg(name),
                     nameRange.start, nameRange.end);
            return false;
        }
        return true;
    }

    bool parseDefaultDirective(ParseResult& result, bool* recognized) {
        *recognized = false;
        skipWhitespaceAndComments();
        const int directiveStart = m_pos;
        if (peek() != QLatin1Char('/')) {
            return false;
        }
        ++m_pos;
        QString directive;
        TextRange directiveRange;
        if (!parseIdentifier(&directive, &directiveRange) ||
            directive != QStringLiteral("default")) {
            m_pos = directiveStart;
            return false;
        }
        *recognized = true;
        if (!result.graph.defaultEntryName().isEmpty()) {
            setError(result,
                     QStringLiteral("/default may appear only once per file"),
                     directiveStart, directiveRange.end);
            return false;
        }

        QString entryName;
        TextRange entryRange;
        if (!parseIdentifier(&entryName, &entryRange)) {
            setError(result, QStringLiteral("Expected entry name after /default"),
                     m_pos, m_pos);
            return false;
        }
        skipWhitespaceAndComments();
        if (peek() == QLatin1Char(';')) {
            consumeSemicolon();
        }
        result.graph.setDefaultEntryName(entryName);
        m_defaultEntryRange = entryRange;
        return true;
    }

    bool parseExternalDirective(ParseResult& result, bool* recognized) {
        *recognized = false;
        skipWhitespaceAndComments();
        const int start = m_pos;
        if (peek() != QLatin1Char('/')) {
            return false;
        }
        ++m_pos;
        QString directive;
        TextRange range;
        if (!parseIdentifier(&directive, &range) || directive != QStringLiteral("external")) {
            m_pos = start;
            return false;
        }
        *recognized = true;
        if (!consumeExpected(QLatin1Char('('))) {
            setError(result, QStringLiteral("Expected '(' after /external"), m_pos, m_pos);
            return false;
        }
        QString role;
        TextRange roleRange;
        if (!parseIdentifier(&role, &roleRange) || !consumeExpected(QLatin1Char(')'))) {
            setError(result, QStringLiteral("Expected role name in /external(role)"), m_pos, m_pos);
            return false;
        }
        if (!result.graph.addExternalRole(role)) {
            setError(result, QStringLiteral("Duplicate external role '%1'").arg(role),
                     roleRange.start, roleRange.end);
            return false;
        }
        skipWhitespaceAndComments();
        if (peek() == QLatin1Char(';')) {
            consumeSemicolon();
        }
        return true;
    }

    bool parseOutform(ParseResult& result) {
        if (!consumeKeyword(QStringLiteral("outform"))) {
            return false;
        }
        QString name;
        TextRange nameRange;
        if (!parseIdentifier(&name, &nameRange)) {
            setError(result, QStringLiteral("Expected outform name"), m_pos, m_pos);
            return false;
        }
        QString modeName;
        TextRange modeRange;
        if (!parseIdentifier(&modeName, &modeRange)) {
            setError(result, QStringLiteral("Expected outform mode 'text' or 'binary'"),
                     m_pos, m_pos);
            return false;
        }
        OutformMode mode;
        if (modeName == QStringLiteral("text")) {
            mode = OutformMode::Text;
        } else if (modeName == QStringLiteral("binary")) {
            mode = OutformMode::Binary;
        } else {
            setError(result,
                     QStringLiteral("Unknown outform mode '%1'; expected 'text' or 'binary'")
                         .arg(modeName),
                     modeRange.start, modeRange.end);
            return false;
        }
        skipWhitespaceAndComments();
        if (peek() != QLatin1Char('{')) {
            setError(result, QStringLiteral("Expected '{' before outform template body"),
                     m_pos, m_pos);
            return false;
        }
        ++m_pos;
        QString templateText;
        int braceDepth = 0;
        bool closed = false;
        while (!atEnd()) {
            if (peekAhead(QStringLiteral("{{"))) {
                const int placeholderEnd = m_text.indexOf(QStringLiteral("}}"), m_pos + 2);
                if (placeholderEnd < 0) {
                    setError(result, QStringLiteral("Unclosed template placeholder"),
                             m_pos, m_text.size());
                    return false;
                }
                templateText += m_text.mid(m_pos, placeholderEnd + 2 - m_pos);
                m_pos = placeholderEnd + 2;
                continue;
            }
            const QChar ch = consume();
            if (ch == QLatin1Char('\\') && !atEnd() &&
                (peek() == QLatin1Char('{') || peek() == QLatin1Char('}'))) {
                templateText += consume();
                continue;
            }
            if (ch == QLatin1Char('{')) {
                ++braceDepth;
                templateText += ch;
            } else if (ch == QLatin1Char('}') && braceDepth > 0) {
                --braceDepth;
                templateText += ch;
            } else if (ch == QLatin1Char('}')) {
                closed = true;
                break;
            } else {
                templateText += ch;
            }
        }
        if (!closed) {
            setError(result, QStringLiteral("Unclosed outform template body"),
                     nameRange.start, m_text.size());
            return false;
        }
        skipWhitespaceAndComments();
        if (peek() == QLatin1Char(';')) {
            consumeSemicolon();
        }
        const OutformNode node{name, mode, templateText, nameRange,
                               m_currentSourceFilePath};
        if (!result.graph.addOutform(node)) {
            setError(result, QStringLiteral("Duplicate outform name '%1'").arg(name),
                     nameRange.start, nameRange.end);
            return false;
        }
        return true;
    }

    bool parseSourceFileMarker(ParseResult& result, bool* recognized) {
        *recognized = false;
        skipWhitespaceAndComments();
        const int start = m_pos;
        if (peek() != QLatin1Char('/')) {
            return false;
        }
        ++m_pos;
        QString directive;
        if (!parseIdentifier(&directive) ||
            directive != QStringLiteral("__breco_source_file")) {
            m_pos = start;
            return false;
        }
        *recognized = true;
        if (!consumeExpected(QLatin1Char('('))) {
            setError(result, QStringLiteral("Invalid internal source marker"), m_pos, m_pos);
            return false;
        }
        QString encoded;
        if (!parseIdentifier(&encoded) || !encoded.startsWith(QLatin1Char('p')) ||
            !consumeExpected(QLatin1Char(')')) || !consumeSemicolon()) {
            setError(result, QStringLiteral("Invalid internal source marker"), m_pos, m_pos);
            return false;
        }
        m_currentSourceFilePath =
            QString::fromUtf8(QByteArray::fromHex(encoded.mid(1).toLatin1()));
        return true;
    }

    bool parseTopLevelDeclaration(ParseResult& result) {
        skipWhitespaceAndComments();
        if (atEnd()) {
            return true;
        }
        const int saved = m_pos;
        bool recognizedSourceMarker = false;
        if (parseSourceFileMarker(result, &recognizedSourceMarker)) {
            return true;
        }
        if (recognizedSourceMarker) {
            return false;
        }
        m_pos = saved;
        bool recognizedDefault = false;
        if (parseDefaultDirective(result, &recognizedDefault)) {
            return true;
        }
        if (recognizedDefault) {
            return false;
        }
        m_pos = saved;
        bool recognizedExternal = false;
        if (parseExternalDirective(result, &recognizedExternal)) {
            return true;
        }
        if (recognizedExternal) {
            return false;
        }
        m_pos = saved;
        QString bestError;
        TextRange bestErrorRange;
        const auto rememberBestError = [&]() {
            if (!result.errorMessage.isEmpty() &&
                (bestError.isEmpty() || result.errorRange.start >= bestErrorRange.start)) {
                bestError = result.errorMessage;
                bestErrorRange = result.errorRange;
            }
        };
        if (parseOutform(result)) {
            return true;
        }
        rememberBestError();
        m_pos = saved;
        if (parseTypedef(result)) {
            return true;
        }
        rememberBestError();
        m_pos = saved;
        if (parseStruct(result)) {
            return true;
        }
        rememberBestError();
        m_pos = saved;
        if (parseStandaloneMember(result)) {
            return true;
        }
        rememberBestError();
        if (bestError.isEmpty()) {
            setError(result, QStringLiteral("Unexpected token at position %1").arg(m_pos),
                     m_pos, m_pos);
        } else {
            result.errorMessage = bestError;
            result.errorRange = bestErrorRange;
        }
        return false;
    }

    static bool isIdentifierStart(QChar ch) {
        return ch.isLetter() || ch == QLatin1Char('_');
    }

    static bool isIdentifierPart(QChar ch) {
        return ch.isLetterOrNumber() || ch == QLatin1Char('_');
    }

    const QString& m_text;
    int m_pos = 0;
    TextRange m_defaultEntryRange;
    QString m_currentSourceFilePath;
};

}  // namespace

ParseResult parseStructDeclaration(const QString& text) {
    Parser parser(text);
    return parser.parse();
}

namespace {

QString sourceFileMarker(const QString& filePath) {
    return QStringLiteral("/__breco_source_file(p%1);\n")
        .arg(QString::fromLatin1(filePath.toUtf8().toHex()));
}

bool expandIncludes(const QString& filePath, QString* output, QString* error,
                    QSet<QString>* active, QSet<QString>* loaded) {
    const QString canonical = QFileInfo(filePath).canonicalFilePath();
    const QString resolved = canonical.isEmpty() ? QFileInfo(filePath).absoluteFilePath() : canonical;
    if (active->contains(resolved)) {
        *error = QStringLiteral("Cyclic structure include involving '%1'").arg(resolved);
        return false;
    }
    if (loaded->contains(resolved)) {
        return true;
    }
    QFile file(resolved);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = QStringLiteral("Could not read structure definition '%1': %2")
                     .arg(resolved, file.errorString());
        return false;
    }
    active->insert(resolved);
    QString text = QString::fromUtf8(file.readAll());
    const QRegularExpression includePattern(
        QStringLiteral(R"breco((?:^|\n)\s*(?:#include|include|/include)\s*[\(]?\s*"([^"]+)"\s*[\)]?\s*;?)breco"));
    qsizetype searchFrom = 0;
    QString expanded;
    auto match = includePattern.match(text, searchFrom);
    while (match.hasMatch()) {
        expanded += text.mid(searchFrom, match.capturedStart() - searchFrom);
        const QString included = QDir(QFileInfo(resolved).absolutePath())
                                     .absoluteFilePath(match.captured(1));
        QString includedText;
        if (!expandIncludes(included, &includedText, error, active, loaded)) {
            return false;
        }
        expanded += sourceFileMarker(QFileInfo(included).absoluteFilePath());
        expanded += includedText;
        expanded += QLatin1Char('\n');
        expanded += sourceFileMarker(resolved);
        searchFrom = match.capturedEnd();
        match = includePattern.match(text, searchFrom);
    }
    expanded += text.mid(searchFrom);
    active->remove(resolved);
    loaded->insert(resolved);
    *output += expanded;
    return true;
}

}  // namespace

ParseResult parseStructDeclarationFile(const QString& filePath) {
    QString text;
    QString error;
    QSet<QString> active;
    QSet<QString> loaded;
    if (!expandIncludes(filePath, &text, &error, &active, &loaded)) {
        ParseResult result;
        result.errorMessage = error;
        return result;
    }
    const QString canonical = QFileInfo(filePath).canonicalFilePath();
    const QString resolved = canonical.isEmpty()
                                 ? QFileInfo(filePath).absoluteFilePath()
                                 : canonical;
    Parser parser(text, resolved);
    return parser.parse();
}

}  // namespace breco
